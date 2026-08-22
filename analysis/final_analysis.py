"""Reproduce the dissertation's final operational and comparison statistics."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd


REPO_ROOT = Path(__file__).resolve().parents[1]
STAGING_ROOT = REPO_ROOT.parent
LOCAL_TZ = "Europe/London"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--project-data",
        type=Path,
        default=REPO_ROOT / "data" / "project_sensing_data.csv",
    )
    parser.add_argument(
        "--ttn-raw",
        type=Path,
        default=STAGING_ROOT / "ttn_uplinks_30days.jsonl",
        help="Local-only raw TTN export; clean CSV is used if this file is absent.",
    )
    parser.add_argument(
        "--ttn-clean",
        type=Path,
        default=REPO_ROOT / "data" / "ttn_uplinks_clean.csv",
    )
    parser.add_argument(
        "--weather",
        type=Path,
        default=STAGING_ROOT / "wunderground_20260801_20260816.json",
    )
    parser.add_argument(
        "--laqn",
        type=Path,
        default=STAGING_ROOT / "Londonair PM DATA.csv",
    )
    parser.add_argument(
        "--write-clean-ttn",
        action="store_true",
        help="Write the public, metadata-minimised TTN CSV from the raw export.",
    )
    return parser.parse_args()


def load_project(path: Path) -> pd.DataFrame:
    data = pd.read_csv(path)
    required = ["created_at", "field1", "field2", "field3", "field4", "field5"]
    missing = [column for column in required if column not in data.columns]
    if missing:
        raise ValueError(f"Project data is missing columns: {', '.join(missing)}")

    data["timestamp"] = pd.to_datetime(data["created_at"], utc=True, errors="coerce")
    for column in required[1:]:
        data[column] = pd.to_numeric(data[column], errors="coerce")
    return data.sort_values("timestamp").reset_index(drop=True)


def project_operational(project: pd.DataFrame) -> dict[str, float | int]:
    valid = project.dropna(subset=["timestamp", "field1", "field2", "field3", "field4", "field5"])
    intervals = valid["timestamp"].diff().dropna().dt.total_seconds().div(60)
    long_gap_mask = intervals > 60

    segment_id = long_gap_mask.reindex(valid.index, fill_value=False).cumsum()
    segment_spans = valid.groupby(segment_id)["timestamp"].agg(lambda x: (x.iloc[-1] - x.iloc[0]).total_seconds())

    return {
        "valid_records": len(valid),
        "span_days": (valid["timestamp"].iloc[-1] - valid["timestamp"].iloc[0]).total_seconds() / 86400,
        "median_interval_min": float(intervals.median()),
        "intervals_9_5_to_10_5_percent": float(intervals.between(9.5, 10.5).mean() * 100),
        "gaps_over_1h": int(long_gap_mask.sum()),
        "maximum_gap_hours": float(intervals.max() / 60),
        "longest_stable_days": float(segment_spans.max() / 86400),
    }


def _decoded_value(decoded: dict[str, Any], semantic: str, field: str) -> Any:
    return decoded.get(semantic, decoded.get(field))


def load_ttn_raw(path: Path) -> pd.DataFrame:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)["result"]
            except (json.JSONDecodeError, KeyError) as exc:
                raise ValueError(f"Invalid TTN record on physical line {line_number}") from exc
            uplink = record["uplink_message"]
            decoded = uplink.get("decoded_payload") or {}
            rows.append(
                {
                    "timestamp": record.get("received_at"),
                    "frame_counter": uplink.get("f_cnt"),
                    "f_port": uplink.get("f_port"),
                    "temperature_c": _decoded_value(decoded, "temperature_c", "field1"),
                    "humidity_percent": _decoded_value(decoded, "humidity_percent", "field2"),
                    "pm25_ugm3": _decoded_value(decoded, "pm25_ugm3", "field3"),
                    "pm10_ugm3": _decoded_value(decoded, "pm10_ugm3", "field4"),
                    "pressure_hPa": _decoded_value(decoded, "pressure_hPa", "field5"),
                    "gateway_reception_count": len(uplink.get("rx_metadata") or []),
                }
            )
    return normalise_ttn(pd.DataFrame(rows))


def normalise_ttn(data: pd.DataFrame) -> pd.DataFrame:
    expected = [
        "timestamp",
        "frame_counter",
        "f_port",
        "temperature_c",
        "humidity_percent",
        "pm25_ugm3",
        "pm10_ugm3",
        "pressure_hPa",
        "gateway_reception_count",
    ]
    missing = [column for column in expected if column not in data.columns]
    if missing:
        raise ValueError(f"TTN data is missing columns: {', '.join(missing)}")
    data = data[expected].copy()
    data["timestamp"] = pd.to_datetime(data["timestamp"], utc=True, errors="coerce")
    for column in expected[1:]:
        data[column] = pd.to_numeric(data[column], errors="coerce")
    return data.sort_values("timestamp").reset_index(drop=True)


def load_ttn(raw_path: Path, clean_path: Path) -> tuple[pd.DataFrame, str]:
    if raw_path.exists():
        return load_ttn_raw(raw_path), "raw local export"
    if clean_path.exists():
        return normalise_ttn(pd.read_csv(clean_path)), "public clean CSV"
    raise FileNotFoundError("Neither the local raw TTN export nor the public clean CSV was found.")


def ttn_operational(ttn: pd.DataFrame) -> dict[str, float | int]:
    valid = ttn.dropna(subset=["timestamp", "frame_counter", "f_port"]).copy()
    intervals = valid["timestamp"].diff().dropna().dt.total_seconds().div(60)
    counter_steps = valid["frame_counter"].diff().dropna()
    missing_counters = int((counter_steps[counter_steps > 1] - 1).sum())
    return {
        "uplinks": len(valid),
        "span_days": (valid["timestamp"].iloc[-1] - valid["timestamp"].iloc[0]).total_seconds() / 86400,
        "median_interval_min": float(intervals.median()),
        "missing_frame_counters": missing_counters,
        "gateway_receptions": int(valid["gateway_reception_count"].sum()),
        "f_port_values": ", ".join(str(int(value)) for value in sorted(valid["f_port"].unique())),
    }


def hourly_project(project: pd.DataFrame) -> pd.DataFrame:
    hourly = project.dropna(subset=["timestamp"]).copy()
    hourly["hour_local"] = hourly["timestamp"].dt.tz_convert(LOCAL_TZ).dt.floor("h").dt.tz_localize(None)
    return hourly.groupby("hour_local", as_index=False)[["field1", "field2", "field3", "field4", "field5"]].mean()


def load_weather(path: Path) -> pd.DataFrame:
    payload = json.loads(path.read_text(encoding="utf-8"))
    observations = payload.get("observations", [])
    rows = []
    for observation in observations:
        if observation.get("stationID") != "ILONDO932":
            continue
        metric = observation.get("metric") or {}
        rows.append(
            {
                "hour_local": pd.to_datetime(observation.get("obsTimeLocal"), errors="coerce").floor("h"),
                "wu_temperature": metric.get("tempAvg"),
                "wu_humidity": observation.get("humidityAvg"),
                "wu_pressure": np.mean([metric.get("pressureMax"), metric.get("pressureMin")]),
            }
        )
    return pd.DataFrame(rows).dropna(subset=["hour_local"])


def paired_stats(frame: pd.DataFrame, project_column: str, external_column: str) -> tuple[int, float, float]:
    pairs = frame[[project_column, external_column]].dropna()
    if len(pairs) < 2:
        return len(pairs), float("nan"), float("nan")
    correlation = np.corrcoef(pairs[project_column], pairs[external_column])[0, 1]
    mean_difference = (pairs[project_column] - pairs[external_column]).mean()
    return len(pairs), float(correlation), float(mean_difference)


def weather_comparison(project_hourly: pd.DataFrame, path: Path) -> dict[str, tuple[int, float, float]]:
    matched = project_hourly.merge(load_weather(path), on="hour_local", how="inner")
    return {
        "temperature": paired_stats(matched, "field1", "wu_temperature"),
        "humidity": paired_stats(matched, "field2", "wu_humidity"),
        "pressure": paired_stats(matched, "field5", "wu_pressure"),
    }


def load_laqn(path: Path) -> pd.DataFrame:
    data = pd.read_csv(path)
    data = data[(data["Site"] == "TH5") & data["Species"].isin(["PM2.5", "PM10"])].copy()
    data["hour_local"] = pd.to_datetime(data["ReadingDateTime"], dayfirst=True, errors="coerce").dt.floor("h")
    data["Value"] = pd.to_numeric(data["Value"], errors="coerce")
    return data.pivot_table(index="hour_local", columns="Species", values="Value", aggfunc="mean").reset_index()


def pm_comparison(project_hourly: pd.DataFrame, path: Path) -> dict[str, tuple[int, float, float]]:
    matched = project_hourly.merge(load_laqn(path), on="hour_local", how="inner")
    return {
        "pm25": paired_stats(matched, "field3", "PM2.5"),
        "pm10": paired_stats(matched, "field4", "PM10"),
    }


def pms_consistency(project: pd.DataFrame) -> dict[str, float | int]:
    values = project[["field3", "field4"]].dropna()
    identical = values["field3"].eq(values["field4"])
    both_zero = values["field3"].eq(0) & values["field4"].eq(0)
    return {
        "records": len(values),
        "identical_count": int(identical.sum()),
        "identical_percent": float(identical.mean() * 100),
        "both_zero_percent": float(both_zero.mean() * 100),
        "pm25_zero_percent": float(values["field3"].eq(0).mean() * 100),
        "pm10_zero_percent": float(values["field4"].eq(0).mean() * 100),
    }


def status(actual: float | int, expected: float | int, tolerance: float = 0) -> str:
    return "PASS" if abs(float(actual) - float(expected)) <= tolerance else "CHECK"


def add_validation(
    rows: list[tuple[str, str, str, str]],
    metric: str,
    actual: float | int,
    expected: float | int,
    actual_format: str,
    expected_text: str,
    tolerance: float = 0,
) -> None:
    rows.append((metric, format(actual, actual_format), expected_text, status(actual, expected, tolerance)))


def print_validation(
    operational: dict[str, float | int],
    ttn_stats: dict[str, float | int],
    weather: dict[str, tuple[int, float, float]] | None,
    pm: dict[str, tuple[int, float, float]] | None,
    pms: dict[str, float | int],
) -> None:
    rows: list[tuple[str, str, str, str]] = []
    add_validation(rows, "ThingSpeak valid records", operational["valid_records"], 4961, ",d", "4,961")
    add_validation(rows, "ThingSpeak span (days)", operational["span_days"], 38.42, ".2f", "~38.42", 0.02)
    add_validation(rows, "ThingSpeak median interval (min)", operational["median_interval_min"], 10.0, ".2f", "~10.00", 0.05)
    add_validation(rows, "Intervals within 9.5-10.5 min (%)", operational["intervals_9_5_to_10_5_percent"], 98.79, ".2f", "~98.79", 0.05)
    add_validation(rows, "Gaps over 1 hour", operational["gaps_over_1h"], 4, "d", "4")
    add_validation(rows, "Maximum gap (hours)", operational["maximum_gap_hours"], 64.99, ".2f", "~64.99", 0.05)
    add_validation(rows, "Longest stable period (days)", operational["longest_stable_days"], 34.66, ".2f", "~34.66", 0.02)
    add_validation(rows, "TTN uplinks", ttn_stats["uplinks"], 318, "d", "318")
    add_validation(rows, "TTN span (days)", ttn_stats["span_days"], 2.21, ".2f", "~2.21", 0.02)
    add_validation(rows, "TTN median interval (min)", ttn_stats["median_interval_min"], 10.0, ".2f", "~10.00", 0.05)
    add_validation(rows, "Absent frame counters", ttn_stats["missing_frame_counters"], 1, "d", "1")
    add_validation(rows, "Gateway receptions", ttn_stats["gateway_receptions"], 994, "d", "994")
    add_validation(rows, "PM2.5 equals PM10 (records)", pms["identical_count"], 4903, "d", "4,903")
    add_validation(rows, "PM2.5 equals PM10 (%)", pms["identical_percent"], 98.83, ".2f", "~98.83", 0.02)
    add_validation(rows, "Both PM readings zero (%)", pms["both_zero_percent"], 22.0, ".2f", "~22", 1.0)

    if weather is not None:
        for name, expected_r, expected_diff in [
            ("temperature", 0.912, 0.88),
            ("humidity", 0.976, 7.14),
            ("pressure", 0.980, 6.24),
        ]:
            n, correlation, difference = weather[name]
            add_validation(rows, f"WU {name} matched hours", n, 384, "d", "~384", 1)
            add_validation(rows, f"WU {name} Pearson r", correlation, expected_r, ".3f", f"~{expected_r:.3f}", 0.005)
            add_validation(rows, f"WU {name} mean difference", difference, expected_diff, "+.2f", f"~{expected_diff:+.2f}", 0.10)

    if pm is not None:
        for name, expected_r, expected_diff in [
            ("pm25", 0.672, -2.1),
            ("pm10", 0.318, -15.7),
        ]:
            n, correlation, difference = pm[name]
            if name == "pm25":
                add_validation(rows, "LAQN PM2.5 matched hours", n, 791, "d", "791", 0)
            else:
                rows.append(("LAQN PM10 matched hours", format(n, "d"), "not specified", "INFO"))
            add_validation(rows, f"LAQN {name.upper()} Pearson r", correlation, expected_r, ".3f", f"~{expected_r:.3f}", 0.005)
            add_validation(rows, f"LAQN {name.upper()} mean difference", difference, expected_diff, "+.2f", f"~{expected_diff:+.1f}", 0.15)

    widths = [max(len(row[index]) for row in (["metric", "reproduced", "expected", "status"], *rows)) for index in range(4)]
    header = ("metric", "reproduced", "expected", "status")
    print(" | ".join(header[index].ljust(widths[index]) for index in range(4)))
    print("-|-".join("-" * width for width in widths))
    for row in rows:
        print(" | ".join(row[index].ljust(widths[index]) for index in range(4)))


def main() -> None:
    args = parse_args()
    project = load_project(args.project_data)
    operational = project_operational(project)
    pms = pms_consistency(project)

    ttn, ttn_source = load_ttn(args.ttn_raw, args.ttn_clean)
    if args.write_clean_ttn:
        if not args.ttn_raw.exists():
            raise FileNotFoundError("--write-clean-ttn requires the local raw TTN export.")
        args.ttn_clean.parent.mkdir(parents=True, exist_ok=True)
        export = ttn.copy()
        export["timestamp"] = export["timestamp"].dt.strftime("%Y-%m-%dT%H:%M:%S.%fZ")
        export.to_csv(args.ttn_clean, index=False, float_format="%.10g")
    ttn_stats = ttn_operational(ttn)

    project_hourly = hourly_project(project)
    weather = weather_comparison(project_hourly, args.weather) if args.weather.exists() else None
    pm = pm_comparison(project_hourly, args.laqn) if args.laqn.exists() else None

    print("Final dissertation analysis validation")
    print(f"Project data: {args.project_data}")
    print(f"TTN source: {ttn_source}")
    print(f"TTN FPort values: {ttn_stats['f_port_values']}")
    if weather is None:
        print("Weather comparison skipped: local Weather Underground input not found.")
    if pm is None:
        print("PM comparison skipped: local LAQN input not found.")
    print()
    print_validation(operational, ttn_stats, weather, pm, pms)


if __name__ == "__main__":
    main()
