# Material editor synchronization

Material editors work on preview copies. Writing or compiling the source asset
does not update that copy. Applying a stale preview later can overwrite an
external change. Material instance editors also cache parameter objects.

`MonolithMaterialEditorSync` wraps material writes at registration time:

1. Resolve open toolkits by their registered object paths and source-instance
   references. Never identify a preview by its short name or scan transient
   expressions for an apparent match.
2. Apply pending edits through the toolkit's native Apply command before the
   handler loads the asset. Applying may replace the source UObject.
3. Execute the original action and its transactions.
4. Refresh the target material/function graph from the resulting source, and
   rebuild affected instance parameter caches in place. Keeping the existing
   material-instance preview object is required because its viewport is bound to
   that object and slider changes are delivered as interactive property events.
   Other dirty material graphs are not forcibly refreshed.

Read-only actions and clipboard preparation do not apply editor changes. A query
still describes the source asset; manual edits that have not been applied can
differ from that source.

The wrapper supports both `asset_path` and `asset_paths` for registered graph,
parameter, instance, batch-property, recompile, and save operations. It reports
`editor_sync`, `synchronized_material_editors`, and
`synchronized_instance_editors`. These describe editor refresh, separately from
shader compilation or visual acceptance. `not_refreshed` is not confirmation of
UI synchronization. Function-instance caches with pending edits are preserved
instead of discarded by dependency refresh.

The current engine provides `IMaterialEditor::RefreshGraphFromOriginal`. This
integration was built and tested against the project's UE 5.8 engine containing
that interface; stock engines without it require an adapter. No engine changes
are made by this fix.

## Regression tests

Run `Monolith.Material.EditorSync.` in the running editor. Tests use temporary
packages and close their fixture editors; no test assets are saved to Content.

- `PendingEditsAndApply`: preserve an unapplied manual parameter change while
  updating another Custom node; check the actual toolkit preview and then Apply
  again to catch stale-preview overwrite.
- `InstanceOverrideCache`: update an override while the instance editor is open;
  check the visible parameter object, preserve the viewport preview object, copy
  the cache back to the source, and verify that an interactive slider event
  reaches the displayed preview without waiting for mouse release.

The tests re-register material actions, which also activates the wrapper after
a Live Coding patch. A normal module load registers it automatically.
