"""Run candidate proxies only, with recovery disabled and a fake HTTP endpoint."""
import json
import os
import subprocess
import sys
import threading
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path
from test_python_transport import Handler, PLUGIN


class StdioHandler(Handler):
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.posts.append(request)
        if request['method'] == 'tools/list':
            self.send(200, json.dumps({'jsonrpc': '2.0', 'id': request['id'],
                'result': {'tools': []}}).encode())
        else:
            self.send(200, b'invalid-json')


class ProxyStdioTests(unittest.TestCase):
    def test_native_and_python_protocol_survive_transport_errors(self):
        for name, command in (
            ('native', [os.environ['MONOLITH_PROXY_CANDIDATE']]),
            ('python', [sys.executable, '-B', str(PLUGIN / 'Scripts/monolith_proxy.py')]),
        ):
            with self.subTest(proxy=name):
                server = ThreadingHTTPServer(('127.0.0.1', 0), StdioHandler)
                server.daemon_threads = True
                server.posts = []
                server.gets = []
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                root = Path(os.environ['MONOLITH_TEST_ARTIFACTS']) / name
                root.mkdir(parents=True, exist_ok=True)
                env = dict(os.environ, MONOLITH_URL=f'http://127.0.0.1:{server.server_port}/mcp',
                    MONOLITH_AUTO_RECOVER_EDITOR='0', MONOLITH_CALL_LOG='0',
                    MONOLITH_PROJECT_ROOT=str(root), LOCALAPPDATA=str(root), TEMP=str(root))
                process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True, encoding='utf-8', env=env, cwd=root)
                def call(request):
                    process.stdin.write(json.dumps(request) + '\n')
                    process.stdin.flush()
                    while True:
                        line = process.stdout.readline()
                        self.assertTrue(line, 'Proxy exited before its response')
                        response = json.loads(line)
                        if 'id' in response:
                            self.assertEqual(response['id'], request['id'])
                            return response
                try:
                    call({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {}})
                    result = call({'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'})
                    self.assertIn('tools', result['result'])
                    self.assertEqual(server.gets, [], 'tools/list should not require a health preflight')
                    result = call({'jsonrpc': '2.0', 'id': 3, 'method': 'tools/call',
                        'params': {'name': 'material_query', 'arguments': {'action': 'get'}}})
                    self.assertTrue(result['result']['isError'])
                    result = call({'jsonrpc': '2.0', 'id': 4, 'method': 'custom/test'})
                    self.assertEqual(result['error']['code'], -32000)
                    self.assertIn('unknown', result['error']['message'])
                    self.assertEqual(call({'jsonrpc': '2.0', 'id': 5, 'method': 'ping'})['result'], {})
                    self.assertEqual(len(server.posts), 3, 'No automatic replay')
                finally:
                    process.stdin.close()
                    process.wait(timeout=5)
                    process.stdout.close()
                    (root / 'stderr.txt').write_text(process.stderr.read(), encoding='utf-8')
                    process.stderr.close()
                    server.shutdown()
                    server.server_close()
                    thread.join()


if __name__ == '__main__':
    unittest.main()
