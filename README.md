# nRF52840 battery switch

Battery powered Matter-over-Thread switch for the **Seeed Studio XIAO nRF52840**.
It supports up to **6 external switches**, and each one can be a momentary push
button, a latching rocker/toggle switch, or a latching switch whose position
means nothing and is reported as presses. Every switch shows up in Matter
controllers as its own *Generic Switch* endpoint. The battery shows up as a
*Power Source*.

It is built on nRF Connect SDK v3.4.1. The Matter stack only runs on
Zephyr, so the RTOS can't be avoided. The application itself is kept small: one
event loop in the main thread, no extra threads, and interrupts that only
queue events.

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

Switches, pins and switch types are set in
[`boards/xiao_ble.overlay`](boards/xiao_ble.overlay):

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
| `"latching"` | rockers/toggles whose position means on/off | switch position |
| `"latching-as-press"` | latching switches whose position means nothing | a short press on every change |

Use `latching-as-press` for two kinds of switch:

* Rockers whose position you can't read from the switch, because they have a
  plain face. Examples: Panasonic Deco Lite, and older Panasonic/National/Jimbo
  Japanese-style module switches.
* Switches that look like push buttons but latch electrically. Examples:
  Schneider ZenCelo, Panasonic Cosmo Art, Rinsa, Glatima.

Every flip is sent as a short press, so a controller can use the switch as a
toggle. Flipping it twice quickly counts as a double press. Long press isn't
possible with these switches.

If you remove a switch node, its Matter endpoint is disabled. Endpoints are
numbered in node order: the first node is endpoint 1.

## Matter data model

| Endpoint | Device type | Clusters |
|---|---|---|
| 0 | Root Node, Power Source | …, Power Source (battery, replaceable) |
| 1–6 | Generic Switch | Identify, Descriptor (tag list "1"…"6"), Switch |

Controllers receive these **Switch cluster events**:

| Switch type | Feature map | Events |
|---|---|---|
| momentary | MS, MSR, MSL, MSM (`0x1E`) | `InitialPress`, `ShortRelease`, `LongPress`, `LongRelease`, `MultiPressOngoing`, `MultiPressComplete` |
| latching | LS (`0x01`) | `SwitchLatched` (position 1 = closed, 0 = open) |
| latching-as-press | MS, MSR, MSM (`0x16`) | per change: `InitialPress`, `ShortRelease`; then `MultiPressComplete(n)` |

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
west init -m <this repo url> --mr main battery-switch-ws
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

For logs over SEGGER RTT, add `-- -DEXTRA_CONF_FILE=debug.conf` to the build
command. RTT needs an SWD probe.

Resulting image sizes with nRF Connect SDK v3.4.1, out of 784 KB of flash
available to the application:

| Build | Flash | RAM |
|---|---|---|
| default (low power) | 569 KB | 159 KB |
| `debug.conf` | 635 KB | 161 KB |

### Flashing over USB (UF2 bootloader)

The build keeps the XIAO's stock Adafruit UF2 bootloader. You don't need a
debug probe:

1. Connect the XIAO over USB. Remove the battery first.
2. Double-press the reset button. A USB drive appears, named `XIAO-BOOT` or `XIAO-SENSE` depending on the bootloader version.
3. Copy `build/battery_switch.uf2` to the drive. The XIAO reboots into the
   firmware once the copy finishes.

`battery_switch.uf2` contains both the application and the Matter factory data
(pairing credentials). The `zephyr.uf2` that Zephyr normally builds isn't
produced here, because it would lack the factory data.

Flash layout (`boards/xiao_ble.overlay`):

| Address | Contents |
|---|---|
| `0x00000–0x26FFF` | MBR + SoftDevice area of the bootloader (not touched) |
| `0x27000–0xEAFFF` | application (784 KB) |
| `0xEB000–0xEBFFF` | Matter factory data |
| `0xEC000–0xF3FFF` | settings: Matter fabrics, Thread network, etc. |
| `0xF4000–0xFFFFF` | UF2 bootloader (not touched) |

Reflashing the UF2 keeps the settings, so the device stays commissioned. To
start over, factory reset it (see below).

### Flashing with an SWD probe

