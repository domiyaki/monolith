# Guided editor workflows

A workflow submits the complete set of instructions once. The editor displays
the current step, navigates to its exact target, and checks completion before
advancing. The workflow itself never writes asset properties.

## Request format

Call `editor.start_guided_workflow` with a title and 1–32 steps. Each step has
`title`, `instruction`, `target`, and `completion`. Target fields are the same
asset, graph, node and property selectors used by `editor.navigate_to`.

```json
{
  "title": "Inspect a Motion Matching setting",
  "steps": [
    {
      "title": "Find Blend Time",
      "instruction": "Click the highlighted property row. Leave its value unchanged.",
      "target": {
        "asset_path": "/Game/YourFolder/YourAnimationBlueprint",
        "graph_name": "AnimGraph",
        "node_id": "AnimGraphNode_MotionMatching_0",
        "property_path": "Node.BlendTime"
      },
      "completion": { "type": "property_clicked" }
    }
  ]
}
```

The portable GASP example is stored in
`Examples/Guides/MannyMotionMatchingInspector.json`. Its asset paths belong to
that example project, not to the guide engine. It asks the person to find two
properties and confirm completion without changing any settings.

## Completion conditions

| Type | What completes the step |
| --- | --- |
| `property_clicked` | An actual click on the exact highlighted property row. |
| `property_equals` | The actual exported property value equals `expected_value`. |
| `confirm` | The person presses the guide's check-and-continue button with the target still valid. |

For `property_equals`, `expected_value` is the exact Unreal reflection export
string, for example `"0.250000"` for a float. Read the actual property format
when composing a guide. An already matching value can complete immediately;
the condition does not require an edit event. The check-and-continue button
does not override a failed click or value condition.

## User control and target checks

The panel presents the full sequence, current instructions, and completion
feedback. Repositioning uses the existing navigation action. Ending the guide
or closing its panel removes the pending highlight and stops progression.

Changing the selected node, closing the target editor, or viewing a different
asset prevents completion of the current step. The guide explains the mismatch
and offers repositioning rather than repeatedly taking focus. Returning to a
valid target allows checking to continue.

Structural validation covers all steps before replacing a running workflow.
Asset-dependent resolution happens when a step is entered; an unavailable or
ambiguous target blocks that step with an explanation. Do not interpret a
workflow's successful submission as proof that every future target exists.

## Scope

The first version guides the native asset/Blueprint/Details targets supported by
the navigation adapter. It does not yet automate arbitrary menus, verify compile
results, or validate runtime behavior. A `confirm` step records an explicit user
confirmation; it is not evidence that a compilation or gameplay test succeeded.

Tests under `Monolith.Editor.Guide` check invalid requests preserve the active
session and open editor set. Real UI progression and result gating require a
separate interactive acceptance check.

## Verified in GASP / UE 5.8 (2026-10-30)

- C++ compilation and linking passed; the deployed editor discovers all four
  workflow actions. Four `Monolith.Editor.Guide` validation tests passed.
- The three-step Manny example progressed through both real property clicks
  and the final user confirmation to `completed`.
- A `property_equals` step with actual `0.200000` and expected `999.000000`
  stayed on step zero. Expected `0.200000` completed without changing the asset.
- Selecting Pose History while the workflow required Motion Matching reported
  `target_matches=false` and stayed on step zero; explicit repositioning restored
  the target.
- Explicit stop and completion cleared navigation highlighting and polling.
- The compact independent guide window was visually inspected beside the MM
  editor. The attempted window-close UI test was interrupted by detected user
  input; its close callback was implemented but that UI gesture was not verified.
- No animation or movement parameter was changed during acceptance.

Sessions are in memory. Re-submit the saved example after editor restart.
Guide definitions are data interpreted by the same engine, not separate scripts
compiled for each property or workflow.
