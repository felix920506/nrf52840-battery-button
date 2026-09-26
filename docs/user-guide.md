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
pin to GND. On the XIAO, the reset pad is D9 (P1.14); it is not a switch input.

## Commissioning

The device advertises over Bluetooth LE for 15 minutes after first boot. Open
your Matter controller and scan the QR code or enter the pairing code printed
on the USB serial port. For release firmware, connect USB and open the serial
port at 115200 baud; see the README Quick start for platform-specific commands.

The device is a Matter-over-Thread device, so your controller setup needs a
Thread border router. **Home Assistant is the only controller tested.** This is
an uncertified test device using Matter development credentials; some
ecosystems warn and allow setup, while others reject it or require extra
developer configuration.

The pairing code persists through firmware updates and factory resets. Keep it
private: anyone in Bluetooth range can commission an unpaired device if they
know the code, and anyone with physical USB access can read it on automatically
provisioned release firmware.

## Switch behavior

Each of up to six inputs appears as a separate Matter Generic Switch endpoint.
The type can be set to:

| Type | Suitable for | Reported behavior |
|---|---|---|
| Momentary (default) | Push buttons | Press, release, long press, and multi-press events |
| Latching | Rockers and toggles | A short press for every change of position |

A latching switch reports changes as presses, not its physical position. A
controller can use it as a toggle. Rapid flips can count as a double press;
long press is not available for latching switches.

Home Assistant exposes the type as a “Switch type” dropdown on each switch's
device page. Controllers without Matter Mode Select support may not show this
setting. The command-line Matter controller can change it with:

```sh
chip-tool modeselect change-to-mode <mode> <node-id> <endpoint>
```

Use mode `0` for Momentary and `2` for Latching. The selected type is stored in
flash and survives reboots, updates, and factory resets. Home Assistant may
need its Matter integration reloaded or Home Assistant restarted to refresh
event entities after a type change.

## Updating firmware

An application-only firmware update preserves the pairing code, controller
pairings, Thread network, and switch types:

1. Download the matching board's UF2 file from the [latest release](../releases/latest).
2. Disconnect the battery and connect USB.
3. Double-press reset to enter the UF2 bootloader. Boards without a reset
   button can enter it by shorting RST to GND twice quickly.
4. Copy the UF2 file to the bootloader drive. On macOS, use Terminal with
   `cp -X`, not Finder; Finder can enter a crash loop. A final `Input/output
   error` can be normal because the board restarts as soon as the copy completes.
5. Disconnect USB and reconnect the battery. Rejoining Thread may take a minute.

Use the same release filename as the original install. Do not use
`new-pairing-code.uf2` for an ordinary update; that image replaces the pairing
code.

## Factory reset and pairing code

Hold the factory-reset input to GND for five seconds. On the XIAO this is D9.
The reset removes Matter fabrics and the Thread network; switch types and the
pairing code remain. After reset, commission the device again using its existing
pairing code.

To replace the pairing code, download `new-pairing-code.uf2` from the
[latest release](../releases/latest), enter UF2 bootloader mode, and copy the
file to the drive. The device generates a new code after restarting; open the
USB serial port to read it. Existing controller pairings are not removed by
changing the code. To start over completely, remove the device from controllers
and factory-reset it as well.

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
