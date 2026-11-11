# Pico 2 WH (Zephyr 4.5.0-rc3)

Target: rpi_pico2/rp2350a/m33_0/w (H means fitted headers).
Build in the Ubuntu-24.04 VS Code Remote WSL window.
Tasks: app: build Pico 2 WH; app: build Pico 2 WH (debug.conf).
The original RP2040 tasks and application configuration remain available.
New tasks use /home/dai/zephyr-venv and this workspace Zephyr tree.
Normal output: app/build-pico2-wh/zephyr/zephyr.uf2
Debug output: app/build-pico2-wh-debug/zephyr/zephyr.uf2
No hardware was flashed.

Overlay: app/boards/rpi_pico2_rp2350a_m33_0_w.overlay
Extra config: app/pico2_wh.conf, applied after prj.conf.
Wi-Fi: built-in board CYW43439 AIROC driver on PIO0; WS2812 uses PIO1.
Relay GP14..21, buzzer GP6, WS2812 GP13 are retained.
Flash: 4 MiB, mapped code offset 0 size 0x3f0000; LittleFS offset
0x3f0000 size 0x10000. No RP2040 256-byte boot2 partition.
RTC: zephyr,rtc-emul software device, since this tree has no RP2350 RTC
node/driver. Time must be restored with SNTP after reset/power loss.
RP2350 hardware entropy replaces the RP2040 test random source.

Python build requirements were installed from zephyr/scripts/requirements-base.txt.
This fixes the missing jsonschema root cause of the west boards traceback.
Wi-Fi blobs were fetched using west blobs fetch hal_infineon.
Original configuration backup: .pico2-backups/20261025-122443/
VS Code Python/Zephyr/IntelliSense paths now point to this workspace and
the Pico 2 normal build. Previous settings are in the backup.
Hardware Wi-Fi/USB/relay/LED/filesystem behavior is not yet tested.
