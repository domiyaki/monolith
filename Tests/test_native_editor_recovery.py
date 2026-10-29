"""Native stdio + Windows process integration tests, using a disposable editor fixture.

Set MONOLITH_NATIVE_PROXY_EXE and MONOLITH_RECOVERY_FIXTURE_EXE to separately
built binaries. MONOLITH_RECOVERY_TEST_ROOT controls all temporary output.
The real editor is inspected by process enumeration but is never a test target.
"""
import http.server
import json
import os
from pathlib import Path
import queue
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'Scripts'))
from monolith_editor_recovery import NativeWindows


class Proxy:
    def __init__(self, exe, env):
        self.process = subprocess.Popen([str(exe)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                        text=True, encoding='utf-8', env=env)
        self.lines = queue.Queue()
        self.serial = 0
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(json.loads(line))

    def rpc(self, method, params=None):
        self.serial += 1
        self.process.stdin.write(json.dumps(dict(jsonrpc='2.0', id=self.serial,
                                                method=method, params=params or {})) + '\n')
        self.process.stdin.flush()
        while True:
            message = self.lines.get(timeout=15)
            if message.get('id') == self.serial:
                return message

    def recovery(self, action='status'):
        response = self.rpc('tools/call', {'name': 'monolith_recovery',
                                         'arguments': {'action': action}})
        assert not response['result'].get('isError'), response
        return json.loads(response['result']['content'][0]['text'])

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=5)
        self.process.stdout.close()


@unittest.skipUnless(os.name == 'nt' and os.environ.get('MONOLITH_NATIVE_PROXY_EXE')
                     and os.environ.get('MONOLITH_RECOVERY_FIXTURE_EXE'),
                     'Requires separately built Windows proxy and process fixture')
class NativeRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ.get('MONOLITH_RECOVERY_TEST_ROOT'))
        self.root = Path(self.temp.name)
        self.plugin = self.root / 'Fixture Project' / 'Plugins' / 'Monolith'
        (self.plugin / 'Config').mkdir(parents=True)
        (self.plugin / 'Monolith.uplugin').write_text('{}')
        (self.plugin / 'Config/EditorRecovery.json').write_text('{"auto_recover":true}')
        self.project = self.plugin.parents[1] / 'Fixture.uproject'
        self.project.write_text('{}')
        self.exe = self.root / 'UnrealEditor.exe'
        shutil.copyfile(os.environ['MONOLITH_RECOVERY_FIXTURE_EXE'], self.exe)
        self.launch_log = self.root / 'launches.txt'
        self.env = dict(os.environ, MONOLITH_PLUGIN_DIR=str(self.plugin),
                        MONOLITH_PROJECT_FILE=str(self.project), MONOLITH_EDITOR_EXE=str(self.exe),
                        MONOLITH_FIXTURE_LAUNCH_LOG=str(self.launch_log), MONOLITH_CALL_LOG='0',
                        LOCALAPPDATA=str(self.root), MONOLITH_URL='http://127.0.0.1:1/mcp')
        self.env.pop('MONOLITH_AUTO_RECOVER_EDITOR', None)
        self.proxies = []
        self.originals = []

    def proxy(self):
        proxy = Proxy(os.environ['MONOLITH_NATIVE_PROXY_EXE'], self.env)
        self.proxies.append(proxy)
        return proxy

    def tearDown(self):
        for proxy in self.proxies:
            proxy.close()
        native = NativeWindows()
        for item in native.processes(str(self.project), str(self.exe)):
            native.k.TerminateProcess(item['handle'], 0)
            native.k.WaitForSingleObject(item['handle'], 5000)
            native.close(item['handle'])
        for process in self.originals:
            process.wait(timeout=5)
        self.temp.cleanup()

    def test_offline_seed_cached_and_local_status(self):
        proxy = self.proxy()
        self.assertEqual(proxy.recovery()['state'], 'not_running')
        tools = proxy.rpc('tools/list')['result']['tools']
        self.assertEqual(sum(t['name'] == 'monolith_recovery' for t in tools), 1)
        cache = self.root / 'Monolith/monolith_proxy_tools_127_0_0_1_1.json'
        cache.parent.mkdir(exist_ok=True)
        cache.write_text('[{"name":"cached_tool"}]')
        tools = proxy.rpc('tools/list')['result']['tools']
        self.assertEqual({t['name'] for t in tools}, {'cached_tool', 'monolith_recovery'})
        self.assertFalse(self.launch_log.exists())

    def test_online_list_also_contains_local_tool(self):
        class Server(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                payload = json.dumps(dict(jsonrpc='2.0', id=body['id'], result={'tools': [{'name': 'live_tool'}]})).encode()
                self.send_response(200); self.end_headers(); self.wfile.write(payload)
            def log_message(self, *args): pass
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Server)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            self.env['MONOLITH_URL'] = f'http://127.0.0.1:{server.server_port}/mcp'
            names = {t['name'] for t in self.proxy().rpc('tools/list')['result']['tools']}
            self.assertEqual(names, {'live_tool', 'monolith_recovery'})
        finally:
            server.shutdown(); server.server_close()

    def test_observed_crash_restarts_once_across_two_proxies(self):
        original = subprocess.Popen([str(self.exe), str(self.project), '--fixture-crash'], env=self.env)
        self.originals.append(original)
        first, second = self.proxy(), self.proxy()
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and not self.launch_log.exists():
            time.sleep(.25)
        self.assertTrue(self.launch_log.exists(), 'Abnormal exit was not recovered')
        lines = self.launch_log.read_text().splitlines()
        self.assertEqual(len(lines), 1)
        self.assertIn('skip_restore', lines[0])
        restarted = first.recovery()
        self.assertNotIn(original.pid, restarted['editor_pids'])
        self.assertEqual(len(restarted['editor_pids']), 1)
        second.recovery('recover')
        self.assertEqual(len(self.launch_log.read_text().splitlines()), 1)

    def test_observed_clean_exit_does_not_restart(self):
        original = subprocess.Popen([str(self.exe), str(self.project), '--fixture-clean'], env=self.env)
        self.originals.append(original)
        proxy = self.proxy()
        original.wait(timeout=10)
        time.sleep(5)
        self.assertEqual(proxy.recovery()['state'], 'not_running')
        self.assertFalse(self.launch_log.exists())


if __name__ == '__main__':
    unittest.main()
