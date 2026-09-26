# Building and flashing

[Back to the README](../README.md)

This guide is for developers building firmware or flashing a board with a
locally built image. For installation from prebuilt releases, use the
[README Quick start](../README.md#quick-start).

## Set up the workspace

The project uses nRF Connect SDK v3.4.1. Initialize a west workspace with this
repository as its manifest:

```sh
west init -m <this repo url> --mr master battery-switch-ws
cd battery-switch-ws && west update
cd nrf52840-battery-button
west build -b xiao_ble/nrf52840 --sysbuild
```

If the SDK was installed with `nrfutil sdk-manager` (default location
`/opt/nordic/ncs/v3.4.1`), build from the SDK directory using its toolchain:

```sh
cd /opt/nordic/ncs/v3.4.1
nrfutil sdk-manager toolchain launch --ncs-version v3.4.1 -- \
  west build -b xiao_ble/nrf52840 --sysbuild -d <repo>/build <repo>
```

For SEGGER RTT logs (requires an SWD probe), use the debug overlay and the
larger development layout:

```sh
west build -b xiao_ble/nrf52840 --sysbuild -d build-debug -- \
  "-DEXTRA_CONF_FILE=debug.conf;dev/no-factory-data.conf" \
  -DEXTRA_DTC_OVERLAY_FILE=dev/large-app.overlay
```

## Flashing the XIAO over UF2

The XIAO's stock Adafruit UF2 bootloader is retained; no debug probe is needed.
Remove the battery before connecting USB.

1. Double-press reset. A drive named `XIAO-BOOT` or `XIAO-SENSE` appears.
2. Copy the built image to the drive. On macOS use `cp -X`; Finder can crash
   when copying UF2 files, and plain `cp` may hang on large images.
3. The board reboots into the firmware when the copy finishes.

The release application image is `build/battery_switch_app.uf2`. It contains
the application and leaves the factory-data page alone. For a freshly
provisioned device, use its individual image at
`build/devices/<serial>/<serial>.uf2`.

## Other boards and flashing methods

The Feather nRF52840 and Pro Micro nRF52840 overlays are included under
[`boards/`](../boards/). Select the board target and corresponding UF2 target;
other Adafruit UF2-compatible nRF52840 boards need an overlay describing the
flash layout, switch pins, reset pin, and optional status LED. Boards with
SoftDevice S140 v7 start the application at `0x27000`; v6 boards start at
`0x26000`. `INFO_UF2.TXT` on the bootloader drive identifies the version.

For SWD flashing, a J-Link or compatible probe on the XIAO's SWD pads can
program the per-device HEX image on first flash:

```sh
west flash --hex-file build/devices/<serial>/<serial>.hex
```

A plain `west flash` programs only the application and is sufficient for an
update.

### nRF52840 Dongle (advanced)

This board is only build-tested. It has Nordic's USB DFU bootloader instead of
UF2 and needs soldering to run from a battery. Build it with:

```sh
west build -b nrf52840dongle/nrf52840 --sysbuild -d build-dongle
```

The image is `build-dongle/nrf52840-battery-button/zephyr/zephyr.hex`. Enter
the bootloader by pressing the small sideways RESET button (not SW1); the red
LED fades in and out. Flash with nRF Connect Programmer (select *Open DFU
Bootloader*, add the `.hex`, then *Write*) or package it with nrfutil:

```sh
nrfutil install nrf5sdk-tools
nrfutil nrf5sdk-tools pkg generate --hw-version 52 --sd-req=0x00 \
  --application build-dongle/nrf52840-battery-button/zephyr/zephyr.hex \
  --application-version 1 dongle.zip
nrfutil nrf5sdk-tools dfu usb-serial -pkg dongle.zip -p /dev/cu.usbmodemXXXX
```

Use `/dev/ttyACM0` on Linux or `COMx` on Windows for the bootloader serial
port. Switch inputs are on pins 0.02, 0.29, 0.31, 1.13, 1.15, and 1.10;
holding SW1 resets the device. The green LED is the status LED.

Out of the box, the Dongle uses USB 5 V and high-voltage mode. For AAA or
CR2032 battery power on VDD, change the solder bridges as described in the
nRF52840 Dongle user guide. Until then its reported battery level is not
meaningful because VDD is regulated to 3.0 V. See
[`boards/nrf52840dongle.overlay`](../boards/nrf52840dongle.overlay) for its
flash layout.

## Battery configuration

Choose the battery chemistry with `west build -t menuconfig` → *Battery switch
application → Battery*, or set it in `prj.conf`:

```conf
CONFIG_APP_BATTERY_CR2032=y
# Or: APP_BATTERY_2XAAA_ALKALINE (default),
# APP_BATTERY_2XAAA_CARBON_ZINC, APP_BATTERY_2XAAA_NIMH
```

Other application options configure momentary/latching debounce, long-press
time, multi-press window and maximum count, latching poll interval, battery
measurement interval, and warning/critical thresholds.

## Per-device pairing codes

Every device needs a unique Matter setup code. Default release firmware creates
random factory data on first boot. For batches with printed labels, generate
one image and credential set per device:

```sh
nrfutil sdk-manager toolchain launch --ncs-version v3.4.1 -- \
  python3 tools/provision_device.py --build-dir build --count 5
```

Each `build/devices/<serial>/` directory contains a UF2 image, HEX image, QR
PNG, and text file with the manual pairing code. Keep the QR and text file
private and with the device; they are required for commissioning. To rebuild an
image with the same credentials:

```sh
python3 tools/provision_device.py --build-dir build \
  --from-device build/devices/<serial>
```

The code-generating utility can also erase the stored code so firmware creates
a replacement on reboot:

```sh
tools/provision_device.py --build-dir build --blank new-pairing-code.uf2
```

## Making a release

The GitHub Actions workflow builds UF2 files for XIAO, Feather, and Pro Micro
on every push and pull request. Pushing a version tag publishes the artifacts
as a GitHub release:

```sh
git tag v1.0.0
```
