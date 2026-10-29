"""Offline transport tests. No UE process, recovery action, or project asset access."""
import concurrent.futures
import importlib.util
import json
import socket
import sys
import threading
import time
import types
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest.mock import patch


PLUGIN = Path(__file__).resolve().parents[3]
RECOVERY_STUB = types.SimpleNamespace(RECOVERY=None, append_tool=lambda tools: tools)
with patch.dict(sys.modules, {"monolith_editor_recovery": RECOVERY_STUB}):
    spec = importlib.util.spec_from_file_location("proxy_transport_fixture", PLUGIN / "Scripts" / "monolith_proxy.py")
    proxy = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(proxy)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, ConnectionAbortedError):
            # Expected when the proxy discards a timed-out channel on Windows.
            pass

    def send(self, status, payload, extra=0, headers=None):
        self.send_response(status)
        self.send_header("Content-Length", str(len(payload) + extra))
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def do_GET(self):
        self.server.gets.append((self.path, self.client_address))
        self.send(200, b'{"healthy":true}')

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        mode = request.get("params", {}).get("mode", "ok")
        self.server.posts.append((mode, self.client_address))
        if "id" not in request:
            self.send(204, b"")
            return
        response = {"jsonrpc": "2.0", "id": request["id"], "result": {"content": []}}
        if mode == "disconnect":
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return
        if mode == "slow":
            self.server.slow_started.set()
            time.sleep(0.25)
        if mode == "http_error":
            self.send(503, b"unavailable")
            return
        if mode == "stalled_error":
            self.send_response(503)
            self.send_header("Content-Length", "100")
            self.end_headers()
            time.sleep(0.25)
            self.close_connection = True
            return
        if mode == "redirect":
            self.send(302, b"redirect", headers={"Location": "/redirect-target"})
            return
        if mode == "malformed":
            self.send(200, b"not json")
            return
        if mode == "bad_utf8":
            self.send(200, b"\xff")
            return
        if mode == "wrong_id":
            response["id"] = "different"
        if mode == "null_result":
            response["result"] = None
        if mode == "both":
            response["error"] = {"code": -1, "message": "failure"}
        if mode in ("rpc_error", "bad_error"):
            response.pop("result")
            response["error"] = {"code": -32602, "message": "invalid params"} if mode == "rpc_error" else "bad"
        data = json.dumps(response).encode()
        if mode == "truncated":
            self.send(200, data, extra=20)
            self.close_connection = True
            return
        self.send(200, data)


class PythonTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.server.daemon_threads = True
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        proxy.MONOLITH_URL = f"http://127.0.0.1:{cls.server.server_port}/mcp"
        proxy.MONOLITH_HEALTH = f"http://127.0.0.1:{cls.server.server_port}/health"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def setUp(self):
        self.server.posts = []
        self.server.gets = []
        self.server.slow_started = threading.Event()

    def tearDown(self):
        proxy._close_http_channel("rpc")
        proxy._close_http_channel("health")

    @staticmethod
    def request(mode="ok", request_id=1):
        return {"jsonrpc": "2.0", "id": request_id, "method": "tools/call", "params": {"name": "test", "mode": mode}}

    def call(self, mode="ok", timeout=2):
        return proxy._post_monolith(json.dumps(self.request(mode)), timeout=timeout)

    def test_reuses_connection_and_consumes_full_responses(self):
        for _ in range(4):
            self.assertIsNotNone(self.call())
        self.assertEqual(len({client for _, client in self.server.posts}), 1)

    def test_health_has_separate_reusable_channel(self):
        self.assertIsNotNone(self.call())
        self.assertTrue(proxy._check_monolith_up())
        self.assertTrue(proxy._check_monolith_up())
        self.assertIsNotNone(self.call())
        self.assertEqual(self.server.posts[0][1], self.server.posts[1][1])
        self.assertEqual(self.server.gets[0][1], self.server.gets[1][1])
        self.assertNotEqual(self.server.posts[0][1], self.server.gets[0][1])

    def test_http_errors_and_redirect_are_not_replayed(self):
        for mode in ("http_error", "redirect"):
            with self.subTest(mode=mode):
                before = len(self.server.posts)
                self.assertIsNone(self.call(mode))
                self.assertEqual(len(self.server.posts), before + 1)
                self.assertIn("HTTP status", proxy._transport.post_error)
                self.assertIsNotNone(self.call())
        self.assertEqual(self.server.gets, [])

    def test_invalid_responses_rejected_and_next_request_works(self):
        for mode in ("malformed", "bad_utf8", "wrong_id", "null_result", "both", "bad_error", "truncated"):
            with self.subTest(mode=mode):
                self.assertIsNone(self.call(mode))
                self.assertTrue(proxy._transport.post_error)
                self.assertIsNotNone(self.call())

    def test_disconnect_is_not_replayed(self):
        self.assertIsNone(self.call("disconnect"))
        self.assertEqual(len(self.server.posts), 1)
        self.assertIsNotNone(self.call())

    def test_timeout_is_not_replayed(self):
        self.assertIsNone(self.call("slow", timeout=0.04))
        self.assertEqual(len(self.server.posts), 1)
        self.assertIsNotNone(self.call())
        time.sleep(0.3)  # Let the timed-out fake handler finish before the next test.

    def test_jsonrpc_error_remains_a_protocol_response(self):
        result = json.loads(self.call("rpc_error"))
        self.assertEqual(result["error"]["code"], -32602)

    def test_notification_accepts_empty_204_without_error(self):
        request = {"jsonrpc": "2.0", "method": "notifications/test"}
        self.assertIsNone(proxy._post_monolith(json.dumps(request)))
        self.assertEqual(proxy._transport.post_error, "")
        self.assertIsNotNone(self.call())
        self.assertEqual(self.server.posts[0][1], self.server.posts[1][1])

    def test_unknown_method_may_return_null_result(self):
        request = self.request("null_result")
        request["method"] = "custom/method"
        response = json.loads(proxy._post_monolith(json.dumps(request)))
        self.assertIsNone(response["result"])

    def test_error_headers_do_not_wait_for_stalled_body(self):
        self.assertIsNone(self.call("stalled_error", timeout=0.04))
        # Reading the body would time out; header rejection preserves HTTP status.
        self.assertIn("HTTP status 503", proxy._transport.post_error)
        self.assertNotIn("TimeoutError", proxy._transport.post_error)
        self.assertIsNotNone(self.call())
        time.sleep(0.3)

    def test_tool_error_reports_unknown_outcome(self):
        result = json.loads(proxy.handle_tools_call(self.request("disconnect")))
        self.assertTrue(result["result"]["isError"])
        message = result["result"]["content"][0]["text"]
        self.assertIn("outcome may be unknown", message)
        self.assertIn("did not replay", message)

    def test_slow_rpc_does_not_block_health_thread(self):
        def run_slow():
            try:
                return self.call("slow")
            finally:
                proxy._close_http_channel("rpc")
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(run_slow)
            self.assertTrue(self.server.slow_started.wait(1))
            self.assertTrue(proxy._check_monolith_up())
            self.assertFalse(future.done(), "Health must finish while slow RPC is still pending")
            self.assertIsNotNone(future.result(timeout=2))


if __name__ == "__main__":
    unittest.main()
