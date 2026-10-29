"""Offline native WinHTTP tests; requires MONOLITH_TRANSPORT_HARNESS executable.

Build transport_harness.cpp into E:\\CodexTrashCan, never the deployed Binaries.
"""
import json
import os
import subprocess
import threading
import time
import unittest
from http.server import ThreadingHTTPServer
from test_python_transport import Handler


class NativeHandler(Handler):
    def send(self, *args, **kwargs):
        delay = getattr(self.server, 'extra_delay', 0)
        if self.command == 'POST' and delay:
            time.sleep(delay)
        return super().send(*args, **kwargs)


class NativeTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(('127.0.0.1', 0), NativeHandler)
        cls.server.daemon_threads = True
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def setUp(self):
        self.server.posts = []
        self.server.gets = []
        self.server.slow_started = threading.Event()
        self.server.extra_delay = 0
        self.process = subprocess.Popen([os.environ['MONOLITH_TRANSPORT_HARNESS'],
            f'http://127.0.0.1:{self.server.server_port}/mcp'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding='utf-8')

    def tearDown(self):
        self.process.stdin.close()
        self.process.wait(timeout=5)
        self.process.stdout.close()
        self.process.stderr.close()

    def call(self, mode='ok', **options):
        command = {'request': {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
            'params': {'mode': mode}}, **options}
        self.process.stdin.write(json.dumps(command) + '\n')
        self.process.stdin.flush()
        return json.loads(self.process.stdout.readline())

    def test_connection_reuse(self):
        for _ in range(8):
            self.assertTrue(self.call()['response'])
        self.assertEqual(len({client for _, client in self.server.posts}), 1)

    def test_http_errors_and_redirect_not_replayed(self):
        for mode in ('http_error', 'redirect'):
            with self.subTest(mode=mode):
                before = len(self.server.posts)
                result = self.call(mode)
                self.assertFalse(result['response'])
                self.assertIn('HTTP status', result['error'])
                self.assertEqual(len(self.server.posts), before + 1)
                self.assertTrue(self.call()['response'])
        self.assertEqual(self.server.gets, [])

    def test_bad_responses(self):
        for mode in ('malformed', 'bad_utf8', 'wrong_id', 'null_result', 'both', 'bad_error', 'truncated'):
            with self.subTest(mode=mode):
                result = self.call(mode)
                self.assertFalse(result['response'])
                self.assertTrue(result['error'])
                self.assertTrue(self.call()['response'])

    def test_disconnect_no_replay(self):
        self.assertFalse(self.call('disconnect')['response'])
        self.assertEqual(len(self.server.posts), 1)
        self.assertTrue(self.call()['response'])

    def test_timeout_no_replay(self):
        # WinHTTP may round short receive waits; use a delay beyond that floor.
        self.server.extra_delay = 6.0
        result = self.call('slow', timeout=0.2)
        self.assertFalse(result['response'])
        self.assertIn('12002', result['error'])
        self.assertEqual(len(self.server.posts), 1)
        self.server.extra_delay = 0
        self.assertTrue(self.call()['response'])
        time.sleep(2.5)  # The timed-out server handler finishes independently.

    def test_valid_rpc_error_passthrough(self):
        result = self.call('rpc_error')
        self.assertFalse(result['error'])
        self.assertEqual(json.loads(result['response'])['error']['code'], -32602)

    def test_notification_and_unknown_method(self):
        result = self.call(request={'jsonrpc': '2.0', 'method': 'notifications/test'})
        self.assertFalse(result['response'])
        self.assertFalse(result['error'])
        result = self.call(request={'jsonrpc': '2.0', 'id': 1, 'method': 'custom/test',
            'params': {'mode': 'null_result'}})
        self.assertIsNone(json.loads(result['response'])['result'])

    def test_error_headers_do_not_wait_for_stalled_body(self):
        result = self.call('stalled_error', timeout=0.04)
        self.assertIn('HTTP status 503', result['error'])
        self.assertFalse(result['response'])
        self.assertTrue(self.call()['response'])
        time.sleep(0.3)

    def test_health_independent_from_slow_post(self):
        result = self.call('slow', concurrent_health=True)
        self.assertTrue(result['healthy'])
        self.assertTrue(result['response'])
        self.assertLess(result['health_ms'], 200)
        self.assertNotEqual(self.server.posts[0][1], self.server.gets[0][1])


if __name__ == '__main__':
    unittest.main()
