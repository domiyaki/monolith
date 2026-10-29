"""Offline discovery cache/startup tests; no real editor, files, or recovery."""
import importlib.util
import io
import json
import os
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

PLUGIN = Path(__file__).resolve().parents[3]
RECOVERY = types.SimpleNamespace(tick=Mock())
with patch.dict(sys.modules, {"monolith_editor_recovery": types.SimpleNamespace(
        RECOVERY=RECOVERY, append_tool=lambda tools: tools)}):
    spec = importlib.util.spec_from_file_location("proxy_discovery_fixture", PLUGIN / "Scripts/monolith_proxy.py")
    proxy = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(proxy)


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        proxy._tools_generation = 0
        proxy._tools_memory_generation = -1
        proxy._tools_memory_result = None
        proxy._tools_memory_expires = 0
        proxy._monolith_was_up = None
        self.now = 100.0
        self.posts = []
        self.result = {"tools": [{"name": "test", "inputSchema": {"type": "object"}}]}
        self.stack = []
        for name, value in [("TOOLS_CACHE_TTL_SECONDS", 5.0),
                            ("_write_tools_cache", Mock()),
                            ("_write_call_log_line", Mock()),
                            ("_post_monolith", self.post)]:
            p = patch.object(proxy, name, value); p.start(); self.stack.append(p)
        p = patch.object(proxy.time, "monotonic", lambda: self.now); p.start(); self.stack.append(p)

    def tearDown(self):
        for p in reversed(self.stack): p.stop()

    def post(self, request):
        msg = json.loads(request); self.posts.append(msg)
        return json.dumps({"jsonrpc": "2.0", "id": msg["id"], "result": self.result})

    def call(self, id, params=None):
        msg = {"jsonrpc": "2.0", "method": "tools/list", "id": id}
        if params is not None: msg["params"] = params
        return json.loads(proxy.handle_tools_list(msg))

    def test_hit_rewrites_id_and_preserves_result(self):
        self.assertEqual(self.call(1)["result"], self.call("second", {})["result"])
        self.assertEqual(self.call("third")["id"], "third")
        self.assertEqual(len(self.posts), 1)
        proxy._write_tools_cache.assert_called_once()

    def test_expiry_is_not_extended_by_reads(self):
        self.call(1); self.now = 104; self.call(2)
        self.now = 105; self.call(3)
        self.assertEqual(len(self.posts), 2)

    def test_zero_ttl_disables_memory_cache(self):
        proxy.TOOLS_CACHE_TTL_SECONDS = 0
        self.call(1); self.call(2)
        self.assertEqual(len(self.posts), 2)

    def test_parameterized_request_never_reuses_or_populates_cache(self):
        self.call(1, {"cursor": "page"}); self.call(2, {"cursor": "page"})
        self.assertEqual(len(self.posts), 2)
        proxy._write_tools_cache.assert_not_called()
        self.call(3); self.call(4, {"cursor": "page"}); self.call(5)
        self.assertEqual(len(self.posts), 4)

    def test_paginated_response_not_cached_even_without_cursor_request(self):
        self.result["nextCursor"] = "more"
        self.call(1); self.call(2)
        self.assertEqual(len(self.posts), 2)
        proxy._write_tools_cache.assert_not_called()

    def test_errors_and_malformed_tools_are_not_cached(self):
        for result in [{"isError": True, "tools": []}, {"tools": "bad"},
                       {"tools": [4]}, {"tools": [{"description": "no name"}]}]:
            self.result = result
            self.call(1); self.call(2)
        self.assertEqual(len(self.posts), 8)
        proxy._write_tools_cache.assert_not_called()
        with patch.object(proxy, "_post_monolith", return_value=json.dumps({"error": {"code": -1}})) as post:
            self.call(1); self.call(2)
            self.assertEqual(post.call_count, 2)

    def test_offline_fallback_not_cached_in_memory(self):
        with patch.object(proxy, "_post_monolith", return_value=None) as post, \
             patch.object(proxy, "_fallback_tools_list", side_effect=lambda msg: json.dumps({"id": msg["id"], "result": {"tools": []}})):
            self.call(1); self.call(2)
            self.assertEqual(post.call_count, 2)
        self.assertIsNone(proxy._tools_memory_result)

    def test_initial_observation_and_transition_invalidate(self):
        self.call(1)
        with patch.object(proxy, "_check_monolith_up", return_value=True), \
             patch.object(proxy, "_send_list_changed") as notify:
            proxy.check_monolith_state_change(io.StringIO())
            self.call(2); self.assertEqual(len(self.posts), 2)
            proxy.check_monolith_state_change(io.StringIO())
            self.call(3); self.assertEqual(len(self.posts), 2)
            notify.assert_not_called()
        with patch.object(proxy, "_check_monolith_up", return_value=False), \
             patch.object(proxy, "_send_list_changed") as notify:
            proxy.check_monolith_state_change(io.StringIO())
            self.call(4); self.assertEqual(len(self.posts), 3)
            notify.assert_called_once()

    def test_transition_during_http_prevents_stale_publication(self):
        def transition(request):
            proxy._invalidate_tools_memory()
            return self.post(request)
        with patch.object(proxy, "_post_monolith", side_effect=transition): self.call(1)
        self.call(2)
        self.assertEqual(len(self.posts), 2)

    def test_ttl_environment_bounds(self):
        for text, expected in [("0", 0), ("-1", 0), ("70", 60), ("nan", 5),
                               ("inf", 5), ("nonsense", 5), ("2.5", 2.5)]:
            with patch.dict(os.environ, {"MONOLITH_TOOLS_CACHE_TTL_SECONDS": text}):
                self.assertEqual(proxy._tools_cache_ttl_from_env(), expected)


class PersistentCacheTests(unittest.TestCase):
    def test_only_changed_tools_are_written(self):
        path = Mock(); path.exists.return_value = True
        path.read_text.return_value = json.dumps([{"name": "tool"}])
        with patch.object(proxy, "_tools_cache_path", return_value=path):
            proxy._write_tools_cache(json.dumps({"id": 2, "result": {"tools": [{"name": "tool"}]}}))
            path.write_text.assert_not_called()
            proxy._write_tools_cache(json.dumps({"id": 3, "result": {"tools": [{"name": "different"}]}}))
            path.write_text.assert_called_once()

    def test_initialized_keeps_health_work_on_background_thread(self):
        source = io.StringIO('\n'.join(json.dumps(msg) for msg in [
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 7, "method": "ping"}]))
        output = io.StringIO()
        with patch.object(proxy, "TextIOWrapper", side_effect=[source, output]), \
             patch.object(proxy.threading, "Thread") as thread, \
             patch.object(proxy, "check_monolith_state_change") as check, \
             patch.object(proxy, "_init_call_log"), patch.object(proxy, "_log"):
            proxy.main()
            thread.return_value.start.assert_called_once()
            check.assert_not_called()
        self.assertEqual(json.loads(output.getvalue())["id"], 7)


if __name__ == "__main__": unittest.main()
