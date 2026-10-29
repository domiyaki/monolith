# Repeatable runtime movement capture

Use `editor.capture_pie_movement_clip` with `input_timeline` to run a complete
input sequence inside Unreal. Use `editor.run_pie_smoke` with the same field for
telemetry without image capture. One request starts the session; Unreal advances
it asynchronously. Polling does not drive the character.

`Templates/MovementTests/MannyForwardStop.example.json` contains a project-specific
example: wait 0.5 game seconds, hold forward for 1.4 seconds, then observe the stop
until 4 seconds. Supply `output_path` explicitly when using the capture action.
For the current workstation, put test output under `E:/CodexTrashCan/`.

## Input timeline

Each segment has `start_seconds`, `duration_seconds`, `input_action` (full asset
path), `value`, and optional `player_index` (default 0). The value shape must match
the InputAction: boolean, scalar, two-component array, or three-component array.
Segments for different actions can overlap, so simultaneous forward/strafe input
is supported. Segments for the same player/action must not overlap. A segment
must fit inside the session duration. All segments are validated before map load
or Play begins.

The time origin is the first ready observation of the target pawn and local input
subsystems. Scheduling uses game time. An editor stall does not become an extra
long key hold measured in wall time. Dispatch occurs on the editor observer tick
and is consumed by Enhanced Input during its subsequent processing; this is not
a fixed-step simulation or a promise of exact frame boundaries. Compare requested
times with reported `first_dispatch_seconds` and `release_seconds`.

The sampled pawn is the controlled pawn of the first segment's player index,
reported as `sample_player_index`. A requested `pawn_class` must match it. When
`expected_anim_class` is supplied, the test waits for a live animation instance
and verifies its class before sending input. Possession changes abort the test.

Ending a segment injects zero. Normal completion, explicit stop, PIE termination,
and the wall-clock watchdog all clean up timeline input. A paused/unready test is
bounded by the watchdog. A failed/aborted test must be inspected as such, not
treated as a successful movement recording. Timelines allow up to 128 segments,
120 game seconds, and 16,384 samples; exceeding the sample limit aborts explicitly.

## Reading the result

Call `editor.poll_pie_smoke` with the returned `session_id` and `include_samples=true`.
Inspect status, runtime identity, input dispatch counts and release results, as
well as image `capture_validity`. A successful request by itself only means the
test was accepted. A missing viewport can still produce telemetry without usable
images.

Full samples share a time basis and contain:

- `pawn.location_cm` and `rotation_pitch_yaw_roll_degrees`: world-space actor transform.
- `pawn.api_velocity_cm_s`: the value reported by the pawn API.
- `pawn.measured_velocity_cm_s`: consecutive position difference divided by elapsed
  game time, absent on the first sample. Teleports also appear in this measurement.
- `anim_vars`: requested reflected AnimInstance fields. Use dotted leaves such as
  `MotionMatchingSelection.SelectedAnim` and `MotionMatchingSelection.SelectedTime`.
- `missing_anim_vars`: requested fields that could not be read.
- Captured frame paths, when available.

The requested animation fields belong to the project, not a universal MM API.
The Manny example uses its existing `MotionMatchingSelection` variable. Another
Anim Blueprint may expose different fields. Actor trajectories are not equivalent
to an animation root-bone track: mesh offsets, rotation, and coordinate conversion
still matter when transferring reference motion to a DCC application.

Save the full report as JSON, then export a trajectory table:

```text
python Scripts/export_movement_capture.py report.json trajectory.csv
```

The exporter prefixes animation columns with `anim.` to keep their names separate
from trajectory and timing columns. It preserves centimeters/degrees and the reported time basis. It does
not generate animation or FBX, alter the moving actor, or change the movement model.

## Existing input action tool

`pie_inject_input_action` remains useful for interactive one-shot or frame-count
holds. A subsequent request for the same player/action replaces the prior queued
hold, including `value=0, repeat_frames=1`. Use the timeline when duration,
automatic release, synchronized observation, and reproducibility matter.
