# nRF52840 battery switch

Battery powered Matter-over-Thread switch for the **Seeed Studio XIAO nRF52840**.
It supports up to **6 external switches**, and each one can be a momentary push
button or a latching rocker/toggle switch. Every switch shows up in Matter
controllers as its own *Generic Switch* endpoint. The battery shows up as a
*Power Source*.

It is built on nRF Connect SDK v3.4.1. The Matter stack only runs on
Zephyr, so the RTOS can't be avoided. The application itself is kept small: one
event loop in the main thread, no extra threads, and interrupts that only
queue events.

## Quick start

No build and no tools are needed: every device creates its own pairing code
the first time it starts.

> [!CAUTION]
> **macOS: don't copy firmware files to the board with Finder.** Dragging a
> UF2 file onto the board's USB drive can send Finder into a crash loop that
> only restarting the Mac ends. Copy it in Terminal instead:
>
> ```sh
> cp -X battery-switch-xiao-nrf52840.uf2 /Volumes/XIAO-BOOT/
> ```
>
> Use your board's file and drive name (`ls /Volumes` shows it). An
> `Input/output error` at the end of the copy is normal: the board restarts
> as soon as it has received the file.

1. Download the UF2 file for your board from the
   [latest release](../../releases/latest):

   | Board | File | Switches | Factory reset |
   |---|---|---|---|
   | Seeed XIAO nRF52840, Sense, Plus | `battery-switch-xiao-nrf52840.uf2` | D0–D5 | hold D9 to GND |
   | Adafruit Feather nRF52840 Express, Sense | `battery-switch-feather-nrf52840.uf2` | A0–A5 | hold USER button |
   | Pro Micro nRF52840, nice!nano, SuperMini | `battery-switch-promicro-nrf52840.uf2` | P0.17, P0.20, P0.22, P0.24, P1.00, P0.11 | hold P1.06 to GND |

   Only the XIAO file has been tested on hardware; the others are
   build-tested. The pins are listed in the board's overlay in
   [`boards/`](boards/). The Feather and Pro Micro files expect the
   bootloader with SoftDevice S140 v6, the XIAO files S140 v7 (they ship like
   that; `INFO_UF2.TXT` on the bootloader drive shows the version). The
   nRF52840 Dongle can run the firmware too, but only for
   [advanced users](#nrf52840-dongle-advanced) who build and flash it
   themselves.
2. Connect the board over USB (no battery connected) and double-press reset.
   A USB drive appears. Copy the UF2 file onto it (on macOS in Terminal with
   `cp -X`, **not with Finder**, see the caution above). The board restarts
   with the switch firmware; the LED flashes 3 times.
3. Open the board's USB serial port in a terminal:
   * macOS: `screen /dev/cu.usbmodem* 115200` (quit with Ctrl-A, then K)
   * Linux: `screen /dev/ttyACM0 115200`
   * Windows: PuTTY or the Arduino serial monitor on the board's COM port

   It prints the pairing QR code and the pairing code (close and reopen the
   port to print them again):
   ```
   Battery Switch
   Serial number: 1145D7F7118F8DDB
   Status: not paired yet

   Scan this QR code in your smart home app, or enter the code below.

       █▀▀▀▀▀█  ▄▀ ▀ █▀▀▀▀▀█
       █ ███ █ ▀▄▀█▄ █ ███ █
       ...

     QR code: MT:XXXXXXXXXXXXXXXXXXX
     Pairing code: XXXX-XXX-XXXX
   ```
4. Add the device in your Matter controller (Home Assistant, Apple Home,
   Google Home, …) with the QR code or the pairing code. It is a Thread
   device, so the controller needs a Thread border router. You can do this
   while it is still on USB power.
5. Unplug USB and connect the battery (see [Hardware](#hardware)). The device
   stays paired.

The pairing code is kept across firmware updates and factory resets. Keep it
private: while the device is not paired, anyone in Bluetooth range who knows
it can add the device. Anyone with physical USB access to the device can
read it.

### Updating a device

An update keeps everything: the pairing code, the pairings with your
controllers, the Thread network and the switch types. The device doesn't need
to be added again.

1. Download the UF2 file for your board from the
   [latest release](../../releases/latest), the same file name as for the
   first install. Not `new-pairing-code.uf2`: that one gives the device a
   new pairing code (see [Getting a new pairing code](#getting-a-new-pairing-code)).
2. Disconnect the battery, then connect the board over USB.
3. Double-press reset. On boards without a reset button (Pro Micro,
   nice!nano, SuperMini) short the RST pad to GND twice quickly. The USB
   drive appears.
4. Copy the UF2 file onto the drive. The board restarts with the new
   firmware; the LED flashes 3 times.

   > [!CAUTION]
   > On macOS, copy it in Terminal with `cp -X`, **not with Finder**: Finder
   > can end up in a crash loop that only restarting the Mac ends.
5. Unplug USB and reconnect the battery. The device rejoins the Thread
   network by itself; this can take a minute.

Your controller shows the installed firmware version on the device page
(Home Assistant: *Firmware*): the release tag, or the commit hash for a
build that isn't a release. The hardware version shows the board the
firmware was built for.

### Getting a new pairing code

The pairing code never changes on its own: it stays across firmware updates
and factory resets, and you can always read it again on the USB serial port.
Replace it if someone else may know it, for example before you give the
device away.

A new code doesn't remove existing pairings: controllers that already have
the device keep it. To start over completely, remove the device from your
controllers (and factory reset it) as well.

1. Download `new-pairing-code.uf2` from the
   [latest release](../../releases/latest). It works for every UF2 board:
   it only erases the page that holds the pairing code (`0xBF000`), not the
   firmware or the settings.
2. Disconnect the battery, connect the board over USB and double-press
   reset. The USB drive appears.
3. Copy `new-pairing-code.uf2` onto the drive (on macOS with `cp -X` in
   Terminal, not with Finder).
4. The board restarts and creates a new pairing code; the LED flashes 3
   times. If the USB drive is still there instead, copy the firmware file for
   your board (as for an update) as well.
5. Open the USB serial port as in the [Quick start](#quick-start): it shows
   the new QR code and pairing code. The old code no longer works.

To make the file yourself: `tools/provision_device.py --build-dir build
--blank new-pairing-code.uf2`.

## Hardware

```
               XIAO nRF52840
            ┌─────────────────┐
 Switch 1 ──┤ D0          5V  │
 Switch 2 ──┤ D1         GND  ├── Battery −, all switches' other terminal
 Switch 3 ──┤ D2         3V3  ├── Battery +
 Switch 4 ──┤ D3         D10  │
 Switch 5 ──┤ D4          D9  │
 Switch 6 ──┤ D5          D8  │
            │ D6          D7  │
            └─────────────────┘
```

* Wire each switch between its pin and **GND**. It uses the internal pull-up,
  so you don't need any external parts.
* Works on the XIAO nRF52840, Sense and Plus. They share the chip, flash and
  D0–D10 pinout, and all build with the `xiao_ble/nrf52840` target.
* **Battery → 3V3 pin** (not BAT, not 5V). The nRF52840 runs directly from
  1.7 to 3.6 V, and the firmware measures the battery on its internal VDD rail,
  so you don't need a voltage divider. Supported batteries:
  * 1× CR2032 (3 V)
  * 2× AAA in series: alkaline (up to 3.2 V when fresh), carbon-zinc or NiMH
* **CR2032:** put a ≥100 µF low-leakage capacitor across 3V3/GND. Coin cells
  can't supply the radio's current peaks well, especially during
  commissioning.
* Unplug USB when running on battery. Don't connect USB and a battery at the
  same time.
* **Factory reset pad: D9** (P1.14). Leave it unconnected, or wire a small
  service button from D9 to GND inside the enclosure. It isn't a switch input,
  so no switch can trigger a reset by accident.

Switches and pins are set in
[`boards/xiao_ble.overlay`](boards/xiao_ble.overlay). Each switch also has a
type. The overlay sets its default, and you can change it later from the smart
home app (see [Changing the switch type](#changing-the-switch-type)):

```dts
switch_3 {
	gpios = <&gpio0 28 (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>;
	label = "Switch 3 (D2)";
	switch-type = "latching";
};
```

| `switch-type` | Use for | Reported as |
|---|---|---|
| `"momentary"` (default) | push buttons | press, release, long press, multi press |
| `"latching"` | rockers and toggles | a short press on every change of position |

A latching switch reports every flip as a short press, not as a position, so
a controller can use it as a toggle. Flipping it twice quickly counts as a
double press; long press isn't possible. This also suits switches whose
position you can't read from the switch:

* plain-faced rockers, e.g. Panasonic Deco Lite, and older
  Panasonic/National/Jimbo Japanese-style module switches;
* switches that look like push buttons but latch electrically, e.g. Schneider
  ZenCelo, Panasonic Cosmo Art, Rinsa, Glatima.

There is no type that reports a rocker's position (Matter `SwitchLatched`).
Controllers such as Home Assistant can't follow a switch that changes to it,
and a position is rarely what automations need.

If you remove a switch node, its Matter endpoint is disabled. Endpoints are
numbered in node order: the first node is endpoint 1.

### Changing the switch type

Each switch endpoint has a Matter **Mode Select** cluster named "Switch type",
with two modes:

| Mode | Switch type |
|---|---|
| 0 | Momentary (push button) |
| 2 | Latching (rocker, toggle) |

Mode 1 was an earlier type that reported the rocker's position; it no longer
exists. A switch stored with it switches to mode 2 on the next boot.

* **Home Assistant** shows it as a dropdown on each switch's device page.
* **Controllers without Mode Select support** (e.g. Apple Home) don't show it.
  Change the type from another controller on the same device, or with
  `chip-tool modeselect change-to-mode <mode> <node-id> <endpoint>`.

The device applies the new type immediately. It changes the electrical handling
(e.g. the power-saving polling of closed latching switches), the Switch cluster
feature map and the events. The type is stored in flash and survives reboots,
firmware updates and factory resets: it describes the wiring, which doesn't
change when you re-pair.

Controllers may keep using the old switch behaviour until they re-read the
device. This Matter SDK can't flag the change through `ConfigurationVersion`
yet.

**Home Assistant** decides a switch's event types only when it creates the
entity. It never recreates the entity for a known device: re-interviewing and
power cycling don't help. Both switch types report `multi_press_1`,
`multi_press_2`, …, so presses keep working after a change. Only the long press
events of a momentary switch appear or disappear. To update those, reload the
Matter integration (*Settings → Devices & services → Matter → ⋮ → Reload*) or
restart Home Assistant.

## Matter data model

| Endpoint | Device type | Clusters |
|---|---|---|
| 0 | Root Node, Power Source | …, Power Source (battery, replaceable) |
| 1–6 | Generic Switch | Identify, Descriptor (tag list "1"…"6"), Switch, Mode Select ("Switch type") |

Controllers receive these **Switch cluster events**:

| Switch type | Feature map | Events |
|---|---|---|
| momentary | MS, MSR, MSL, MSM (`0x1E`) | `InitialPress`, `ShortRelease`, `LongPress`, `LongRelease`, `MultiPressOngoing`, `MultiPressComplete` |
| latching | MS, MSR, MSM (`0x16`) | per change of position: `InitialPress`, `ShortRelease`; then `MultiPressComplete(n)` |

Momentary sequences follow the Matter spec and the TC-SWTCH-2.4/2.5
certification tests:

* single press: `InitialPress`, `ShortRelease`, then `MultiPressComplete(1)` after the multi-press window
* double press: `InitialPress`, `ShortRelease`, `InitialPress`, `MultiPressOngoing(2)`, `ShortRelease`, `MultiPressComplete(2)`
* hold: `InitialPress`, `LongPress`, then `LongRelease` when released (no `MultiPressComplete`)
* more presses than `MultiPressMax`: `MultiPressComplete(0)`

`CurrentPosition` follows the switch. The Power Source cluster reports
`BatVoltage`, `BatPercentRemaining`, `BatChargeLevel` and
`BatReplacementNeeded`.

## Building and flashing

Set up the workspace. This fetches nRF Connect SDK v3.4.1 next to this repo:

```sh
west init -m <this repo url> --mr master battery-switch-ws
cd battery-switch-ws && west update
cd nrf52840-battery-button
west build -b xiao_ble/nrf52840 --sysbuild
```

If you installed the SDK with `nrfutil sdk-manager` instead (by default it goes
in `/opt/nordic/ncs/v3.4.1`), build from inside that SDK directory, using its
toolchain:

```sh
cd /opt/nordic/ncs/v3.4.1
nrfutil sdk-manager toolchain launch --ncs-version v3.4.1 -- \
  west build -b xiao_ble/nrf52840 --sysbuild -d <repo>/build <repo>
```

For logs over SEGGER RTT (needs an SWD probe), use the debug overlay. It
doesn't fit next to the factory data, so build it with the SDK's test
credentials and the larger development layout:

```sh
west build -b xiao_ble/nrf52840 --sysbuild -d build-debug -- \
  "-DEXTRA_CONF_FILE=debug.conf;dev/no-factory-data.conf" \
  -DEXTRA_DTC_OVERLAY_FILE=dev/large-app.overlay
```

Resulting image sizes with nRF Connect SDK v3.4.1:

| Build | Flash | App partition | RAM |
|---|---|---|---|
| default (low power, USB pairing info) | 577 KB | 608 KB | 164 KB |
| debug (no factory data, `dev/large-app.overlay`) | 625 KB | 788 KB | 161 KB |

### Making a release

[`.github/workflows/firmware.yml`](.github/workflows/firmware.yml) builds the
UF2 files for the XIAO, the Feather nRF52840 and the Pro Micro nRF52840 on
every push and pull request (downloadable from the run's artifacts). Each board builds on its own
runner, in parallel. Pushing a tag also publishes them, together with
`new-pairing-code.uf2`, as a GitHub release named after the tag:

```sh
git tag v1.0.0
git push origin v1.0.0
```

### Per-device pairing codes

Every device needs its own Matter setup code. The Matter SDK's test code
(passcode `20202021`) is public: anyone in BLE range could commission a device
using it while its commissioning window is open. So the build doesn't contain
any pairing credentials. They live in the Matter factory data page
(`0xBF000`), which firmware updates don't touch, and get there in one of two
ways.

**Automatically, on first boot** (the default, used by the release files).
When the factory data page is empty,
[`src/transport/matter/self_provision.cpp`](src/transport/matter/self_provision.cpp)
creates it before Matter starts:

* a random setup passcode (hardware RNG, from the whole valid range, skipping
  the values the specification forbids),
* a random discriminator and SPAKE2+ salt, and the SPAKE2+ verifier,
* the chip's factory-programmed device ID as serial number.

The result has the same format as the SDK's factory data generator produces,
so from then on the device behaves exactly like a provisioned one. The
passcode is stored too, so the device can show its code on the USB serial
port ([`src/core/usb_info.c`](src/core/usb_info.c)): when a terminal opens
the port, it prints the QR code and the pairing code. USB is only switched on
while USB power is present, so this costs nothing on battery.

**With printed labels,** for a batch of devices:
[`tools/provision_device.py`](tools/provision_device.py) creates a firmware
image per device. For each device it picks:

* a random setup passcode (from the whole valid range, skipping the values
  the specification forbids),
* a random discriminator,
* a random SPAKE2+ salt,
* a unique serial number.

It writes them into that device's factory data. Only the SPAKE2+ verifier is
stored on the device, never the passcode, so the code can't be read back from
the chip; the USB serial port then refers to the label.

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
python3 tools/provision_device.py --build-dir build --from-device build/devices/<serial>
```

### Flashing over USB (UF2 bootloader)

The build keeps the XIAO's stock Adafruit UF2 bootloader. You don't need a
debug probe:

1. Connect the XIAO over USB. Remove the battery first.
2. Double-press the reset button. A USB drive appears, named `XIAO-BOOT` or
   `XIAO-SENSE` depending on the bootloader version.
3. Copy the file to the drive with `cp -X`; on macOS, a plain `cp` can hang
   on large files. The XIAO reboots into the firmware once the copy
   finishes.

   > [!CAUTION]
   > **Never use Finder on macOS for this.** Dragging a UF2 file onto the
   > drive can send Finder into a crash loop that only restarting the Mac
   > ends.

   * `build/battery_switch_app.uf2` (the release file): first flash and
     firmware updates. It contains only the application and leaves the
     factory data page alone, so a device keeps its pairing code.
   * Or, for a device with a printed label: its own
     `build/devices/<serial>/<serial>.uf2` for the first flash.

Flash layout ([`boards/uf2_matter_layout.dtsi`](boards/uf2_matter_layout.dtsi)):

| Address | Contents |
|---|---|
| `0x00000–0x26FFF` | MBR + SoftDevice area of the bootloader (not touched) |
| `0x27000–0xBEFFF` | application (608 KB) |
| `0xBF000–0xBFFFF` | Matter factory data |
| `0xC0000–0xEBFFF` | unused |
| `0xEC000–0xF3FFF` | settings: Matter fabrics, Thread network, etc. |
| `0xF4000–0xFFFFF` | UF2 bootloader (not touched) |

The factory data sits directly behind the application, so both together form
a single image of about 620 KB. The bootloader's serial DFU also accepts that
(see [Bluetooth LE test build](#bluetooth-le-test-build) for how to use it).

Reflashing the UF2 keeps the settings, so the device stays commissioned. To
start over, factory reset it (see below).

### Other nRF52840 boards with the UF2 bootloader

The firmware isn't tied to the XIAO. Any nRF52840 board with the Adafruit UF2
bootloader works: Adafruit Feather nRF52840 and ItsyBitsy, Pro Micro/nice!nano
style boards, and so on. It flashes by copying the UF2 file, with no probe
needed. Use the board's Zephyr UF2 target (e.g.
`adafruit_feather_nrf52840/nrf52840/uf2`, `promicro_nrf52840/nrf52840/uf2`) and
add an overlay for it in `boards/`, following
[`boards/xiao_ble.overlay`](boards/xiao_ble.overlay). Overlays for the
Feather nRF52840 and the Pro Micro nRF52840 are included. An overlay needs:

1. **The flash layout.** Boards shipped with SoftDevice S140 v7 start their
   application at 0x27000, boards with S140 v6 at 0x26000. `INFO_UF2.TXT` on
   the bootloader drive shows the version.
   ```dts
   #define UF2_APP_START 0x26000   /* S140 v6 */
   #include "uf2_matter_layout.dtsi"
   ```
2. **The `switches` node**, listing the board's pins you wire the switches to.
3. **The factory reset pad** (`factory-reset-gpios` in `zephyr,user`), a
   `status-led` alias if the board has an LED, and
   `#include "battery_switch_common.dtsi"` at the end (battery measurement and
   the USB serial port).

Build, provision and flash exactly as for the XIAO.

### nRF52840 Dongle (advanced)

> [!WARNING]
> **Advanced, at your own risk.** The code path for the nRF52840 Dongle
> (PCA10059) is included, but no release file is built for it and it has
> only been build-tested. It has Nordic's USB DFU bootloader instead of a UF2
> bootloader, so flashing it needs Nordic's tools, and running it from a
> battery needs soldering.

Build it yourself:

```sh
west build -b nrf52840dongle/nrf52840 --sysbuild -d build-dongle
```

The image is `build-dongle/nrf52840-battery-button/zephyr/zephyr.hex`. To
flash it, plug the dongle in and press its RESET button, the small one that
is pushed sideways (not SW1); the red LED fades in and out. Then either:

* **nRF Connect Programmer** (in [nRF Connect for
  Desktop](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Desktop)):
  select the dongle (*Open DFU Bootloader*), *Add file* → the `.hex`,
  *Write*.
* **nrfutil:**
  ```sh
  nrfutil install nrf5sdk-tools
  nrfutil nrf5sdk-tools pkg generate --hw-version 52 --sd-req=0x00 \
    --application build-dongle/nrf52840-battery-button/zephyr/zephyr.hex \
    --application-version 1 dongle.zip
  nrfutil nrf5sdk-tools dfu usb-serial -pkg dongle.zip -p /dev/cu.usbmodemXXXX
  ```
  (`/dev/ttyACM0` on Linux, `COMx` on Windows: the bootloader's serial port.)

Wiring (the pads are labelled with their pin numbers): switches on 0.02,
0.29, 0.31, 1.13, 1.15 and 1.10, factory reset by holding SW1, status on the
green LED. After flashing, the dongle's USB serial port shows the pairing code
as on the other boards.

Updates work the same way and keep the pairing code, the pairings and the
settings: the bootloader replaces the application in place and leaves the
pages above it alone. For a new pairing code, flash the application together
with an erased pairing code page instead:
`tools/provision_device.py --build-dir build-dongle --blank-hex new-code.hex`
(the UF2 `new-pairing-code.uf2` doesn't work on the dongle).

**Power:** out of the box the dongle runs from USB 5 V, with the nRF52840 in
high voltage mode. For 2x AAA or a CR2032 on VDD, change its solder bridges
for an external supply as described in the nRF52840 Dongle user guide. Until
then the battery level it reports isn't meaningful (VDD is regulated to
3.0 V).

Layout ([`boards/nrf52840dongle.overlay`](boards/nrf52840dongle.overlay)):
application `0x01000–0xBEFFF` (760 KB), factory data `0xBF000`, settings
`0xD8000–0xDFFFF` directly below the bootloader at `0xE0000`.

### Flashing with an SWD probe

A J-Link or another probe on the SWD pads under the XIAO works too, and leaves
the bootloader alone. For a first flash, program the device's image:
`west flash --hex-file build/devices/<serial>/<serial>.hex`. A plain
`west flash` programs only the application, which is enough for an update.

Choose the battery type with `west build -t menuconfig` → *Battery switch
application → Battery*, or in `prj.conf`:

```
CONFIG_APP_BATTERY_CR2032=y            # or APP_BATTERY_2XAAA_ALKALINE (default),
                                       # APP_BATTERY_2XAAA_CARBON_ZINC, APP_BATTERY_2XAAA_NIMH
```

Other options, under *Battery switch application*: debounce time (set
separately for momentary and latching switches, both 20 ms by default),
long-press time, multi-press window and max count, latching switch poll
interval, battery measurement interval, and warning/critical thresholds.

## Using it

* **Commissioning:** after the first boot the device advertises over BLE for 15
  minutes, and the blue LED blinks briefly every 2 s. Pressing any switch
  restarts advertising while the device isn't commissioned. In the controller
  app (Apple Home, Google Home, Home Assistant, …), scan the device's QR code,
  or enter its pairing code: shown on the USB serial port (see
  [Quick start](#quick-start)), or, for devices provisioned with
  `tools/provision_device.py`, in `build/devices/<serial>/`. It uses the Matter **test** vendor/product ID and
  development attestation certificates, so controllers show it as an
  uncertified test device.
* **Startup:** the blue LED flashes 3 times when the firmware starts
  (`CONFIG_APP_BOOT_FLASHES`, 0 to disable), then stays off.
* **Identify:** the blue LED blinks.
* **Factory reset:** hold the **reset pad D9** to GND for 5 s
  (`CONFIG_APP_FACTORY_RESET_HOLD_MS`). The LED flashes once when the pad is
  detected and five times at the reset. This removes all Matter fabrics and the
  Thread network; switch types and the pairing code stay. The pad is also
  checked at boot, so it works even if the device keeps restarting.

## Power design

* Thread **Sleepy End Device** and Matter **ICD** (LIT capable; it runs as SIT
  with a 5 s slow poll until a controller registers for check-ins). TX power
  is 0 dBm.
* Switch pins wake the CPU through the GPIO SENSE mechanism (level
  interrupts), not GPIOTE IN channels. SENSE draws no current while idle.
* A **closed latching switch** would draw about 230 µA through the pull-up
  indefinitely. Instead, the pin is disconnected and sampled for about 10 µs
  every 100 ms (`CONFIG_APP_LATCH_POLL_INTERVAL_MS`), which costs roughly 1 µA.
  The trade-off is up to 100 ms of latency when the switch is opened.
  Momentary buttons draw pull-up current only while held.
* The battery is measured once per hour on the internal VDD channel, with no
  divider.
* The on-board 2 MB QSPI flash is put into deep power-down. UART, I²C, SPI, PWM
  and USB are disabled, and so are logging and the console, unless you build
  with `debug.conf`.

The XIAO's own regulator and charger circuitry sit on the 3V3 rail. Measure the
sleep current of your board with a power profiler; it's the real limit on
battery life.

## Code layout

```
src/main.c                    event loop, switch type changes, battery timer
src/core/app_loop.*           ISR-safe event queue
src/core/switch_input.*       GPIO, debounce, low-power polling of closed rockers
src/core/switch_gesture.*     press/long/multi-press state machine (transport independent)
src/core/battery.*            VDD measurement, per-chemistry discharge curves
src/core/status_led.*         LED patterns
src/core/switch_config.*      stored switch types (settings)
src/core/reset_pin.*          factory reset pad
src/core/usb_info.*           USB serial port showing the pairing code and QR code
src/transport/transport.h     interface to the radio protocol
src/transport/matter/         Matter over Thread implementation, first-boot pairing code
src/third_party/qrcodegen/    QR code encoder (Project Nayuki, MIT)
boards/                       per-board pins and flash layout
.github/workflows/            builds the release UF2 files
src/default_zap/              Matter data model (.zap) and generated code
sysbuild.cmake                builds battery_switch_app.uf2 (application only)
dev/                          development-only config overlays (see Testing on a Mac)
tools/                        provisioning with labels, BLE test client, SRP-to-mDNS bridge,
                              Thread dataset provisioning
```

### Adding Zigbee or BLE later

The core has no Matter dependencies. It emits `struct switch_event`s and
`struct battery_state`s through
[`src/transport/transport.h`](src/transport/transport.h). To add a transport:

1. Implement the functions from `transport.h` in, for example,
   `src/transport/zigbee/`.
2. Add a `config APP_TRANSPORT_ZIGBEE` entry to the `APP_TRANSPORT` choice in
   `Kconfig`, and add its sources in `CMakeLists.txt`.
3. Turn off `SB_CONFIG_MATTER` in `sysbuild.conf` for that build, for example
   with a separate `sysbuild_zigbee.conf` selected through `-DSB_CONF_FILE=`.

The switch events map directly to the Zigbee *Multistate Input* cluster, or to
a BLE GATT notification.

### Bluetooth LE test build

Without a Thread network, the switches can be tested over Bluetooth LE. The
`ble` build variant replaces Matter with a small GATT server
([`src/transport/ble/ble_transport.c`](src/transport/ble/ble_transport.c)).
It sends the same switch events, using the same IDs as the Matter Switch
cluster events, and battery level through the Battery Service. The switch type
can be changed over BLE too:
`python3 tools/ble_test_client.py --set-type 2 latching`.

```sh
west build -b xiao_ble/nrf52840 --sysbuild -d build-ble -- -DFILE_SUFFIX=ble \
  -DEXTRA_CONF_FILE=usb-logging.conf
pip install bleak
python3 tools/ble_test_client.py
```

`usb-logging.conf` adds a USB serial log. It also
reboot the board into the bootloader when the serial port is opened at
1200 baud and closed with DTR low, so you can reflash without pressing reset.
By default the bootloader then starts in serial-DFU mode
(`CONFIG_APP_USB_BOOTLOADER_MODE_SERIAL`). That mode is more reliable than the
USB drive on macOS, which can hang when copying large UF2 files:

```sh
python3 -c "import serial,time; s=serial.Serial('/dev/cu.usbmodemXXXX',1200); time.sleep(.3); s.dtr=False; time.sleep(.3); s.close()"
adafruit-nrfutil dfu genpkg --dev-type 0x0052 --application build-ble/nrf52840-battery-button/zephyr/zephyr.hex pkg.zip
adafruit-nrfutil dfu serial -pkg pkg.zip -p /dev/cu.usbmodemXXXX -b 115200 --singlebank
```

`adafruit-nrfutil --touch 1200` doesn't work here, because it leaves DTR
asserted. Select `CONFIG_APP_USB_BOOTLOADER_MODE_UF2` to get the USB drive
after the touch instead. The same one-liner also works from Arduino firmware,
which enters serial-only mode as well.

Serial DFU writes one contiguous image, from the application start to the
end of the factory data. The XIAO's bootloader rejected an 804 KB image but
accepted 654 KB. So the flash layout keeps the factory data directly behind a
608 KB application partition: the whole Matter image with factory data is
about 620 KB and flashes over serial DFU:

```sh
adafruit-nrfutil dfu genpkg --dev-type 0x0052 --application build/devices/<serial>/<serial>.hex pkg.zip
```

Serial DFU needs the bootloader's serial mode, which only the 1200-baud touch
of a `usb-logging.conf` build enters. A release build has no USB, so use the
UF2 drive for it (double-press reset, then `cp -X`, see
[Testing on a Mac](#testing-on-a-mac)).

### Changing the data model

Edit `src/default_zap/battery_switch.zap` with `west zap-gui`, then regenerate
with:

```sh
west zap-generate -z src/default_zap/battery_switch.zap
```

This rewrites `battery_switch.matter` and `zap-generated/`.

## Testing on a Mac

There are two ways to test on a Mac. The simplest is the
[Bluetooth LE test build](#bluetooth-le-test-build). It needs nothing but the
board and tests the switch inputs and press detection.

Testing the real Matter-over-Thread firmware also needs a Thread radio. The
steps below use a **Nordic nRF52840 Dongle** as the radio, OpenThread's
`ot-daemon` as the Thread network, and `chip-tool` as the Matter controller.
This was tested with nRF Connect SDK v3.4.1 on an Apple silicon Mac in
September 2026.

macOS gets in the way in three places:

* **No BLE commissioning.** macOS won't let third-party apps use the Matter
  BLE service (`0xFFF6`). Service discovery fails with `CBErrorDomain Code=8`
  ("The specified UUID is not allowed for this operation"), and `chip-tool`
  reports `GATT write characteristic operation failed`. The device therefore
  gets its Thread dataset through a development shell, and is then
  commissioned over the network. Phone-based controllers are not affected.
* **No mDNS on the Thread interface.** `ot-daemon` uses a point-to-point
  `utun` interface, which mDNSResponder ignores.
  [`tools/srp_mdns_bridge.py`](tools/srp_mdns_bridge.py) republishes the
  device's SRP registrations with `dns-sd -P`.
* **`ot-daemon` dies from SIGPIPE** when an `ot-ctl` client disconnects early
  (the log ends with `Failed to write CLI output: Socket is not connected`).
  Start it with SIGPIPE ignored, as shown below.

> **Be a good neighbour.** Other Thread networks are probably nearby. Always
> scan first, then create your own network on a channel no other network
> uses, with freshly generated credentials. Never run `thread start` on a
> device until you've confirmed its dataset. With no dataset, OpenThread
> starts its built-in default network (`OpenThread`, PAN 0x2929, channel 11).
> `tools/provision_thread_dataset.py` makes that check for you.

Below, `$NCS` is the SDK (for example `/opt/nordic/ncs/v3.4.1`), `$WORK` is a
scratch directory outside this repository, and `$REPO` is this repository.
Run `west` through the SDK toolchain, e.g.
`nrfutil sdk-manager toolchain launch --ncs-version v3.4.1 -- west ...`.

### 1. Dongle: OpenThread RCP firmware

Press the dongle's reset button (the sideways one) to enter its bootloader.
Then:

```sh
cd $NCS
west build -b nrf52840dongle/nrf52840 --sysbuild -d $WORK/build-rcp nrf/samples/openthread/coprocessor
nrfutil install nrf5sdk-tools
nrfutil nrf5sdk-tools pkg generate --hw-version 52 --sd-req=0x00 \
  --application $WORK/build-rcp/coprocessor/zephyr/zephyr.hex --application-version 1 $WORK/rcp.zip
nrfutil nrf5sdk-tools dfu usb-serial -pkg $WORK/rcp.zip -p /dev/cu.usbmodemXXXX
```

Afterwards the dongle shows up as "Thread Co-Processor" (VID 0x1915, PID
0x0000). Its serial port name changes, so check both boards' USB serial
numbers so you don't mix up the dongle and the switch.

### 2. `ot-daemon` and `ot-ctl`

Build them from the SDK's OpenThread sources, so they match the RCP firmware:

```sh
cmake -GNinja -S $NCS/modules/lib/openthread -B $WORK/ot-build \
  -DOT_PLATFORM=posix -DOT_DAEMON=ON -DOT_FTD=ON -DOT_MTD=OFF -DOT_RCP=OFF \
  -DOT_SRP_SERVER=ON -DOT_ECDSA=ON -DOT_SERVICE=ON -DOT_NETDATA_PUBLISHER=ON \
  -DOT_LOG_OUTPUT=PLATFORM_DEFINED -DOT_BUILD_EXECUTABLES=ON
ninja -C $WORK/ot-build ot-daemon ot-ctl
```

Start `ot-daemon` as root. Run this in a terminal of its own, because
`sudo` needs to ask for a password. The daemon keeps its settings in the
current directory.

```sh
mkdir -p $WORK/ot-run
sudo -b nohup sh -c "trap '' PIPE; cd $WORK/ot-run && exec $WORK/ot-build/src/posix/ot-daemon -v -d 4 'spinel+hdlc+uart:///dev/cu.usbmodem<dongle>' > ot-daemon.log 2>&1"
sleep 3; sudo chmod 666 /tmp/openthread-utun*.sock   # lets ot-ctl run without sudo
```

It creates an interface `utunN`; use that name as `ot-ctl -I utunN` from
here on. Stop it with `sudo pkill -f ot-build/src/posix/ot-daemon`.

### 3. Your own Thread network

```sh
OTCTL="$WORK/ot-build/src/posix/ot-ctl -I utunN"
$OTCTL ifconfig up
$OTCTL scan energy 200      # passive: busy channels
$OTCTL scan                 # beacon request: channel and PAN of nearby networks
$OTCTL dataset init new     # random key, PAN ID, extended PAN ID, mesh-local prefix
$OTCTL dataset channel 19   # a channel no nearby network uses
$OTCTL dataset channelmask 0x00080000   # 1 << 19: stay on that channel
$OTCTL dataset networkname BattSwitchTest
$OTCTL dataset              # check that the PAN ID differs from the scanned ones
$OTCTL dataset commit active
$OTCTL thread start
$OTCTL srp server enable
```

Limit the channel mask to your channel. Otherwise the leader regularly sends
MLE Announce messages on every channel from 11 to 26, including the ones
other networks use. Those messages are secured with your network key, so
other networks drop them, but there's no need to transmit there at all.

The Mac becomes the network's leader. Its `utun` interface gets the
mesh-local prefix, so it can reach the device directly. No border routing is
involved, and nothing is announced to your LAN except the mDNS records the
bridge publishes.

After a daemon restart, the dataset is still stored, but you need to repeat
`ifconfig up`, `thread start` and `srp server enable`.

### 4. `chip-tool` for macOS

The prebuilt `chip-tool` on the Nordic release page is for Linux. Build it
(about 6 GB and 20 minutes):

```sh
git clone --depth 1 --branch v3.4.1 https://github.com/nrfconnect/sdk-connectedhomeip.git $WORK/chip
cd $WORK/chip
python3 scripts/checkout_submodules.py --shallow --platform darwin
source scripts/bootstrap.sh -p all,darwin
gn gen out/chip-tool --root=examples/chip-tool && ninja -C out/chip-tool chip-tool
```

### 5. Development firmware for the switch

This build has no factory data: it uses the Matter SDK test credentials,
discriminator 3840 and passcode 20202021. It has the OpenThread shell and
the USB log. Don't use it outside a test setup.

```sh
west build -b xiao_ble/nrf52840 --sysbuild -d $WORK/build-dev $REPO -- \
  -DSB_CONFIG_MATTER_FACTORY_DATA_GENERATE=n \
  "-DEXTRA_CONF_FILE=usb-logging.conf;dev/no-factory-data.conf;dev/ot-shell.conf" \
  -DEXTRA_DTC_OVERLAY_FILE=dev/large-app.overlay
```

To flash it, double-press reset, then copy it to the drive **with `cp -X`**:

```sh
cp -X $WORK/build-dev/battery_switch_app.uf2 /Volumes/XIAO-BOOT/
```

A plain `cp` (or Finder) first writes a `._` metadata file, which can hang
the drive. The `Input/output error` at the end of a successful copy is normal:
the board reboots once it has received the file. Serial DFU also works for
this build (see the [Bluetooth LE test build](#bluetooth-le-test-build)).

### 6. Put the switch on the network

```sh
pip install pyserial
python3 $REPO/tools/provision_thread_dataset.py --ot-ctl "$OTCTL" \
  --serial-number <XIAO USB serial> --expect-channel 19 --avoid-pan 0x<neighbour PAN>
```

The switch attaches as a sleepy child of the Mac and registers its
commissionable service (`_matterc._udp`, subtype `_L3840`) with the SRP
server. Keep the bridge running for the rest of the test:

```sh
python3 $REPO/tools/srp_mdns_bridge.py --ot-ctl "$OTCTL"
```

### 7. Commission and watch the events

```sh
CT=$WORK/chip/out/chip-tool/chip-tool
S="--storage-directory $WORK/chip-storage"
$CT pairing onnetwork-long 1 20202021 3840 $S \
  --paa-trust-store-path $WORK/chip/credentials/development/paa-root-certs \
  --bypass-attestation-verifier true        # development credentials
$CT descriptor read parts-list 1 0 $S       # endpoints 1..6
$CT powersource read bat-voltage 1 0 $S
echo "switch subscribe-event-by-id 0xFFFFFFFF 1 60 1 0xFFFF" | $CT interactive start $S
```

Press the switches. Each press shows up as a Switch cluster event (0x01
InitialPress, 0x02 LongPress, 0x03 ShortRelease, 0x04 LongRelease, 0x05
MultiPressOngoing, 0x06 MultiPressComplete) on
endpoints 1–6.

### Cleaning up

* `$CT pairing unpair 1 $S` removes the switch from the test fabric.
* Stop `ot-daemon`. The test network disappears with it.
* Flash a normal firmware build to the switch.

## Known limitations

* The device attestation certificate (DAC) is still the Matter SDK's shared
  development certificate for VID 0xFFF1 / PID 0x8000. It proves nothing about
  the individual device. Pairing security doesn't depend on it; that comes
  from the per-device passcode. A product needs its own vendor ID, a PAI from
  a Matter-approved PAA, and a unique DAC per device. The SDK's factory data
  generator can create those (`--gen_certs`, with `chip-cert`).
* Devices that create their own pairing code store the passcode in plain text
  in the factory data page, so they can show it over USB. Anyone with
  physical access can read it (over USB, or with an SWD probe), just as they
  could read a label. To get a new code, copy `new-pairing-code.uf2` from the
  release (or `tools/provision_device.py --blank`) to the bootloader drive: it
  erases the page, and the device creates a new code when it restarts.
  Pairings with controllers stay.
* To save flash, endpoint 0 has none of the optional diagnostics clusters:
  Thread Network Diagnostics, Software Diagnostics and Diagnostic Logs. General
  Diagnostics, which is mandatory, is still there.
* No OTA/DFU and no MCUboot. The goal was a barebones build; enabling
  `SB_CONFIG_MATTER_OTA` also requires MCUboot and a slot in the external
  flash.

## License

[MIT](LICENSE), except for code from other projects, which keeps its own
license:

* [`src/third_party/qrcodegen/`](src/third_party/qrcodegen/): QR code
  encoder, MIT, © Project Nayuki.
* [`src/default_zap/zap-generated/`](src/default_zap/zap-generated/): code
  generated by the Matter SDK's ZAP templates, Apache-2.0, © Project CHIP
  Authors.

The firmware is built with the nRF Connect SDK (Zephyr, Matter, OpenThread),
whose parts come under their own licenses, including the Nordic 5-Clause
License for Nordic's parts.
