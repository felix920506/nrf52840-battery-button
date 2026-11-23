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

### Long switch cables

An open switch holds its pin high only through the internal pull-up
(about 13 kΩ). A long cable laid next to mains wiring picks up fast spikes
when nearby loads switch (relays, motors, dimmers, light switches), and they
can pull the pin low for long enough to look like a press. Keep the cable
short, twist each switch wire with its GND wire, and route it away from mains
conductors.

If a switch still triggers on its own, add a 10 nF ceramic capacitor (any
generic MLCC) from each affected input pin to GND, soldered at the board, not
at the switch. The capacitor and the cable's small coupling capacitance (tens
of pF) form a divider, so the spikes reach the pin about a hundred times
weaker. It draws no current, and the release firmware works with it as is.

Don't use more than 10 nF on a switch that is, or may later be set to,
latching (generic MLCCs can be up to 80% above their marking, which the
firmware allows for). While a latching switch is closed, the firmware checks every
100 ms whether it has opened by turning the pull-up on for 400 µs; a larger
capacitor can't charge in that time, so **opening the switch would never be
detected**. Momentary switches work with up to 100 nF.

Where you can choose the switch, a [NO/NC switch](#nonc-switches) on three
wires is more robust still, with or without a capacitor.

### NO/NC switches

For the most reliable switch on a long cable, use a switch with a changeover
contact (NO/NC, SPDT, or a "3-way" wall switch) and three wires. Wire its
common terminal to GND, the contact that closes when pressed (or in the "on"
position) to the switch pin, and the other contact to the switch's NC pin:

| Board | NC pins for switches 1–6 |
|---|---|
| Seeed XIAO nRF52840 | D6, D7, D8, D10 (switches 1–4 only) |
| Adafruit Feather nRF52840 | D5, D6, D9, D10, D11, D12 |
| Pro Micro nRF52840 | P0.31, P0.29, P0.02, P1.15, P1.13, P1.11 |

Then set the switch's type to **Momentary NO/NC** or **Latching NO/NC** in
the smart home app (see [Switch behavior](#switch-behavior)). The release
firmware supports this as is. With the other types, the NC pin is not used.

In each position, one of the two pins is tied to GND by the switch, which
interference can hardly lift, so the device only accepts a change when both
pins change. Interference that pulls the open pin low is ignored. Neither
position draws current through the pull-ups. The switch must open one
contact before closing the other; most rockers, microswitches and 3-way
switches do. If both contacts read closed, or both open (for example a NO/NC
type on a switch with only two wires), the USB serial port says so, so you
can check the wiring. A NO/NC type on a two-wire switch doesn't work: it
would never see the switch being released or turned off.

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

**Home Assistant** refuses devices with test credentials unless its Matter
server allows them. Pairing then fails after the device certificate is read,
with "This device uses a test/development certificate". Before pairing, open
*Settings → Add-ons → Matter Server → Configuration*, turn on the test-net DCL
option, save, and restart the add-on. For a stand-alone python-matter-server,
start it with `--enable-test-net-dcl`. Only do this for devices you trust:
it lets Home Assistant add any device with Matter test credentials. You can
turn it off again once the device is paired: it only matters while a device
is added. [`tools/pair_home_assistant.py`](#pair-with-home-assistant-from-a-computer)
does both for you.

There are two ways to pair: with your controller's phone app over Bluetooth
([below](#pair-with-a-phone)), which works with any Matter controller, or,
for Home Assistant, from a computer with the board connected over USB
([Pair with Home Assistant from a computer](#pair-with-home-assistant-from-a-computer)).

### Pair with a phone

1. Connect the board over USB, without the battery, and open its serial port
   at 115200 baud (see the README Quick start for commands). Keep the port
   open: it shows the QR code and pairing code, and each pairing step as it
   happens.
2. Check that the device is in pairing mode: the blue LED blinks briefly
   every 2 s. If it doesn't, press any switch, or press `P` in the terminal.
   Pairing mode lasts 15 minutes.
3. On your phone, turn on Bluetooth and stay within a few metres of the
   device. The phone must be on the same network as your controller.
4. In your controller's app, add a Matter device and scan the QR code from
   the serial port, or choose to enter the code manually and type the
   11-digit pairing code. In the Home Assistant Companion app, go to
   *Settings → Devices & services → Devices*, select *Add device*, then
   *Add Matter device*. Make sure the test-net DCL option is on first (see
   above).
5. The app may warn that the device is uncertified; choose to add it anyway.
   It then gives the device your Thread network over Bluetooth, and the
   device joins it. The phone needs your Thread network's credentials for
   this: in the Home Assistant Companion app on Android, use *Sync Thread
   credentials* (*Settings → Companion app → Troubleshooting*) if pairing
   stops at the Thread step.
6. When the app shows the device, pairing is done and the LED stops
   blinking. You can now disconnect USB and connect the battery.

If pairing fails, the serial port shows the step where it stopped and what to
check (see [Pairing diagnostics](#pairing-diagnostics-on-the-serial-port)).

### Pair with Home Assistant from a computer

Instead of scanning the QR code with a phone, `tools/pair_home_assistant.py`
pairs a device connected to your computer over USB directly with Home
Assistant, without Bluetooth. It reads the pairing code from the serial port
and gives the device Home Assistant's preferred Thread network over USB (the
`T` command). Once the device has joined the network and registered with the
border router, Home Assistant's Matter server commissions it over the network.
The device's pairing events are shown as they happen, so if pairing fails you
see which step failed. The pairing code and the Thread network key go only to
Home Assistant and the device; they are never printed.

You need:

- The Matter integration in Home Assistant. The Matter server must accept
  test certificates (see [Commissioning](#commissioning)): with the Matter
  Server add-on, the script turns its test-net DCL option on for the pairing
  and off again afterwards, after asking you. Each change restarts the add-on,
  so your Matter devices are unavailable in Home Assistant for about a minute,
  twice. `--keep-matter-settings` leaves the option alone. Without the
  Supervisor (Home Assistant Container or Core), turn the option on yourself.
- A Thread border router, with its network set as Home Assistant's preferred
  network (*Settings → Devices & services → Thread*), within range of the
  device. Use `--thread-network <name>` for a different network.
- Home Assistant's Matter server must reach Thread devices through the border
  router, as for any Thread device: same LAN, IPv6 enabled, no VLAN or
  firewall between them.
- Firmware with the `T` command (press `H` on the serial port to check).

Create a long-lived access token for an administrator (your profile in Home
Assistant, *Security* tab). Connect the board over USB without the battery,
close any terminal that has its serial port open, then run:

```sh
HA_TOKEN=<token> python3 tools/pair_home_assistant.py --url http://homeassistant.local:8123
```

Without `HA_TOKEN`, the script asks for the token. It finds the device's port
by its USB ID; choose one with `--port` if several are connected. Use
`--insecure` for an HTTPS server with a self-signed certificate. When pairing
succeeds, it prints a link to the device's page in Home Assistant. Without a
clone of this repository, download `tools/pair_home_assistant.py` and
`tools/triage.py` into the same folder. Only Python 3 is needed (on Windows,
also `pip install pyserial`).

The device keeps the Thread network even if pairing fails, and tries the
same network on the next attempt. A factory reset removes it. While the
network is set, pairing over Bluetooth with a different Thread network fails;
factory reset the device first.

With `--bluetooth`, Home Assistant pairs over Bluetooth instead, as a phone
would. This also works with older firmware without the `T` command, but needs
Bluetooth commissioning enabled in the Matter server (the Bluetooth option of
the Matter Server add-on, or `--bluetooth-adapter` for a stand-alone
python-matter-server) and its host's Bluetooth adapter within range of the
device. It sets the Matter server's Thread network to the preferred one;
`--keep-thread-network` keeps the current one.

### Pairing diagnostics on the serial port

Smart home apps often report only "pairing failed". Keep the serial port open
while pairing: the device prints each step as the controller takes it, and if
pairing fails, the step where it stopped and what to check. For example:

```text
[  41.2 s] Bluetooth: controller connected
[  41.9 s] Controller is checking the pairing code (PASE)
[  43.4 s] Pairing code accepted, secure session established
[  43.6 s] Controller: arm fail-safe (start or extend, or cancel commissioning)
[  44.1 s] Controller: read device certificate
[  44.8 s] Controller: check device attestation
[  51.0 s] Fail-safe expired: the controller cancelled pairing or a step timed out
[  51.0 s] Pairing FAILED at step: controller checking the device certificate
[  51.0 s]   -> The controller stopped after reading the device certificate. This is an
...
```

The failure explanations cover a wrong pairing code, a rejected device
certificate, a controller without Thread credentials, a device that cannot
join the Thread network, and a controller that cannot reach the device over
Thread.

The device keeps the pairing events since it started (about 70 events; the
oldest are dropped first) and prints them all whenever a terminal opens the
port, after the pairing code. Closing and reopening the terminal, or opening
it only after a failed attempt, loses nothing. The events also survive a
crash and a factory reset: a `---- Restarted ----` line separates them from
the events after the restart, and a crash is reported with its cause. Only a
power cycle (unplugging USB, removing the battery) erases them, so after a
failed attempt, keep USB connected until you have looked at them.

Event times count seconds since the device started. `S` shows the current
uptime and why the device last restarted, so you can work out when an event
happened and find it in your controller's logs.

Press a key in the terminal to run a command:

| Key | Command |
|---|---|
| `S` | Status: uptime, last restart reason, paired controllers (fabrics), pairing mode, last pairing attempt, Thread network, role and service registration with the border router |
| `L` | Pairing events again |
| `D` | Diagnostics report for support: firmware, status and all pairing events, without the pairing code |
| `P` | Start pairing mode (Bluetooth advertising), like pressing a switch |
| `X` | Stop pairing mode |
| `T` | Join a Thread network pasted as a hex dataset, to pair over the network instead of Bluetooth (see [Pair with Home Assistant from a computer](#pair-with-home-assistant-from-a-computer)) |
| `I` | Show the pairing code and QR code again |
| `F` | Factory reset; press `Y` to confirm |
| `H` | Help |

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
The type can be set to (the NO/NC types only on switches with an NC pin, see
[NO/NC switches](#nonc-switches)):

| Type | Suitable for | Reported behavior |
|---|---|---|
| Momentary (default) | Push buttons | Press, release, long press, and multi-press events |
| Latching | Rockers and toggles | A short press for every change of position |
| Momentary NO/NC | Push buttons with a changeover contact, 3 wires | As Momentary |
| Latching NO/NC | Rockers and toggles with a changeover contact, 3 wires | As Latching |

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

Use mode `0` for Momentary, `2` for Latching, `3` for Momentary NO/NC and `4`
for Latching NO/NC. The device applies the new
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

Use the file for the same board as the original install. Do not use
`new-pairing-code-<version>.uf2` for an ordinary update; that image replaces the pairing
code.

Your controller shows the installed firmware version on the device page (Home
Assistant: *Firmware*): the release tag, or the commit hash for a build that
isn't a release. The hardware version shows the board the firmware was built
for. The USB serial port shows both at the top when a terminal opens it, so
you can check them before pairing, for example after step 4:

```text
Battery Switch
Firmware: v1.0.0
Hardware: Seeed XIAO nRF52840 (xiao_ble/nrf52840)
Serial number: ...
```

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

1. Download `new-pairing-code-<version>.uf2` from the
   [latest release](https://github.com/felix920506/nrf52840-battery-button/releases/latest).
   It works for every UF2 board: it only erases the page that holds the
   pairing code, not the firmware or the settings.
2. Disconnect the battery, connect the board over USB and double-press reset.
3. Copy `new-pairing-code-<version>.uf2` onto the drive (on macOS with `cp -X` in
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
- **Pairing fails:** keep the serial port open while pairing and read the
  [pairing diagnostics](#pairing-diagnostics-on-the-serial-port); press `S`
  for the current state.
- **Controller rejects the device:** only Home Assistant has been tested.
  Matter test credentials are not accepted by every ecosystem. Home Assistant
  needs the Matter server's test-net DCL option (see
  [Commissioning](#commissioning)).
- **Device does not join Thread:** confirm the controller has a working Thread
  border router and that the device is powered and in range. The `S` command
  shows the network the device was given and whether it is connected.
- **Device was removed from the app but won't pair again:** press `S`. If it
  still lists a controller, the device was not told it was removed; factory
  reset it (`F` on the serial port, or the reset pad).
- **A switch triggers on its own:** the switch cable is picking up
  interference, typically from mains wiring next to it. Keep the cable short,
  twist each switch wire with its GND wire, and route it away from mains
  conductors. If that is not enough, add a 10 nF capacitor from the input pin
  to GND (see [Long switch cables](#long-switch-cables)), or use a
  [NO/NC switch](#nonc-switches) on three wires. The firmware
  only accepts a change that several samples in a row agree on; you can also
  build with a larger `CONFIG_APP_DEBOUNCE_LATCHING_MS` and
  `CONFIG_APP_DEBOUNCE_SAMPLES`.

### Troubleshoot with a coding agent

If the list above doesn't help, `tools/triage.py` starts a coding agent
(Claude Code, Codex, or Antigravity, whichever is installed) that investigates
the problem with you. It looks at the connected board, the docs and the
firmware source for your version, searches existing issues, and can help you
file a good one. It asks before it runs anything that changes your device and
before it posts anything.

You need Python 3 and one of the agent CLIs (`claude`, `codex`, or `agy`).
Connect the board over USB if you can (without the battery), then run, from a
clone of this repository:

```sh
python3 tools/triage.py
```

Or, without a clone:

```sh
curl -fsSLO https://raw.githubusercontent.com/felix920506/nrf52840-battery-button/master/tools/triage.py
python3 triage.py
```

If more than one agent is installed, it asks which one to use; choose with
`--agent claude`, `--agent codex` or `--agent antigravity`, and a model with
`--model <model-id>`. `--no-serial` skips opening the board's serial port. On
Windows, reading the serial port needs `pip install pyserial`.

The script writes a triage folder with a `context.md` describing this computer
and the connected board, and the playbook the agent follows. With current
firmware, it also reads the device's diagnostics report (the `D` command), so
after a failed pairing attempt the agent sees every step the controller took
and where it stopped. Run it before unplugging the board: a power cycle
erases those events. The serial number is left out of `context.md`. If you
need the pairing code to pair and don't have a serial terminal open, the agent
can read it from the board and tell you the 11-digit code to enter manually in
your controller's app. With no agent
installed, the script prints the files' paths so you can paste the playbook
into any coding agent.

If your agent has a Home Assistant MCP server set up, the agent asks whether it
may connect to your Home Assistant server. With your permission, it reads the
device, its entities and related logs there instead of asking you to copy them.
It only reads unless you approve a specific change.
