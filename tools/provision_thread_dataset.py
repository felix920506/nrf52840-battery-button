#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Give a device running the dev/ot-shell.conf build the Thread dataset of a
local ot-daemon network, then start Thread on it.

For computers that cannot commission over BLE. The dataset is entered in
short pieces (the shell's line buffer is too small for the hex form), read
back and compared with the host's dataset field by field. Thread is only
started if everything matches, so the device never falls back to
OpenThread's built-in default network. Secrets are never printed.

    python3 tools/provision_thread_dataset.py --ot-ctl "ot-ctl -I utun11" \\
        --serial-number 1145D7F7118F8DDB --expect-channel 19
"""

import argparse
import re
import shlex
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
FIELDS = ("Channel", "PAN ID", "Ext PAN ID", "Network Name", "Mesh Local Prefix", "Network Key", "PSKc")
PUBLIC = ("Channel", "PAN ID", "Ext PAN ID", "Network Name")


def parse(text):
    fields = {}
    for line in text.splitlines():
        key, sep, value = line.partition(":")
        if sep:
            fields[key.strip()] = value.strip()
    return fields


class Shell:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.3)
        self.s.dtr = True
        time.sleep(0.5)
        self.s.read(65536)

    def run(self, command, secret=False, wait=1.0):
        self.s.write((command + "\r\n").encode())
        time.sleep(wait)
        out = ANSI.sub("", self.s.read(65536).decode(errors="replace"))
        ok = "Done" in out
        shown = " ".join(command.split()[:3]) + (" <hidden>" if secret else "")
        print(f"  {shown if secret else command} -> {'Done' if ok else 'FAILED'}")
        if not ok:
            print("    (output hidden)" if secret else "    " + out.strip()[-300:])
            sys.exit(1)
        return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--ot-ctl", default="ot-ctl", help='ot-ctl command, e.g. "ot-ctl -I utun11"')
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--port", help="device serial port")
    group.add_argument("--serial-number", help="USB serial number of the device")
    parser.add_argument("--expect-channel", help="refuse unless the host network uses this channel")
    parser.add_argument("--avoid-pan", action="append", default=[],
                        help="refuse if the host network uses this PAN ID (e.g. 0xbac3), repeatable")
    args = parser.parse_args()

    host = parse(subprocess.run(shlex.split(args.ot_ctl) + ["dataset", "active"],
                                capture_output=True, text=True).stdout)
    if not all(host.get(f) for f in FIELDS):
        sys.exit("Could not read the host's active dataset. Is ot-daemon running with a network?")
    if args.expect_channel and host["Channel"] != args.expect_channel:
        sys.exit(f"Host network is on channel {host['Channel']}, expected {args.expect_channel}")
    if host["PAN ID"].lower() in (p.lower() for p in args.avoid_pan):
        sys.exit(f"Host network uses PAN ID {host['PAN ID']}, which is on the avoid list")
    print("Host network:", {f: host[f] for f in PUBLIC})

    port = args.port or next((p.device for p in list_ports.comports()
                              if p.serial_number == args.serial_number), None)
    if not port:
        sys.exit("Device not found")
    sh = Shell(port)

    print("Entering dataset on", port)
    sh.run("ot dataset clear")
    sh.run("ot dataset activetimestamp 1")
    sh.run("ot dataset channel " + host["Channel"])
    sh.run("ot dataset channelmask " + host["Channel Mask"])
    sh.run("ot dataset extpanid " + host["Ext PAN ID"])
    sh.run("ot dataset meshlocalprefix " + host["Mesh Local Prefix"].split("/")[0])
    sh.run("ot dataset networkkey " + host["Network Key"], secret=True)
    sh.run("ot dataset networkname " + host["Network Name"])
    sh.run("ot dataset panid " + host["PAN ID"])
    sh.run("ot dataset pskc " + host["PSKc"], secret=True)
    sh.run("ot dataset securitypolicy " + host["Security Policy"])
    sh.run("ot dataset commit active")

    device = parse(sh.run("ot dataset active", secret=True, wait=1.5))
    mismatch = [f for f in FIELDS if device.get(f) != host.get(f)]
    if mismatch:
        sys.exit(f"Device dataset does not match the host ({', '.join(mismatch)}); Thread NOT started")
    print("Device dataset matches the host:", {f: device[f] for f in PUBLIC})

    sh.run("ot ifconfig up")
    sh.run("ot thread start", wait=2)
    for _ in range(30):
        time.sleep(2)
        sh.s.write(b"ot state\r\n")
        time.sleep(0.5)
        state = re.search(r"\b(child|router|leader|detached|disabled)\b",
                          ANSI.sub("", sh.s.read(65536).decode(errors="replace")))
        print("  state:", state.group(1) if state else "?")
        if state and state.group(1) in ("child", "router"):
            return 0
    print("Device did not attach within 60 s")
    return 1


if __name__ == "__main__":
    sys.exit(main())