`west flash` also works, with a J-Link or another probe on the SWD pads under
the XIAO. It programs the application and the factory data and leaves the
bootloader alone.

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
  restarts advertising while the device isn't commissioned. The QR code is
  in `build/matter_factory_data/zephyr/factory_data.png`, and the setup
  details are in `factory_data.json` next to it. They are also printed to the
  RTT log in a `debug.conf` build. By default the passcode is `20202021` and
  the discriminator is `3840`, so the manual pairing code is `34970112332`.
  The build uses the Matter **test** vendor/product ID and test certificates,
  so controllers show it as an uncertified test device.
* **Identify:** the blue LED blinks.
* **Factory reset:**
  * momentary switch 1: hold it for 10 s
  * latching switch 1 (either latching type): flip it 10 times within 5 s

## Power design

* Thread **Sleepy End Device** and Matter **ICD** (LIT capable; it runs as SIT
  with a 5 s slow poll until a controller registers for check-ins). TX power
  is 0 dBm.
* Switch pins wake the CPU through the GPIO SENSE mechanism (level
  interrupts), not GPIOTE IN channels. SENSE draws no current while idle.
* A **closed latching switch** (either latching type) would draw about 230 µA through the pull-up
  indefinitely. Instead, the pin is disconnected and sampled for about 10 µs
  every 100 ms (`CONFIG_APP_LATCH_POLL_INTERVAL_MS`), which costs roughly 1 µA.
  The trade-off is up to 100 ms of latency when the rocker is switched off.
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
src/main.c                    event loop, factory reset, battery timer
src/core/app_loop.*           ISR-safe event queue
src/core/switch_input.*       GPIO, debounce, low-power polling of closed rockers
src/core/switch_gesture.*     press/long/multi-press state machine (transport independent)
src/core/battery.*            VDD measurement, per-chemistry discharge curves
src/core/status_led.*         LED patterns
src/transport/transport.h     interface to the radio protocol
src/transport/matter/         Matter over Thread implementation
src/default_zap/              Matter data model (.zap) and generated code
sysbuild.cmake                builds battery_switch.uf2 (app + factory data)
dev/                          development-only config overlays (see Testing on a Mac)
tools/                        BLE test client, SRP-to-mDNS bridge, Thread dataset provisioning
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
cluster events, and battery level through the Battery Service:

```sh
west build -b xiao_ble/nrf52840 --sysbuild -d build-ble -- -DFILE_SUFFIX=ble \
  -DEXTRA_CONF_FILE=usb-logging.conf -DEXTRA_DTC_OVERLAY_FILE=usb-logging.overlay
pip install bleak
python3 tools/ble_test_client.py
```

`usb-logging.conf` and `usb-logging.overlay` add a USB serial log. They also
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

Serial DFU writes one contiguous image, and on this bootloader that image must
stay well below the size of the application partition. A Matter build with
factory data (about 804 KB, from 0x27000 to the end of the factory data) is
rejected; the application alone (about 650 KB) goes through. So flash Matter
builds with factory data through the UF2 drive (use `cp -X`, see
[Testing on a Mac](#testing-on-a-mac)), or build without factory data for
development.

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
  -DEXTRA_DTC_OVERLAY_FILE=usb-logging.overlay
```

To flash it, double-press reset, then copy it to the drive **with `cp -X`**:

```sh
cp -X $WORK/build-dev/battery_switch.uf2 /Volumes/XIAO-BOOT/
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
MultiPressOngoing, 0x06 MultiPressComplete, 0x00 SwitchLatched) on
endpoints 1–6.

### Cleaning up

* `$CT pairing unpair 1 $S` removes the switch from the test fabric.
* Stop `ot-daemon`. The test network disappears with it.
* Flash a normal firmware build to the switch.

## Known limitations

* All six switch endpoints share one endpoint type in the `.zap` file. So a
  `latching` endpoint still lists `MultiPressMax` in its AttributeList, even
  though its feature map is `LS`. Controllers ignore this, but it would need
  separate endpoint types to pass certification.
* No OTA/DFU and no MCUboot. The goal was a barebones build; enabling
  `SB_CONFIG_MATTER_OTA` also requires MCUboot and a slot in the external
  flash.
