# Editor recovery owned by the MCP proxy

The Windows proxy can recover its own project's editor while the editor-side
HTTP server is unavailable. The editor plugin cannot perform this work after
its host process has exited.

## Configuration

`Config/EditorRecovery.json` enables observation for this installation:

```json
{ "auto_recover": true }
```

The proxy resolves the single `.uproject` above its plugin directory, then the
engine executable using the project's EngineAssociation and Windows engine
registrations. This installation also supports an existing `UE_<version>`
directory on the project drive. Explicit configuration takes precedence:

| Environment variable | Purpose |
| --- | --- |
| `MONOLITH_PROJECT_FILE` | Exact existing `.uproject` path |
| `MONOLITH_EDITOR_EXE` | Exact existing `UnrealEditor.exe` path |
| `MONOLITH_AUTO_RECOVER_EDITOR` | `1` enables observation, `0` disables it |
| `MONOLITH_PLUGIN_DIR` | Existing plugin directory; useful for staged binaries |

Optional `project_file` and `editor_exe` JSON settings resolve relative to the
plugin directory. The log-only `MONOLITH_PROJECT_ROOT` setting is independent.
Restart/reconnect the proxy after changing configuration.

## Behavior

The proxy first observes a running editor whose command line identifies the
configured project and whose executable matches the configured engine. An
observed nonzero process exit starts the same project with:

```text
UnrealEditor.exe "<project>.uproject" -AutoDeclinePackageRecovery
```

This is a per-launch choice to skip the Restore Packages prompt. Existing
autosave and backup package files remain on disk. Recovery metadata may change
as Unreal processes that option; it is not an autosave deletion operation.

A missing HTTP response alone leaves the editor process alone. An editor that
exits normally stays closed. A proxy started without an editor waits for an
editor to observe. A newly launched editor has a 120-second startup grace;
failure during startup is reported rather than repeatedly launched.

All proxy instances share a project-scoped Windows mutex and
`RuntimeData/EditorRecoveryState.json`, limiting launches to one per five
minutes. This also prevents several chats from starting duplicate editors.
The interrupted editor request is never replayed. After reconnecting, inspect
the resulting assets before deciding whether to repeat a mutation.

## Offline tool

`monolith_recovery` is present in online, cached, and seed tool lists:

```json
{ "action": "status" }
```

Reports the resolved project/engine paths and matched editor process state.
The proxy's ordinary health notifications indicate when HTTP becomes ready.

```json
{ "action": "recover", "skip_restore": true }
```

Starts an absent editor. A running editor is preserved. If exactly one matching
editor is at the English `Restore Packages` modal, this explicit action can
replace it with a skip-restore launch. Set `skip_restore` to false to preserve
that modal. Automated observation never terminates a living editor. Other
languages require an explicit future modal-detection implementation.

## Validation and deployment

`Tests/test_editor_recovery.py` exercises the Python policy with mocked process
operations. `Tests/test_native_editor_recovery.py` exercises native stdio tool
lists and real disposable Windows process fixtures, including abnormal and
clean exits and concurrent proxies. Build the fixture source
`Tools/MonolithProxy/Tests/EditorRecoveryFixture.cpp` as `UnrealEditor.exe` in a
temporary test directory, and set `MONOLITH_NATIVE_PROXY_EXE`,
`MONOLITH_RECOVERY_FIXTURE_EXE`, and `MONOLITH_RECOVERY_TEST_ROOT` for that test.
These tests do not crash a real Unreal Editor.

Build outputs and test scratch data should use an external temporary directory.
Replace `Binaries/monolith_proxy.exe` with the verified native build. Already
running proxies keep their old image until the MCP connection is restarted.
New tool visibility may also require the client's MCP tool-list refresh.
