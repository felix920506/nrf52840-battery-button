#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Test client for the Bluetooth LE transport (build with -DFILE_SUFFIX=ble).

Finds the "BatterySwitch" peripheral, prints its switch configuration and
battery state, then prints every switch event until interrupted.

    pip install bleak
    python3 tools/ble_test_client.py [--name BatterySwitch] [--timeout 20]
"""

import argparse
import asyncio
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

SWITCH_SERVICE = "5a1d0001-6b8f-4c1e-9a52-0c4f0b8e0a11"
SWITCH_EVENT = "5a1d0002-6b8f-4c1e-9a52-0c4f0b8e0a11"
SWITCH_INFO = "5a1d0003-6b8f-4c1e-9a52-0c4f0b8e0a11"
BATTERY_VOLTAGE = "5a1d0004-6b8f-4c1e-9a52-0c4f0b8e0a11"
SWITCH_TYPE = "5a1d0005-6b8f-4c1e-9a52-0c4f0b8e0a11"
BATTERY_LEVEL = "00002a19-0000-1000-8000-00805f9b34fb"

# Same IDs as the Matter Switch cluster events.
EVENTS = {
    0: "SwitchLatched",
    1: "InitialPress",
    2: "LongPress",
    3: "ShortRelease",
    4: "LongRelease",
    5: "MultiPressOngoing",
    6: "MultiPressComplete",
}
TYPES = {0: "momentary", 1: "latching", 2: "latching-as-press"}
TYPE_IDS = {name: value for value, name in TYPES.items()}


def format_event(data: bytes) -> str:
    number, event, position, count = struct.unpack("<BBBB", data[:4])
    text = f"switch {number}: {EVENTS.get(event, f'event {event}')}"
    if event == 0:
        text += f" position={position}"
    if event in (5, 6):
        text += f" count={count}"
    return text


async def run(name: str, timeout: float, set_type=None) -> int:
    print(f"Scanning for '{name}' ({timeout:.0f} s)...")
    device = await BleakScanner.find_device_by_filter(
        lambda d, adv: d.name == name or SWITCH_SERVICE in adv.service_uuids,
        timeout=timeout,
    )
    if device is None:
        print("Not found. Is the board powered and advertising (blue LED blinking)?")
        return 1

    print(f"Connecting to {device.name} ({device.address})...")
    async with BleakClient(device) as client:
        if set_type:
            number, type_name = set_type
            await client.write_gatt_char(SWITCH_TYPE, bytes([number, TYPE_IDS[type_name]]), response=True)
            print(f"Requested switch {number} -> {type_name}")
            await asyncio.sleep(0.5)

        info = await client.read_gatt_char(SWITCH_INFO)
        count = info[0]
        types = ", ".join(f"{i + 1}={TYPES.get(t, t)}" for i, t in enumerate(info[1 : 1 + count]))
        print(f"{count} switches: {types}")

        level = (await client.read_gatt_char(BATTERY_LEVEL))[0]
        (mv,) = struct.unpack("<H", await client.read_gatt_char(BATTERY_VOLTAGE))
        print(f"Battery: {mv} mV, {level} %")

        start = time.monotonic()

        def on_event(_, data: bytearray) -> None:
            print(f"[{time.monotonic() - start:8.3f}] {format_event(bytes(data))}", flush=True)

        def on_voltage(_, data: bytearray) -> None:
            print(f"Battery: {struct.unpack('<H', data)[0]} mV", flush=True)

        await client.start_notify(SWITCH_EVENT, on_event)
        await client.start_notify(BATTERY_VOLTAGE, on_voltage)
        print("Listening for switch events, Ctrl+C to stop.", flush=True)

        while client.is_connected:
            await asyncio.sleep(0.5)

    print("Disconnected.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--name", default="BatterySwitch")
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--set-type", nargs=2, metavar=("SWITCH", "TYPE"),
                        help="change a switch's type first, e.g. --set-type 2 latching "
                             "(momentary, latching, latching-as-press)")
    args = parser.parse_args()
    set_type = None
    if args.set_type:
        if args.set_type[1] not in TYPE_IDS:
            parser.error(f"TYPE must be one of: {', '.join(TYPE_IDS)}")
        set_type = (int(args.set_type[0]), args.set_type[1])
    try:
        return asyncio.run(run(args.name, args.timeout, set_type))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
