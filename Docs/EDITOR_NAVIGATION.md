# Editor navigation

The editor navigation actions help a person reach an existing asset, Blueprint
graph or node and inspect the actual editor selection. They do not create,
connect, compile or save Blueprint assets.

## Workflow

1. Resolve the actual asset used by the actor or animation pipeline using the
   existing asset, Blueprint and animation query tools.
2. Inspect graph data to obtain an exact graph name/path and node name/GUID.
3. Call `editor_query` with `action: "navigate_to"`.
4. Call `get_navigation_state` after the editor has processed its UI update.
5. Explain the selected object's function and the specific property to inspect.
   After the person edits it, read its value again before reporting a change.

Example for the GASP project's current Manny animation Blueprint:

```json
{
  "action": "navigate_to",
  "params": {
    "asset_path": "/Game/CascadeurLiveLinkBackflipTest/MotionMatching/ABP_Manny_Locomotion",
    "graph_name": "AnimGraph",
    "node_id": "AnimGraphNode_MotionMatching_0"
  }
}
```

Asset paths are caller inputs; the implementation contains no project-specific
paths. An asset-only request opens its native editor. Graph and node navigation
requires a Blueprint (including an Animation Blueprint).

## Exact targets and evidence

- Graph selectors match an exact name or complete graph object path. Node
  selectors match an exact UObject name or GUID.
- A node search without a graph searches all Blueprint graphs. Multiple matches
  produce an error with candidate locations. Supply a graph path or unique GUID
  to resolve the ambiguity.
- Invalid graph/node/property targets are rejected before opening an editor.
- State reads report the actual focused graph, selected nodes and objects bound
  to Blueprint Details. These are editor state, not a screenshot assessment.
- `pixel_visibility_verified` remains false. A separate screenshot is needed
  to confirm the target is visually usable at the current window size.
- Package dirty state is reported. Navigation never clears a pre-existing dirty
  flag or saves unrelated changes.
- `get_current_editor_context` includes navigation state for each edited asset.
  Its existing active-asset confidence distinguishes an exact tab-manager match
  from the most recently activated editor fallback.

## Scope

This first version addresses native asset opening and Blueprint graph/node
navigation. It does not provide a universal pixel locator for every Unreal
Editor button, nor does it infer which asset is used at runtime from its name.
Chooser, Pose Search Database and animation assets can be opened; specialized
row/entry selection in those editors requires dedicated adapters.

Use native Blueprint Details for property inspection. A reflection read does
not by itself prove that a matching property row is visible. Report unsupported
native Details adapters explicitly rather than presenting a different panel as
the asset's own editor.

## Properties and readback

Add `"property_path": "Node.BlendTime"` to the node navigation request to read
that reflected field and request its native Blueprint Details presentation.
Nested scalar structs are supported. Array indexing and traversal through object
references are not supported. With no node specified, a Blueprint property is
resolved on its generated class default object; other asset properties are
resolved on the asset itself.

Read back using the full object path returned by navigation:

```json
{
  "action": "get_navigation_state",
  "params": {
    "asset_path": "/Game/CascadeurLiveLinkBackflipTest/MotionMatching/ABP_Manny_Locomotion.ABP_Manny_Locomotion"
  }
}
```

`last_property_target` contains the original `baseline_exported`, current
`value_exported` and `changed_since_navigation`. The comparison tracks the most
recent successful navigation with a property; a new navigation replaces it.
Targets are weak references so inspection does not keep obsolete assets alive.
If compilation replaces the target node or class default object, an unavailable
target is reported instead of silently comparing a different object.

The native Details panel may update on a later editor tick. Inspect
`details_object_paths` after navigation. A successful request and exported value
alone do not establish property-row visibility.

Property reveal waits for a Slate post-tick with the same editor, graph,
selection and bound Details object. A changed target cancels the request;
the delayed callback never reselects objects or takes focus. It retries a
pending Details binding at most twice. `reveal_status` explains the outcome.
`exact_property_path_listed` and `property_row_listed` describe native Details
enumeration, not visible screen pixels. Nested customized rows may not appear in
the enumeration even when the reflected field exists.

### Click-to-acknowledge property blink

After its initial reveal, the property row alternates highlighted/clear every
0.6 seconds. It continues until the person clicks the target row; there is no
automatic time limit. Clicks are matched through the actual Slate hit path,
the owning Details view, and the exact property row index. The input observer
returns false, so normal field editing and context menus still receive the click.
Clicking unrelated rows does not acknowledge the target. Changing the selected
node/editor or starting a new navigation clears the previous cue.

Later flashes only illuminate a currently generated, visibly clipped-in row;
they do not scroll an off-screen row back into view. State readback reports
`blink_active`, `highlight_visible`, and `reveal_status`:
`blinking_until_property_click` while pending,
`acknowledged_by_property_click` after the click, or a cancellation reason.
The orange graph-node selection outline is independent and remains selected.

Click-acknowledgement acceptance (2026-10-30): the deployed module compiled and
linked successfully, and the four navigation regression tests passed. Live
state samples reported both visible and cleared phases while `blink_active`
remained true. During UI inspection, user input clicked the target and readback
reported `acknowledged_by_property_click`, `blink_active=false`, and
`highlight_visible=false`. Blend Time remained `0.2` and the package stayed
clean. The planned automated unrelated-row click was interrupted by user input,
so that specific negative click case was not claimed as interactively verified.

## Tests

Run `editor.run_automation_tests` with prefix `Monolith.Editor.Navigation` for
the transient-fixture resolution tests. They check missing graphs, ambiguous
node names, graph-scope violations and missing properties, including unchanged
editor sets and package dirty state. Real asset navigation and screenshot
acceptance are separate checks from these failure-path tests.

### Local acceptance: GASP, UE 5.8, 2026-10-30

- MonolithEditor C++ compilation and DLL linking succeeded; the deployed module
  was loaded after a clean editor exit and restart.
- All four `Monolith.Editor.Navigation` automation tests passed with zero
  warnings and errors.
- Native Pose Search Database opening succeeded for `PSD_Manny_Locomotion`.
- Navigation by node name and GUID reached the Manny Motion Matching node.
- A screenshot confirmed the selected node was centered and the Details panel
  had scrolled to and highlighted `Blend Time`, displaying `0.2`.
- Follow-up state confirmed the actual graph, selected node, Details object,
  exact property row, and `highlight_requested_after_details_binding`.
- Current-context reporting identified the ABP through an exact active-tab
  manager match and included its navigation state.
- Final dirty content and dirty map package lists were both empty. No animation
  or movement parameter was changed for acceptance. A manual value edit and
  comparison transition from false to true was not exercised in this run.

Local build products, retired binaries and deployment hash manifests reside in
`E:\CodexTrashCan\MonolithEditorNavigation_20261030`; they are not required by the
runtime navigation feature. Source and registration live in this plugin.
