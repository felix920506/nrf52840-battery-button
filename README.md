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
	latching;            /* rocker/toggle switch */
};
```

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
west flash
```

For logs over SEGGER RTT, add `-- -DEXTRA_CONF_FILE=debug.conf` to the build
command.

**Flashing needs an SWD probe** (J-Link, or a Raspberry Pi debug probe with
`--runner openocd`/`pyocd`), connected to the SWD pads on the bottom of the
XIAO. The firmware uses the whole internal flash, so it **erases the UF2
bootloader**. You can restore the bootloader later from Seeed's bootloader
`.hex`.

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
  restarts advertising while the device isn't commissioned. The QR code and
  manual pairing code come from the generated factory data: see
  `build/nrf52840-battery-button/zephyr/` or the RTT log. The build uses the
  Matter **test** vendor/product ID and test certificates, so controllers show
  it as an uncertified test device.
* **Identify:** the blue LED blinks.
* **Factory reset:**
  * momentary switch 1: hold it for 10 s
  * latching switch 1: toggle it 10 times within 5 s

## Power design

* Thread **Sleepy End Device** and Matter **ICD** (LIT capable; it runs as SIT
  with a 5 s slow poll until a controller registers for check-ins). TX power
  is 0 dBm.
* Switch pins wake the CPU through the GPIO SENSE mechanism (level
  interrupts), not GPIOTE IN channels. SENSE draws no current while idle.
* A **closed latching switch** would draw about 230 µA through the pull-up
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
  latching endpoint still lists `MultiPressMax` in its AttributeList, even
  though its feature map is `LS`. Controllers ignore this, but it would need
  separate endpoint types to pass certification.
* No OTA/DFU and no MCUboot. The goal was a barebones build; enabling
  `SB_CONFIG_MATTER_OTA` also requires MCUboot and a slot in the external
  flash.
