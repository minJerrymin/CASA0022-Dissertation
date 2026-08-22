# Final analysis

`final_analysis.py` reconstructs the final Chapter 4 calculations from the supplied project data and local comparison inputs. It calculates each result from the observations; dissertation target values are used only for the printed validation comparison.

## Requirements

- Python 3.10 or later
- Packages listed in `requirements.txt` (`pandas` and `numpy`)

Install and run from the repository root:

```bash
python -m pip install -r analysis/requirements.txt
python analysis/final_analysis.py
```

To regenerate the public TTN analytical CSV from the local raw export:

```bash
python analysis/final_analysis.py --write-clean-ttn
```

## Inputs

| Input | Default path | Public? | Purpose |
|---|---|---:|---|
| Project sensing data | `data/project_sensing_data.csv` | Yes | Operational statistics, hourly project means and PMS consistency |
| Clean TTN data | `data/ttn_uplinks_clean.csv` | Yes | Public fallback for TTN statistics |
| Raw TTN export | `../ttn_uplinks_30days.jsonl` | No | Local regeneration of the clean TTN CSV |
| Weather Underground JSON | `../wunderground_20260801_20260816.json` | No | Temperature, humidity and pressure comparison |
| LAQN CSV | `../Londonair PM DATA.csv` | No | PM2.5 and PM10 comparison |

The three local-only paths assume the supplied source files remain beside the repository. Command-line options can point to equivalent files elsewhere. If the external comparison files are absent, the script still runs the operational, TTN and PMS consistency analyses and reports that the comparisons were skipped.

## Method

ThingSpeak `created_at` values and TTN timestamps are parsed as UTC and sorted chronologically. Consecutive intervals are calculated directly. A stable segment ends whenever the next interval exceeds one hour; the longest segment is the largest elapsed time between its first and last records.

For external comparisons, project observations are converted from UTC to `Europe/London`, floored to the local hour and averaged. Weather Underground observations use the supplied `obsTimeLocal` hour for station ILONDO932. Temperature uses `tempAvg`, humidity uses `humidityAvg`, and pressure is `(pressureMax + pressureMin) / 2`.

LAQN `ReadingDateTime` is parsed day-first as local clock time. Hourly project PM means are matched by local hour to site TH5 (Tower Hamlets, Victoria Park). Missing external values are excluded pairwise, which produces 791 PM2.5 pairs and 779 PM10 pairs in the supplied files.

Pearson's `r` is calculated from each matched pair of numeric series. Mean difference always means `project − external source`. The sites were not co-located, so these are temporal comparisons rather than a formal calibration.

## Outputs

The script prints a concise `metric | reproduced | expected | status` table. `PASS` means the reconstructed value falls within a small, explicitly defined rounding tolerance; `INFO` identifies a useful metric for which the dissertation specification did not state an expected value.
