# ESP32-P4-WIFI6-Touch-LCD-4.3 ESP-Brookesia Firmware

[中文](README_ZH.md)

Source snapshot for the dated ESP32-P4-WIFI6-Touch-LCD-4.3 Rev3.x factory
image. It provides the Brookesia phone launcher and bundled applications,
including Wi-Fi through ESP-Hosted, camera, audio, music, video, drawing,
spectrum analysis, settings, calculator, and Xiaozhi.

## Requirements

- ESP-IDF v5.5.5
- ESP32-P4 Rev3.x

## Board configuration

| Item | Value |
| --- | --- |
| Display | 4.3-inch ST7701 MIPI-DSI panel |
| Resolution | 480 x 800 |
| Data lanes | 2 |
| Per-lane bit rate | 500 Mbps |
| DPI clock | 30 MHz |
| Touch | GT911 over I2C |

The local board component preserves the product BSP's ST7701 initialization,
panel timings, pins, and lane rate. Its MIPI PHY reference clock is selected by
silicon generation: pre-v3 uses the legacy `MIPI_DSI_PHY_CLK_SRC_DEFAULT`, while
Rev3.x uses `MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT`. The Rev3.x build profile sets
the minimum silicon revision to 3.0 and enables 250 MHz PSRAM.

The GT911 reset is GPIO 23 and the interrupt pin is not connected. The BSP
probes I2C address `0x5D` and then `0x14` before creating the touch device.
This preserves the configuration used by this dated image. The newer Registry
BSP used by the product examples leaves both GT911 INT and RST unassigned; a
future factory image should be rebuilt and released rather than silently
rewriting this snapshot.

## Build

Export ESP-IDF v5.5.5, then run this command from this directory:

```bash
idf.py -B build-lcd-4-3-v5.5.5-rev3_x -D SDKCONFIG="$PWD/build-lcd-4-3-v5.5.5-rev3_x/sdkconfig" -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.rev3_x" build
```

The application binary is named `esp32-p4-lcd-4-3-brookesia.bin`.

## Merge factory image

After a successful build, create the 16 MiB combined image:

```bash
(cd build-lcd-4-3-v5.5.5-rev3_x && python -m esptool --chip esp32p4 merge_bin -o ../../ESP32-P4-WIFI6-Touch-LCD-4.3-FactoryOnly-260820.bin @flash_args)
```

Flash the combined image at offset `0x0`. Compilation does not validate display,
touch, audio, camera, Wi-Fi, or other behavior on physical hardware.
