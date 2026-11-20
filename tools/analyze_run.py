#!/usr/bin/env python3
"""Evaluate a formal 72-hour run without reducing the denominator for missing data."""
import argparse
import csv
import json
from pathlib import Path

EXPECTED_WINDOWS = 4320
EXPECTED_SAMPLES = 259200

def truth(value):
    return str(value).lower() in ("true", "1", "yes")

def number(row, key, default=0):
    value = row.get(key)
    return int(float(value)) if value not in (None, "", "null") else default

def analyze(rows, run_id):
    unique = {}
    duplicate_ids = 0
    for row in rows:
        if row.get("run_id") != run_id or row.get("record_type") != "MINUTE" or truth(row.get("demo")):
            continue
        identity = row.get("record_id")
        if not identity:
            raise ValueError("Every row must retain its original record_id")
        if identity in unique:
            duplicate_ids += 1
            if unique[identity] != row:
                raise ValueError("Conflicting content for " + identity)
        unique[identity] = row
    windows = {}
    uncertain = partials = outside = duplicates = 0
    for row in unique.values():
        index = number(row, "window_index", -1)
        if index < 0 or not row.get("measured_at") or row.get("time_quality") != "SYNCED":
            uncertain += 1
            continue
        if index >= EXPECTED_WINDOWS:
            outside += 1
            continue
        if truth(row.get("partial")) or number(row, "window_ms") != 60000 or number(row, "expected_count") != 60:
            partials += 1
            continue
        count = sum(number(row, key) for key in ("valid_count", "warmup_count", "error_count", "missed_count"))
        if count != 60 or not 0 <= number(row, "valid_ms") <= 60000:
            raise ValueError("Invalid minute accounting")
        if index in windows:
            duplicates += 1
            continue
        windows[index] = row
    missing = [index for index in range(EXPECTED_WINDOWS) if index not in windows]
    ranges = []
    for index in missing:
        if ranges and ranges[-1]["last_window"] == index - 1:
            ranges[-1]["last_window"] = index
            ranges[-1]["count"] += 1
        else:
            ranges.append(dict(first_window=index, last_window=index, count=1, reason="UNDETERMINED; correlate with device and power evidence"))
    arrived = timely = 0
    for row in windows.values():
        received = number(row, "received_at")
        if received:
            arrived += 1
            latency = received - number(row, "measured_at")
            timely += 0 <= latency <= 120000
    valid_samples = sum(number(row, "valid_count") for row in windows.values())
    valid_ms = sum(number(row, "valid_ms") for row in windows.values())
    any_valid = sum(number(row, "valid_count") > 0 for row in windows.values())
    mostly_valid = sum(number(row, "valid_count") >= 54 for row in windows.values())
    return dict(run_id=run_id, expected_windows=EXPECTED_WINDOWS, expected_sample_slots=EXPECTED_SAMPLES,
                complete_unique_windows=len(windows), completeness=len(windows)/EXPECTED_WINDOWS,
                completeness_slo_pass=len(windows)/EXPECTED_WINDOWS >= 0.99,
                valid_samples=valid_samples, valid_sample_coverage=valid_samples/EXPECTED_SAMPLES,
                windows_with_valid_samples=any_valid, windows_with_valid_samples_fraction=any_valid/EXPECTED_WINDOWS,
                windows_with_90pct_valid_samples=mostly_valid, windows_with_90pct_valid_samples_fraction=mostly_valid/EXPECTED_WINDOWS,
                recorded_warmup_slots=sum(number(row, "warmup_count") for row in windows.values()),
                recorded_error_slots=sum(number(row, "error_count") for row in windows.values()),
                recorded_missed_slots=sum(number(row, "missed_count") for row in windows.values()),
                valid_ms=valid_ms, valid_time_coverage=valid_ms/(72*3600000),
                coverage_note="Conservative lower bound from complete windows; partial and uncertain windows are listed separately.",
                cloud_confirmed_windows=arrived, cloud_completeness=arrived/EXPECTED_WINDOWS,
                delivered_within_120s=timely, timely_fraction_of_all_planned_windows=timely/EXPECTED_WINDOWS,
                timeliness_note="Manual-offline periods need a separate, evidence-backed online denominator.",
                partial_windows_excluded=partials, uncertain_time_windows=uncertain,
                outside_run_windows=outside, duplicate_record_ids=duplicate_ids,
                duplicate_complete_window_indices=duplicates, gaps=ranges)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv_file")
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--output")
    args = parser.parse_args()
    with open(args.csv_file, newline="", encoding="utf-8-sig") as stream:
        report = analyze(list(csv.DictReader(stream)), args.run_id)
    text = json.dumps(report, indent=2)
    if args.output:
        Path(args.output).write_text(text + "\n")
    print(text)

if __name__ == "__main__":
    main()
