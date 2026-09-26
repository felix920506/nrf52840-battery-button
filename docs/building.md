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

The debug build doesn't fit next to the factory data, which is why it uses the
SDK's test credentials and the larger layout. Image sizes with nRF Connect SDK
v3.4.1:

| Build | Flash | App partition | RAM |
|---|---|---|---|
| default (low power, USB pairing info) | 577 KB | 608 KB | 164 KB |
| debug (no factory data, `dev/large-app.overlay`) | 625 KB | 788 KB | 161 KB |

## Flashing the XIAO over UF2

The XIAO's stock Adafruit UF2 bootloader is retained; no debug probe is needed.
Remove the battery before connecting USB.

1. Double-press reset. A drive named `XIAO-BOOT` or `XIAO-SENSE` appears.
2. Copy the built image to the drive. On macOS use `cp -X`; Finder can crash
   when copying UF2 files, and plain `cp` may hang on large images.
3. The board reboots into the firmware when the copy finishes.

The release application image is `build/battery_switch_app.uf2`. It contains
the application and leaves the factory-data page alone, so a device keeps its
pairing code. For a device provisioned with a printed label, use its
individual image at `build/devices/<serial>/<serial>.uf2` for the first flash.

Reflashing keeps the settings, so the device stays commissioned. To start
over, factory reset it (see the [user guide](user-guide.md#factory-reset-and-pairing-code)).

### Flash layout

From [`boards/uf2_matter_layout.dtsi`](../boards/uf2_matter_layout.dtsi):

| Address | Contents |
|---|---|
| `0x00000–0x26FFF` | MBR + SoftDevice area of the bootloader (not touched) |
| `0x27000–0xBEFFF` | application (608 KB) |
| `0xBF000–0xBFFFF` | Matter factory data |
| `0xC0000–0xEBFFF` | unused |
| `0xEC000–0xF3FFF` | settings: Matter fabrics, Thread network, switch types |
| `0xF4000–0xFFFFF` | UF2 bootloader (not touched) |

The factory data sits directly behind the application, so both together form
a single contiguous image of about 620 KB. That is small enough for the
bootloader's serial DFU as well (see
[Serial DFU](development.md#serial-dfu-and-the-1200-baud-touch)). If the
application outgrows its partition, the link fails: move the factory data up.

## Other boards and flashing methods

The firmware isn't tied to the XIAO. Any nRF52840 board with the Adafruit UF2
bootloader works; overlays for the Adafruit Feather nRF52840 and the Pro Micro
nRF52840 are included under [`boards/`](../boards/). Use the board's Zephyr
UF2 target (e.g. `adafruit_feather_nrf52840/nrf52840/uf2`,
`promicro_nrf52840/nrf52840/uf2`) and, for a new board, add an overlay
following [`boards/xiao_ble.overlay`](../boards/xiao_ble.overlay). An overlay
needs:

1. **The flash layout.** Boards shipped with SoftDevice S140 v7 start their
   application at `0x27000`, boards with S140 v6 at `0x26000`. `INFO_UF2.TXT`
   on the bootloader drive shows the version.
   ```dts
   #define UF2_APP_START 0x26000   /* S140 v6 */
   #include "uf2_matter_layout.dtsi"
   ```
2. **The `switches` node**, listing the board's pins you wire the switches to.
   Removing a switch node disables its Matter endpoint; endpoints are numbered
   in node order, the first node being endpoint 1.
3. **The factory reset pad** (`factory-reset-gpios` in `zephyr,user`), a
   `status-led` alias if the board has an LED, and
   `#include "battery_switch_common.dtsi"` at the end (battery measurement and
   the USB serial port).

Build, provision and flash exactly as for the XIAO.

### Flashing with an SWD probe

A J-Link or another probe on the SWD pads under the XIAO works too, and leaves
the bootloader alone. For a first flash, program the device's image:

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
port. Wiring (the pads are labelled with their pin numbers): switch inputs on
0.02, 0.29, 0.31, 1.13, 1.15 and 1.10; **factory reset** by holding SW1 for
5 s; status on the green LED. After flashing, the Dongle's USB serial port
shows the pairing code as on the other boards.

Updates work the same way and keep the pairing code, the pairings and the
settings: the bootloader replaces the application in place and leaves the
pages above it alone. For a new pairing code, flash the application together
with an erased pairing code page instead:

```sh
tools/provision_device.py --build-dir build-dongle --blank-hex new-code.hex
```

The UF2 `new-pairing-code.uf2` from the release doesn't work on the Dongle.

Out of the box, the Dongle uses USB 5 V and high-voltage mode. For AAA or
CR2032 battery power on VDD, change the solder bridges as described in the
nRF52840 Dongle user guide. Until then its reported battery level is not
meaningful because VDD is regulated to 3.0 V.

Layout ([`boards/nrf52840dongle.overlay`](../boards/nrf52840dongle.overlay)):
application `0x01000–0xBEFFF` (760 KB), factory data `0xBF000`, settings
`0xD8000–0xDFFFF` directly below the bootloader at `0xE0000`.

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

Every device needs its own Matter setup code. The Matter SDK's test code
(passcode `20202021`) is public: anyone in BLE range could commission a device
using it while its commissioning window is open. So the build doesn't contain
any pairing credentials. They live in the Matter factory data page
(`0xBF000`), which firmware updates don't touch, and get there in one of two
ways.

**Automatically, on first boot** (the default, used by the release files).
When the factory data page is empty,
[`src/transport/matter/self_provision.cpp`](../src/transport/matter/self_provision.cpp)
creates it before Matter starts: a random setup passcode from the hardware
RNG (skipping the values the specification forbids), a random discriminator
and SPAKE2+ salt with the SPAKE2+ verifier, and the chip's factory-programmed
device ID as serial number. The result has the same format as the SDK's
factory data generator produces. The passcode is stored too, so the device
can show its code on the USB serial port
([`src/core/usb_info.c`](../src/core/usb_info.c)). USB is only switched on
while USB power is present, so this costs nothing on battery.

**With printed labels,** for a batch of devices:
[`tools/provision_device.py`](../tools/provision_device.py) creates a firmware
image per device, each with a random passcode, discriminator, SPAKE2+ salt and
a unique serial number. Only the SPAKE2+ verifier is stored on the device,
never the passcode, so the code can't be read back from the chip; the USB
serial port then refers to the label.

```sh
nrfutil sdk-manager toolchain launch --ncs-version v3.4.1 -- \
  python3 tools/provision_device.py --build-dir build --count 5
```

For each device, `build/devices/<serial>/` then contains:

| File | Use |
|---|---|
| `<serial>.uf2` | first flash (application + this device's factory data) |
| `<serial>.hex` | the same, for serial DFU or an SWD probe |
| `<serial>.png` | pairing QR code, e.g. to print as a label |
| `<serial>.txt` | QR code payload and manual pairing code |

**Keep the QR code and the `.txt` file private, and with the device.** They
are the only way to commission it. If you lose them, provision the device
again: the new image carries a new code.

To fully reflash an already provisioned device with a newer build (for example
over serial DFU, which writes the whole image), rebuild its image with the
same credentials:

```sh
python3 tools/provision_device.py --build-dir build \
  --from-device build/devices/<serial>
```

The same tool builds `new-pairing-code.uf2`, the release file that erases the
factory data page so the firmware creates a new code on the next boot
(`--blank-hex` produces the HEX equivalent for the Dongle):

```sh
python3 tools/provision_device.py --build-dir build --blank new-pairing-code.uf2
```

## Making a release

[`.github/workflows/firmware.yml`](../.github/workflows/firmware.yml) builds
the UF2 files for the XIAO, the Feather nRF52840 and the Pro Micro nRF52840 on
every push and pull request; they can be downloaded from the run's artifacts.
Each board builds on its own runner, in parallel. Pushing a tag also publishes
them, together with `new-pairing-code.uf2`, as a GitHub release named after
the tag:

```sh
git tag v1.0.0
git push origin v1.0.0
```

The firmware reports the tag as its software version to Matter controllers,
or the commit hash for a build that isn't a release.
