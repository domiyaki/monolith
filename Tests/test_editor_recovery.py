"""Run with PYTHONPYCACHEPREFIX pointing outside the project: python -m unittest discover -s Tests -p test_editor_recovery.py."""
import json
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Scripts"))
import monolith_editor_recovery as recovery
import monolith_proxy as proxy


class FakeWindows:
    def __init__(self):
        self.items = []
        self.code = None
        self.launches = []
        self.modal = False

    def processes(self, *args): return list(self.items)
    def close(self, handle): pass
    def exit_code(self, handle): return self.code
    def restore_modal(self, pid): return ["Restore Packages"] if self.modal else []
    def launch_locked(self, *args):
        self.launches.append(args)
        return {"launched": True, "pid": 4}


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.native = FakeWindows()
        self.now = 1000
        self.manager = recovery.EditorRecovery(self.native, lambda: ("X:/P.uproject", "X:/UnrealEditor.exe"), lambda: self.now)
        self.env = patch.dict(os.environ, {"MONOLITH_AUTO_RECOVER_EDITOR": "1"})
        self.env.start()
        self.addCleanup(self.env.stop)

    def observe(self):
        self.native.items = [{"pid": 3, "handle": 5, "creation_time": 1}]
        self.manager.tick(True)

    def test_no_startup_launch(self):
        self.manager.tick(False)
        self.assertEqual(self.native.launches, [])

    def test_observed_crash_restarts_once(self):
        self.observe()
        self.native.items = []; self.native.code = 7
        self.manager.tick(False); self.manager.tick(False)
        self.assertEqual(len(self.native.launches), 1)

    def test_clean_exit_does_not_restart(self):
        self.observe()
        self.native.items = []; self.native.code = 0
        self.manager.tick(False)
        self.assertEqual(self.native.launches, [])

    def test_live_editor_is_preserved(self):
        self.observe()
        self.assertEqual(self.manager.recover(False)["reason"], "same_project_editor_running")
        self.assertEqual(self.native.launches, [])

    def test_explicit_restore_passes_identity(self):
        self.observe(); self.native.modal = True
        self.assertTrue(self.manager.recover(False)["launched"])
        self.assertEqual(self.native.launches[0][2]["creation_time"], 1)

    def test_skip_restore_false_preserves_modal(self):
        self.observe(); self.native.modal = True
        self.manager.recover(False, False)
        self.assertEqual(self.native.launches, [])

    def test_auto_never_kills_modal(self):
        self.observe(); self.native.modal = True
        self.manager.tick(False)
        self.assertEqual(self.native.launches, [])

    def test_auto_optout(self):
        self.observe(); self.native.items = []; self.native.code = 7
        with patch.dict(os.environ, MONOLITH_AUTO_RECOVER_EDITOR="0"):
            self.manager.tick(False)
        self.assertEqual(self.native.launches, [])

    def test_grace_blocks_manual_duplicate(self):
        self.manager.recover(False)
        self.assertEqual(self.manager.recover(False)["reason"], "startup_grace")
        self.assertEqual(len(self.native.launches), 1)

    def test_healthy_recover_is_idempotent(self):
        self.assertEqual(self.manager.recover(True)["reason"], "editor_healthy")
        self.assertEqual(self.native.launches, [])

    def test_local_call_never_forwards(self):
        with patch.object(proxy, "_post_monolith", side_effect=AssertionError("forwarded")), patch.object(proxy, "_check_monolith_up", return_value=False), patch.object(proxy, "RECOVERY", self.manager):
            result = json.loads(proxy.handle_tools_call({"id": 1, "params": {"name": "monolith_recovery", "arguments": {"action": "status"}}}))
        self.assertFalse(result["result"]["isError"])

    def test_all_tool_list_sources(self):
        schema = {"name": "upstream"}
        with patch.object(proxy, "_post_monolith", return_value=json.dumps({"id": 1, "result": {"tools": [schema]}})), patch.object(proxy, "_write_tools_cache"), patch.object(proxy, "_write_call_log_line"):
            names = [t["name"] for t in json.loads(proxy.handle_tools_list({"id": 1}))["result"]["tools"]]
            self.assertIn("monolith_recovery", names)
        for cached in ([schema], None):
            with self.subTest(cached=cached), patch.object(proxy, "_read_tools_cache", return_value=cached):
                names = [t["name"] for t in json.loads(proxy._fallback_tools_list({"id": 1}))["result"]["tools"]]
                self.assertIn("monolith_recovery", names)

    def test_local_tool_deduplicated(self):
        self.assertEqual(sum(x["name"] == "monolith_recovery" for x in recovery.append_tool(recovery.append_tool([]))), 1)

    def test_configuration_failure_is_structured(self):
        def fail(): raise ValueError("bad configuration")
        self.manager.resolver = fail
        self.assertEqual(self.manager.recover()["error"], "bad configuration")


if __name__ == "__main__":
    unittest.main()
