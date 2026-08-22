# Data

## `project_sensing_data.csv`

This is the final sensing dataset exported from the project's ThingSpeak channel. It contains 4,961 records from 9 July to 17 August 2026. All five measurement fields are populated in every record.

| Column | Measurement | Unit |
|---|---|---|
| `field1` | Temperature | °C |
| `field2` | Relative humidity | % |
| `field3` | PM2.5 | µg/m³ |
| `field4` | PM10 | µg/m³ |
| `field5` | Atmospheric pressure | hPa |

The observations are unchanged from the final export; only the filename has been made descriptive.

## `ttn_uplinks_clean.csv`

This public analytical dataset contains 318 final TTN uplinks from approximately 15–17 August 2026. It retains the timestamp, frame counter, FPort, decoded measurements and number of gateway receptions for each uplink.

The local raw JSONL export was not published. Device and application identifiers, EUIs and addresses, gateway identifiers and locations, radio metadata, correlation identifiers, raw binary payloads and other network metadata were removed because they are unnecessary for the reported analysis.

## External comparison data

Raw third-party files are intentionally not redistributed. To reproduce the complete comparison, obtain equivalent exports in the formats described in [the analysis documentation](../analysis/README.md):

- Weather Underground personal weather station ILONDO932, 1–16 August 2026, for temperature, relative humidity and pressure.
- London Air Quality Network site TH5 — Tower Hamlets, Victoria Park, for PM2.5 and PM10.

These external sites were not co-located with the project sensing node. The comparison therefore assesses temporal agreement rather than formal sensor calibration.
