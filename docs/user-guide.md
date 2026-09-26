# User guide

[Back to the README](../README.md)

This guide covers wiring, commissioning, everyday configuration, updates, and
recovery for a built firmware image. The quickest first install is in the
[README Quick start](../README.md#quick-start).

## Hardware and wiring

The release firmware supports these boards:

| Board | Switch pins | Factory reset |
|---|---|---|
| Seeed XIAO nRF52840, Sense, Plus | D0–D5 | hold D9 to GND |
| Adafruit Feather nRF52840 Express, Sense | A0–A5 | hold USER button |
| Pro Micro nRF52840, nice!nano, SuperMini | P0.17, P0.20, P0.22, P0.24, P1.00, P0.11 | hold P1.06 to GND |

Only the XIAO release image has been tested on hardware; the others are
build-tested. The XIAO pin diagram:

```text
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

Wire each switch between its input pin and GND. The inputs use internal pull-ups,
so no external components are needed. Connect the battery positive to 3V3, not
BAT or 5V; connect negative to GND. Supported batteries are one CR2032 or two
AAA cells in series (alkaline, carbon-zinc, or NiMH). For a CR2032, add a
low-leakage capacitor of at least 100 µF across 3V3/GND to support radio current
peaks. **Never connect USB and a battery at the same time.**

Leave the factory-reset pin unconnected or connect a service button from that
pin to GND. On the XIAO, the reset pad is D9 (P1.14); it is not a switch input,
so no switch can trigger a reset by accident.

The XIAO nRF52840, Sense and Plus share the chip, flash and D0–D10 pinout, so
the same release file works on all three.

## Commissioning

The device advertises over Bluetooth LE for 15 minutes after first boot, and
the blue LED blinks briefly every 2 s while it does. Pressing any switch
restarts advertising while the device isn't commissioned. Open your Matter
controller and scan the QR code or enter the pairing code printed on the USB
serial port. For release firmware, connect USB and open the serial port at
115200 baud; see the README Quick start for platform-specific commands.

The device is a Matter-over-Thread device, so your controller setup needs a
Thread border router. **Home Assistant is the only controller tested.** This is
an uncertified test device using Matter development credentials; some
ecosystems warn and allow setup, while others reject it or require extra
developer configuration.

The pairing code persists through firmware updates and factory resets. Keep it
private: anyone in Bluetooth range can commission an unpaired device if they
know the code, and anyone with physical USB access can read it on automatically
provisioned release firmware.

## LED

The blue LED on the XIAO gives this feedback:

| Pattern | Meaning |
|---|---|
| 3 flashes at power-on | the firmware has started |
| brief blink every 2 s | not commissioned; advertising over Bluetooth LE |
| blinking on request | *Identify* from a controller |
| 1 flash, then 5 flashes | factory reset pad detected, then factory reset done |

Otherwise it stays off to save power.

## Switch behavior

Each of up to six inputs appears as a separate Matter Generic Switch endpoint.
The type can be set to:

| Type | Suitable for | Reported behavior |
|---|---|---|
| Momentary (default) | Push buttons | Press, release, long press, and multi-press events |
| Latching | Rockers and toggles | A short press for every change of position |

A latching switch reports changes as presses, not its physical position. A
controller can use it as a toggle. Rapid flips can count as a double press;
long press is not available for latching switches. This also suits switches
whose position you can't read from the switch itself:

- plain-faced rockers, e.g. Panasonic Deco Lite, and older
  Panasonic/National/Jimbo Japanese-style module switches;
- switches that look like push buttons but latch electrically, e.g. Schneider
  ZenCelo, Panasonic Cosmo Art, Rinsa, Glatima.

There is no type that reports a rocker's position: controllers such as Home
Assistant can't follow it, and a position is rarely what automations need.

Home Assistant exposes the type as a “Switch type” dropdown on each switch's
device page. Controllers without Matter Mode Select support (e.g. Apple Home)
don't show this setting; change the type from another controller on the same
device, or with the command-line Matter controller:

```sh
chip-tool modeselect change-to-mode <mode> <node-id> <endpoint>
```

Use mode `0` for Momentary and `2` for Latching. The device applies the new
type immediately. It is stored in flash and survives reboots, updates, and
factory resets: it describes the wiring, which doesn't change when you
re-pair.

Controllers may keep using the old switch behaviour until they re-read the
device. **Home Assistant** decides a switch's event types only when it creates
the entity, and never recreates it for a known device; re-interviewing and
power cycling don't help. Both types report `multi_press_1`, `multi_press_2`,
…, so presses keep working after a change. Only the long-press events of a
momentary switch appear or disappear. To update those, reload the Matter
integration (*Settings → Devices & services → Matter → ⋮ → Reload*) or
restart Home Assistant.

## Updating firmware

An application-only firmware update preserves the pairing code, controller
pairings, Thread network, and switch types:

1. Download the matching board's UF2 file from the [latest release](https://github.com/felix920506/nrf52840-battery-button/releases/latest).
2. Disconnect the battery and connect USB.
3. Double-press reset to enter the UF2 bootloader. Boards without a reset
   button can enter it by shorting RST to GND twice quickly.
4. Copy the UF2 file to the bootloader drive. On macOS, use Terminal with
   `cp -X`, not Finder; Finder can enter a crash loop. A final `Input/output
   error` can be normal because the board restarts as soon as the copy completes.
   The board restarts with the new firmware; the LED flashes 3 times.
5. Disconnect USB and reconnect the battery. Rejoining Thread may take a minute.

Use the same release filename as the original install. Do not use
`new-pairing-code.uf2` for an ordinary update; that image replaces the pairing
code.

Your controller shows the installed firmware version on the device page (Home
Assistant: *Firmware*): the release tag, or the commit hash for a build that
isn't a release. The hardware version shows the board the firmware was built
for.

## Factory reset and pairing code

Hold the factory-reset input to GND for five seconds. On the XIAO this is D9.
The LED flashes once when the pad is detected and five times at the reset. The
reset removes Matter fabrics and the Thread network; switch types and the
pairing code remain. The pad is also checked at boot, so it works even if the
device keeps restarting. After reset, commission the device again using its
existing pairing code.

The pairing code never changes on its own. Replace it if someone else may know
it, for example before you give the device away. A new code doesn't remove
existing pairings: controllers that already have the device keep it. To start
over completely, remove the device from your controllers and factory-reset it
as well.

1. Download `new-pairing-code.uf2` from the
   [latest release](https://github.com/felix920506/nrf52840-battery-button/releases/latest).
   It works for every UF2 board: it only erases the page that holds the
   pairing code, not the firmware or the settings.
2. Disconnect the battery, connect the board over USB and double-press reset.
3. Copy `new-pairing-code.uf2` onto the drive (on macOS with `cp -X` in
   Terminal, not with Finder).
4. The board restarts and creates a new pairing code; the LED flashes 3
   times. If the USB drive is still there instead, copy the firmware file for
   your board (as for an update) as well.
5. Open the USB serial port as in the README Quick start: it shows the new QR
   code and pairing code. The old code no longer works.

## Troubleshooting

- **Bootloader drive does not appear:** double-press reset; on boards without a
  reset button, short RST to GND twice quickly.
- **UF2 copy hangs on macOS:** use `cp -X` from Terminal, not Finder or plain
  `cp`. The XIAO may restart before the copy command exits; an I/O error at
  that point is expected.
- **No pairing code appears:** confirm the board is running the firmware, then
  close and reopen the correct USB serial port at 115200 baud.
- **Controller rejects the device:** only Home Assistant has been tested.
  Matter test credentials are not accepted by every ecosystem.
- **Device does not join Thread:** confirm the controller has a working Thread
  border router and that the device is powered and in range.
