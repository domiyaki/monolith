"""Stdio discovery contract for both proxies; fake HTTP only, recovery disabled.

Set MONOLITH_PROXY_CANDIDATE and MONOLITH_TEST_ARTIFACTS (outside the project).
No running Unreal Editor is contacted or changed.
"""
import contextlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PLUGIN = Path(__file__).resolve().parents[3]


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_args):
        pass

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, ConnectionAbortedError):
            pass

    def send(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def do_GET(self):
        self.server.get_count += 1
        self.server.health_seen.set()
        time.sleep(self.server.health_delay)
        self.send(200 if self.server.healthy else 503, {'healthy': self.server.healthy})

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.posts.append(request)
        response = {'jsonrpc': '2.0', 'id': request['id']}
        if self.server.mode == 'error':
            response['error'] = {'code': -32000, 'message': 'fixture error'}
        elif self.server.mode == 'malformed':
            response['result'] = {'tools': 'not an array'}
        else:
            response['result'] = {'tools': [{'name': 'fixture_tool', 'description': 'fixture',
                                           'inputSchema': {'type': 'object', 'properties': {}}}]}
            if self.server.mode == 'paginated':
                response['result']['nextCursor'] = 'next-page'
        self.send(200, response)


class Client:
    def __init__(self, command, root, endpoint, ttl):
        root.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, MONOLITH_URL=endpoint, MONOLITH_AUTO_RECOVER_EDITOR='0',
                   MONOLITH_CALL_LOG='0', MONOLITH_PROJECT_ROOT=str(root),
                   MONOLITH_TOOLS_CACHE_TTL_SECONDS=str(ttl), LOCALAPPDATA=str(root), TEMP=str(root))
        self.stderr = (root / 'stderr.txt').open('w', encoding='utf-8')
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.stderr, text=True, encoding='utf-8', env=env, cwd=root)
        self.messages = queue.Queue()
        self.notifications = []
        self.sequence = 0
        def read():
            for line in self.process.stdout:
                self.messages.put(json.loads(line))
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()
        self.call('initialize', {})

    def notify_initialized(self):
        self.process.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n')
        self.process.stdin.flush()

    def call(self, method, params=None):
        self.sequence += 1
        request = {'jsonrpc': '2.0', 'id': self.sequence, 'method': method}
        if params is not None:
            request['params'] = params
        self.process.stdin.write(json.dumps(request) + '\n')
        self.process.stdin.flush()
        deadline = time.monotonic() + 10
        while True:
            response = self.messages.get(timeout=max(0.01, deadline - time.monotonic()))
            if 'id' not in response:
                self.notifications.append(response)
                continue
            if response['id'] != self.sequence:
                raise AssertionError(('Response ID mismatch', response, self.sequence))
            return response

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        self.reader.join(timeout=2)
        self.process.stdout.close()
        self.stderr.close()


class DiscoveryTests(unittest.TestCase):
    def fixtures(self):
        return [('native', [os.environ['MONOLITH_PROXY_CANDIDATE']]),
                ('python', [sys.executable, '-B', str(PLUGIN / 'Scripts/monolith_proxy.py')])]

    @contextlib.contextmanager
    def fixture(self, name, command, ttl=5):
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.daemon_threads = True
        server.posts = []
        server.get_count = 0
        server.health_seen = threading.Event()
        server.health_delay = 0
        server.healthy = True
        server.mode = 'success'
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        root = Path(os.environ['MONOLITH_TEST_ARTIFACTS']) / self._testMethodName / name
        client = Client(command, root, f'http://127.0.0.1:{server.server_port}/mcp', ttl)
        try:
            yield client, server
        finally:
            client.close()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def test_cache_hit_preserves_new_request_id_and_tools(self):
        for name, command in self.fixtures():
            with self.subTest(proxy=name), self.fixture(name, command) as (client, server):
                first = client.call('tools/list')
                second = client.call('tools/list', {})
                self.assertNotEqual(first['id'], second['id'])
                self.assertEqual(first['result'], second['result'])
                self.assertEqual(len(server.posts), 1)
                self.assertIn('fixture_tool', [t['name'] for t in second['result']['tools']])

    def test_ttl_zero_disables_cache(self):
        for name, command in self.fixtures():
            with self.subTest(proxy=name), self.fixture(name, command, ttl=0) as (client, server):
                client.call('tools/list'); client.call('tools/list')
                self.assertEqual(len(server.posts), 2)

    def test_expired_cache_refreshes(self):
        for name, command in self.fixtures():
            with self.subTest(proxy=name), self.fixture(name, command, ttl=0.05) as (client, server):
                client.call('tools/list')
                time.sleep(0.1)
                client.call('tools/list')
                self.assertEqual(len(server.posts), 2)

    def test_nonempty_params_bypass_cached_default_page(self):
        for name, command in self.fixtures():
            with self.subTest(proxy=name), self.fixture(name, command) as (client, server):
                client.call('tools/list')
                client.call('tools/list', {'cursor': 'page-two'})
                client.call('tools/list', {'cursor': 'page-two'})
                client.call('tools/list')
                self.assertEqual(len(server.posts), 3)
                self.assertEqual(server.posts[1]['params']['cursor'], 'page-two')

    def test_error_response_is_not_cached(self):
        for name, command in self.fixtures():
            with self.subTest(proxy=name), self.fixture(name, command) as (client, server):
                server.mode = 'error'
                self.assertIn('error', client.call('tools/list'))
                server.mode = 'success'
                self.assertIn('result', client.call('tools/list'))
                self.assertEqual(len(server.posts), 2)

    def test_malformed_tools_response_is_not_cached(self):
        for name, command in self.fixtures():
            with self.subTest(proxy=name), self.fixture(name, command) as (client, server):
                server.mode = 'malformed'
                client.call('tools/list')
                server.mode = 'success'
                result = client.call('tools/list')
                self.assertIsInstance(result['result']['tools'], list)
                self.assertEqual(len(server.posts), 2)

    def test_paginated_result_is_not_cached(self):
        for name, command in self.fixtures():
            with self.subTest(proxy=name), self.fixture(name, command) as (client, server):
                server.mode = 'paginated'
                client.call('tools/list'); client.call('tools/list')
                self.assertEqual(len(server.posts), 2)

    def test_initialized_notification_does_not_block_on_health(self):
        for name, command in self.fixtures():
            with self.subTest(proxy=name), self.fixture(name, command) as (client, server):
                server.health_delay = 2.5
                started = time.monotonic()
                client.notify_initialized()
                self.assertEqual(client.call('ping')['result'], {})
                self.assertIn('result', client.call('tools/list'))
                self.assertLess(time.monotonic() - started, 1.5,
                                'Foreground initialized must not wait for a 2.5s health request')

    def test_health_transition_invalidates_cache_and_notifies(self):
        for name, command in self.fixtures():
            with self.subTest(proxy=name), self.fixture(name, command, ttl=60) as (client, server):
                self.assertTrue(server.health_seen.wait(timeout=6), 'Initial background health request')
                time.sleep(0.15)  # allow response handling to publish the initial observation
                client.call('tools/list')
                client.call('tools/list')
                self.assertEqual(len(server.posts), 1)
                server.healthy = False
                server.health_seen.clear()
                self.assertTrue(server.health_seen.wait(timeout=7), 'Next background health request')
                time.sleep(0.15)
                client.call('tools/list')
                self.assertEqual(len(server.posts), 2)
                self.assertTrue(any(n.get('method') == 'notifications/tools/list_changed'
                                    for n in client.notifications))


if __name__ == '__main__':
    unittest.main()
