"""Export a full poll_pie_smoke report to a world-space pawn trajectory CSV.

Usage: python export_movement_capture.py report.json trajectory.csv
Units are Unreal centimeters, seconds, and degrees. This is the pawn/updated
component trajectory, not an extracted animation root-bone track or an FBX.
The caller chooses the output path. No project assets are modified.
"""
import argparse
import csv
import json
from pathlib import Path


def export_report(report, destination):
    samples = report.get("samples", [])
    if not samples or not any("pawn" in sample for sample in samples):
        raise ValueError("Report has no pawn samples; request poll_pie_smoke with include_samples=true")
    variable_names = sorted({name for sample in samples for name in sample.get("anim_vars", {})})
    fields = ["time_seconds", "sample_time_basis", "location_x_cm", "location_y_cm", "location_z_cm",
              "pitch_degrees", "yaw_degrees", "roll_degrees", "api_velocity_x_cm_s", "api_velocity_y_cm_s",
              "api_velocity_z_cm_s", "measured_velocity_x_cm_s", "measured_velocity_y_cm_s",
              "measured_velocity_z_cm_s", "measured_ground_speed_cm_s", "frame_path"] + ["anim." + name for name in variable_names]
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with destination.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for sample in samples:
            pawn = sample.get("pawn")
            if not pawn:
                continue
            row = {"time_seconds": sample["t"],
                   "sample_time_basis": report.get("sample_time_basis", "wall_seconds_since_marker"),
                   "measured_ground_speed_cm_s": pawn.get("measured_ground_speed_cm_s", ""),
                   "frame_path": sample.get("frame_path", "")}
            for key, columns in [
                ("location_cm", ["location_x_cm", "location_y_cm", "location_z_cm"]),
                ("rotation_pitch_yaw_roll_degrees", ["pitch_degrees", "yaw_degrees", "roll_degrees"]),
                ("api_velocity_cm_s", ["api_velocity_x_cm_s", "api_velocity_y_cm_s", "api_velocity_z_cm_s"]),
                ("measured_velocity_cm_s", ["measured_velocity_x_cm_s", "measured_velocity_y_cm_s", "measured_velocity_z_cm_s"]),
            ]:
                values = pawn.get(key)
                if values is not None:
                    if len(values) != 3:
                        raise ValueError(f"Invalid {key}: expected three components")
                    row.update(zip(columns, values))
            for name, value in sample.get("anim_vars", {}).items():
                row["anim." + name] = json.dumps(value, ensure_ascii=False) if isinstance(value, (dict, list)) else value
            writer.writerow(row)
            count += 1
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    if args.report.resolve() == args.destination.resolve():
        parser.error("Report and CSV destination must be different files")
    report = json.loads(args.report.read_text(encoding="utf-8-sig"))
    count = export_report(report, args.destination)
    print(json.dumps({"rows": count, "csv": str(args.destination.resolve()),
                      "status": report.get("status"), "sample_time_basis": report.get("sample_time_basis")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
