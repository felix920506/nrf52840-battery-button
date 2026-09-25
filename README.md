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

Other options, under *Battery switch application*: debounce time, long-press
time, multi-press window and max count, latching switch poll interval,
battery measurement interval, and warning/critical thresholds.

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

### Changing the data model

Edit `src/default_zap/battery_switch.zap` with `west zap-gui`, then regenerate
with:

```sh
west zap-generate -z src/default_zap/battery_switch.zap
```

This rewrites `battery_switch.matter` and `zap-generated/`.

## Known limitations

* All six switch endpoints share one endpoint type in the `.zap` file. So a
  `latching` endpoint still lists `MultiPressMax` in its AttributeList, even
  though its feature map is `LS`. Controllers ignore this, but it would need
  separate endpoint types to pass certification.
* No OTA/DFU and no MCUboot. The goal was a barebones build; enabling
  `SB_CONFIG_MATTER_OTA` also requires MCUboot and a slot in the external
  flash.
