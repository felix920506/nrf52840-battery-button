# Development and testing

[Back to the README](../README.md)

This guide collects development workflows and implementation details. Normal
users can install a prebuilt release using the [README Quick start](../README.md#quick-start).

## Code layout

```text
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
src/transport/ble/            Bluetooth LE test transport (GATT server)
src/third_party/qrcodegen/    QR code encoder (Project Nayuki, MIT)
src/default_zap/              Matter data model (.zap) and generated code
boards/                       per-board pins and flash layout
dev/                          development-only config overlays (see Testing on macOS)
tools/                        provisioning with labels, BLE test client, SRP-to-mDNS bridge,
                              Thread dataset provisioning
sysbuild.cmake                builds battery_switch_app.uf2 (application only), firmware version
.github/workflows/            builds the release UF2 files
```

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

- single press: `InitialPress`, `ShortRelease`, then `MultiPressComplete(1)`
  after the multi-press window
- double press: `InitialPress`, `ShortRelease`, `InitialPress`,
  `MultiPressOngoing(2)`, `ShortRelease`, `MultiPressComplete(2)`
- hold: `InitialPress`, `LongPress`, then `LongRelease` when released (no
  `MultiPressComplete`)
- more presses than `MultiPressMax`: `MultiPressComplete(0)`

`CurrentPosition` follows the switch. The Power Source cluster reports
`BatVoltage`, `BatPercentRemaining`, `BatChargeLevel` and
`BatReplacementNeeded`.

The Mode Select cluster on each switch endpoint has modes 0 (momentary) and 2
(latching). Mode 1 was an earlier type that reported the rocker's position
(Matter `SwitchLatched`); it no longer exists, and a switch stored with it
switches to mode 2 on the next boot. Controllers such as Home Assistant can't
follow a switch that changes to a position-reporting type, and a position is
rarely what automations need. Changing the type takes effect immediately: it
changes the electrical handling (the power-saving polling of closed latching
switches), the Switch cluster feature map and the events. Controllers may keep
using the old behaviour until they re-read the device; this Matter SDK can't
flag the change through `ConfigurationVersion` yet.

## Adding a transport

The core has no Matter dependencies. It emits `struct switch_event`s and
`struct battery_state`s through
[`src/transport/transport.h`](../src/transport/transport.h). To add a transport:

1. Implement the interface in a directory such as `src/transport/zigbee/`.
2. Add a `config APP_TRANSPORT_ZIGBEE` entry to the existing `APP_TRANSPORT`
   choice in `Kconfig`, and add its sources in `CMakeLists.txt`.
3. Turn off `SB_CONFIG_MATTER` in `sysbuild.conf` for that build, for example
   with a separate `sysbuild_zigbee.conf` selected through `-DSB_CONF_FILE=`.

The switch events map directly to the Zigbee *Multistate Input* cluster, or to
a BLE GATT notification.

## Changing the Matter data model

Edit `src/default_zap/battery_switch.zap` with `west zap-gui`, then regenerate:

```sh
west zap-generate -z src/default_zap/battery_switch.zap
```

This rewrites `battery_switch.matter` and the generated code under
`zap-generated/`.

## Bluetooth LE test build

Without a Thread network, the switches can be tested over Bluetooth LE. The
`ble` build variant replaces Matter with a small GATT server
([`src/transport/ble/ble_transport.c`](../src/transport/ble/ble_transport.c)).
It sends the same switch events, using the same IDs as the Matter Switch
cluster events, and battery level through the Battery Service.

```sh
west build -b xiao_ble/nrf52840 --sysbuild -d build-ble -- \
  -DFILE_SUFFIX=ble -DEXTRA_CONF_FILE=usb-logging.conf
pip install bleak
python3 tools/ble_test_client.py
```

The client can change the switch type too, for example:
`python3 tools/ble_test_client.py --set-type 2 latching`.

## Serial DFU and the 1200-baud touch

`usb-logging.conf` adds a USB serial log. It also enables
`CONFIG_APP_USB_BOOTLOADER_RESET`: opening the serial port at 1200 baud and
closing it with DTR low reboots the board into the bootloader, so you can
reflash without pressing reset. Which bootloader mode it enters is chosen by
the `APP_USB_BOOTLOADER_MODE` Kconfig choice:

- `CONFIG_APP_USB_BOOTLOADER_MODE_SERIAL` (default): serial DFU only. More
  reliable than the USB drive on macOS, which can hang when copying large UF2
  files.
- `CONFIG_APP_USB_BOOTLOADER_MODE_UF2`: the UF2 USB drive appears instead.

Reflash a `usb-logging.conf` build over serial DFU like this:

```sh
python3 -c "import serial,time; s=serial.Serial('/dev/cu.usbmodemXXXX',1200); time.sleep(.3); s.dtr=False; time.sleep(.3); s.close()"
adafruit-nrfutil dfu genpkg --dev-type 0x0052 --application build-ble/nrf52840-battery-button/zephyr/zephyr.hex pkg.zip
adafruit-nrfutil dfu serial -pkg pkg.zip -p /dev/cu.usbmodemXXXX -b 115200 --singlebank
```

`adafruit-nrfutil --touch 1200` doesn't work here, because it leaves DTR
asserted. The same one-liner also works from Arduino firmware, which enters
serial-only mode as well.

Serial DFU writes one contiguous image, from the application start to the end
of the factory data. The XIAO's bootloader rejected an 804 KB image but
accepted 654 KB. That is why the flash layout keeps the factory data directly
behind a 608 KB application partition (see the
[flash layout](building.md#flash-layout)): the whole Matter image with factory
data is about 620 KB and flashes over serial DFU:

```sh
adafruit-nrfutil dfu genpkg --dev-type 0x0052 --application build/devices/<serial>/<serial>.hex pkg.zip
```

Serial DFU needs the bootloader's serial mode, which only the 1200-baud touch
of a `usb-logging.conf` build enters. A release build has no USB console, so
use the UF2 drive for it (double-press reset, then `cp -X`).

## Testing Matter over Thread on macOS

The macOS test setup uses an nRF52840 Dongle running OpenThread RCP firmware,
`ot-daemon` as the Thread network, and `chip-tool` as the Matter controller.
It requires the nRF Connect SDK v3.4.1, an Apple silicon Mac, and substantial
build tools and disk space (the `chip-tool` build is about 6 GB and 20 minutes).

macOS cannot commission third-party Matter BLE services, and its mDNS responder
ignores the point-to-point `utun` interface used by `ot-daemon`. The commands
below use `tools/srp_mdns_bridge.py` to publish SRP registrations. Start
`ot-daemon` with SIGPIPE ignored to avoid it exiting when an `ot-ctl` client
disconnects early.

> **Be a good neighbour:** scan nearby Thread networks and choose a channel
> that is not in use. Never start Thread before confirming the dataset;
> OpenThread otherwise starts its built-in default network. The dataset
> provisioning utility checks the selected dataset.

Set `$NCS` to the SDK directory, `$WORK` to a scratch directory outside this
repository, and `$REPO` to this repository. Run west through the SDK toolchain.

### 1. Dongle RCP firmware

Put the Dongle into its bootloader with its sideways reset button, then build
and flash the RCP:

```sh
cd $NCS
west build -b nrf52840dongle/nrf52840 --sysbuild -d $WORK/build-rcp nrf/samples/openthread/coprocessor
nrfutil install nrf5sdk-tools
nrfutil nrf5sdk-tools pkg generate --hw-version 52 --sd-req=0x00 \
  --application $WORK/build-rcp/coprocessor/zephyr/zephyr.hex --application-version 1 $WORK/rcp.zip
nrfutil nrf5sdk-tools dfu usb-serial -pkg $WORK/rcp.zip -p /dev/cu.usbmodemXXXX
```

The Dongle reappears as “Thread Co-Processor” and its serial port name changes.
Identify both boards by USB serial number to avoid mixing them up.

### 2. Build and start OpenThread tools

Build tools from the SDK sources so they match the RCP firmware:

```sh
cmake -GNinja -S $NCS/modules/lib/openthread -B $WORK/ot-build \
  -DOT_PLATFORM=posix -DOT_DAEMON=ON -DOT_FTD=ON -DOT_MTD=OFF -DOT_RCP=OFF \
  -DOT_SRP_SERVER=ON -DOT_ECDSA=ON -DOT_SERVICE=ON -DOT_NETDATA_PUBLISHER=ON \
  -DOT_LOG_OUTPUT=PLATFORM_DEFINED -DOT_BUILD_EXECUTABLES=ON
ninja -C $WORK/ot-build ot-daemon ot-ctl
```

Run the daemon in its own terminal session:

```sh
mkdir -p $WORK/ot-run
sudo -b nohup sh -c "trap '' PIPE; cd $WORK/ot-run && exec $WORK/ot-build/src/posix/ot-daemon -v -d 4 'spinel+hdlc+uart:///dev/cu.usbmodem<Dongle>' > ot-daemon.log 2>&1"
sleep 3; sudo chmod 666 /tmp/openthread-utun*.sock
```

It creates a `utunN` interface. Set `OTCTL="$WORK/ot-build/src/posix/ot-ctl -I utunN"` for the commands below. Stop the daemon with `sudo pkill -f ot-build/src/posix/ot-daemon`.

### 3. Create a Thread network

```sh
$OTCTL ifconfig up
$OTCTL scan energy 200
$OTCTL scan
$OTCTL dataset init new
$OTCTL dataset channel 19
$OTCTL dataset channelmask 0x00080000
$OTCTL dataset networkname BattSwitchTest
$OTCTL dataset
$OTCTL dataset commit active
$OTCTL thread start
$OTCTL srp server enable
```

Choose an unused channel based on the scans and set the corresponding channel
mask (`1 << channel`). Restricting the mask prevents leader announcements on
other channels. The Mac becomes the leader; after daemon restart repeat
`ifconfig up`, `thread start`, and `srp server enable`.

### 4. Build `chip-tool`

The Nordic prebuilt tool is for Linux. Build the macOS version (about 6 GB and
20 minutes):

```sh
git clone --depth 1 --branch v3.4.1 https://github.com/nrfconnect/sdk-connectedhomeip.git $WORK/chip
cd $WORK/chip
python3 scripts/checkout_submodules.py --shallow --platform darwin
source scripts/bootstrap.sh -p all,darwin
gn gen out/chip-tool --root=examples/chip-tool && ninja -C out/chip-tool chip-tool
```

### 5. Build development firmware

This test-only image has no factory data: it uses the Matter SDK test
credentials (discriminator `3840`, passcode `20202021`), and adds the
OpenThread shell and the USB log. Do not use it outside an isolated test
setup.

```sh
west build -b xiao_ble/nrf52840 --sysbuild -d $WORK/build-dev $REPO -- \
  -DSB_CONFIG_MATTER_FACTORY_DATA_GENERATE=n \
  "-DEXTRA_CONF_FILE=usb-logging.conf;dev/no-factory-data.conf;dev/ot-shell.conf" \
  -DEXTRA_DTC_OVERLAY_FILE=dev/large-app.overlay
```

Double-press reset and flash with:

```sh
cp -X $WORK/build-dev/battery_switch_app.uf2 /Volumes/XIAO-BOOT/
```

### 6. Put the switch on the network

```sh
pip install pyserial
python3 $REPO/tools/provision_thread_dataset.py --ot-ctl "$OTCTL" \
  --serial-number <XIAO USB serial> --expect-channel 19 --avoid-pan 0x<neighbour PAN>
python3 $REPO/tools/srp_mdns_bridge.py --ot-ctl "$OTCTL"
```

Keep the bridge running. The switch attaches as a sleepy child and registers
its commissionable service with the SRP server.

### 7. Commission and inspect events

```sh
CT=$WORK/chip/out/chip-tool/chip-tool
S="--storage-directory $WORK/chip-storage"
$CT pairing onnetwork-long 1 20202021 3840 $S \
  --paa-trust-store-path $WORK/chip/credentials/development/paa-root-certs \
  --bypass-attestation-verifier true
$CT descriptor read parts-list 1 0 $S
$CT powersource read bat-voltage 1 0 $S
echo "switch subscribe-event-by-id 0xFFFFFFFF 1 60 1 0xFFFF" | $CT interactive start $S
```

Press the inputs to observe Switch cluster events (0x01 InitialPress, 0x02
LongPress, 0x03 ShortRelease, 0x04 LongRelease, 0x05 MultiPressOngoing, 0x06
MultiPressComplete) on endpoints 1–6.

Cleaning up:

- `$CT pairing unpair 1 $S` removes the switch from the test fabric.
- Stop `ot-daemon`. The test network disappears with it.
- Flash a normal firmware build to the switch.

## Power design

- Thread **Sleepy End Device** and Matter **ICD** (LIT capable; it runs as SIT
  with a 5 s slow poll until a controller registers for check-ins). TX power
  is 0 dBm.
- Switch pins wake the CPU through the GPIO SENSE mechanism (level
  interrupts), not GPIOTE IN channels. SENSE draws no current while idle.
- A **closed latching switch** would draw about 230 µA through the pull-up
  indefinitely. Instead, the pin is disconnected and sampled for about 10 µs
  every 100 ms (`CONFIG_APP_LATCH_POLL_INTERVAL_MS`), which costs roughly
  1 µA. The trade-off is up to 100 ms of latency when the switch is opened.
  Momentary buttons draw pull-up current only while held.
- The battery is measured once per hour on the internal VDD channel, with no
  divider.
- The on-board 2 MB QSPI flash is put into deep power-down. UART, I²C, SPI,
  PWM and USB are disabled, and so are logging and the console, unless you
  build with `debug.conf` or `usb-logging.conf`.
- The XIAO's own regulator and charger circuitry sit on the 3V3 rail. Measure
  the sleep current of your board with a power profiler; it's the real limit
  on battery life.

## Known limitations

- The device attestation certificate (DAC) is the Matter SDK's shared
  development certificate for VID 0xFFF1 / PID 0x8000. It proves nothing about
  the individual device. Pairing security doesn't depend on it; that comes
  from the per-device passcode. A product needs its own vendor ID, a PAI from
  a Matter-approved PAA, and a unique DAC per device. The SDK's factory data
  generator can create those (`--gen_certs`, with `chip-cert`).
- Devices that create their own pairing code store the passcode in plain text
  in the factory data page, so they can show it over USB. Anyone with
  physical access can read it (over USB, or with an SWD probe), just as they
  could read a label. Devices provisioned with `tools/provision_device.py`
  store only the SPAKE2+ verifier.
- To save flash, endpoint 0 has none of the optional diagnostics clusters:
  Thread Network Diagnostics, Software Diagnostics and Diagnostic Logs.
  General Diagnostics, which is mandatory, is still there.
- No OTA/DFU and no MCUboot. The goal was a barebones build; enabling
  `SB_CONFIG_MATTER_OTA` also requires MCUboot and a slot in the external
  flash.
