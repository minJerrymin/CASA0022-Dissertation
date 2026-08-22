# Bill of materials

## Outdoor sensing node

| Item | Quantity / specification |
|---|---|
| Arduino MKR WAN 1310 | 1 |
| Plantower PMS5003 | 1 |
| HDC1080 temperature/humidity module | 1 |
| BMP280 pressure module | 1 |
| Pololu S7V7F5 5 V step-up/step-down regulator | 1 |
| Solar panel | 1 W |
| Rechargeable AA NiMH cells | 3 × 1.2 V, 2000 mAh |
| Battery Charging Board | 1 |
| Capacitors | 2 × 100 µF |
| External LoRa antenna | 1 |
| Stripboard and wiring | As required |
| 3D-printed sensing enclosure | 1 |

The Battery Charging Board LOAD/OUTPUT supplies the MKR WAN 1310 through its battery connector; MKR VIN is not used. The Pololu regulator supplies 5 V to the PMS5003 and its SHDN input is controlled by MKR D5.

The sensing enclosure's concept and functional requirements were developed through discussion involving the project author and Simon. Simon completed the detailed CAD design and 3D printing; those CAD files are not included.

## Indoor display

| Item | Quantity / specification |
|---|---|
| DFRobot FireBeetle 2 ESP32-E | 1 |
| 4.2-inch e-paper display | 1 |
| 3D-printed display enclosure | 1 |

The included [display enclosure CAD](cad/display_enclosure/) was designed by the project author and is distinct from the sensing enclosure.
