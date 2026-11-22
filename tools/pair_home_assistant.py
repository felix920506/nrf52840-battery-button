#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Pair a battery switch connected over USB with Home Assistant, without a phone
and without Bluetooth.

The script reads the pairing code from the device's USB serial port and gives
the device Home Assistant's preferred Thread network over USB (the T command).
Once the device has joined the network and registered with the border router,
Home Assistant's Matter server commissions it over the network. Meanwhile the
script shows the device's own pairing diagnostics, so if pairing fails you see
which step failed.

    python3 tools/pair_home_assistant.py --url http://homeassistant.local:8123

The Home Assistant token is a long-lived access token of an administrator
(your profile page in Home Assistant, Security tab). The script reads it from
the HA_TOKEN environment variable, or asks for it.

Requirements on the Home Assistant side:
  - The Matter integration.
  - A Thread border router, with its network as Home Assistant's preferred
    Thread network (Settings > Devices & services > Thread), within range of
    the device, which is connected to this computer.
  - The Matter server can reach Thread devices through the border router
    (the same as for any Thread device: same LAN, IPv6 enabled).

With --bluetooth, Home Assistant pairs over Bluetooth instead, as a phone
would (for firmware without the T command). That needs Bluetooth
commissioning enabled in the Matter server and its Bluetooth adapter within
range of the device.

Only the Python standard library is needed (and tools/triage.py next to this
file). On Windows, reading the serial port also needs pyserial.

The pairing code and the Thread network key go only to Home Assistant and
to the device; they are never printed.
"""


import argparse
import base64
import getpass
import json
import os
import re
import socket
import ssl
import struct
import sys
import threading
import time
import urllib.parse

from triage import ANSI, Port, list_serial_ports

USB_ID = (0x2FE3, 0x0004)
QR_PAYLOAD = re.compile(r"QR code:\s*(MT:[0-9A-Z.\-]+)")
SERIAL_NUMBER = re.compile(r"Serial number:\s*(\S+)")
STATUS = re.compile(r"^Status:\s*(.+)$", re.M)
# A pairing event line: "[  41.2 s] ...".
EVENT = re.compile(r"^\[\s*\d+\.\d s\]")

# How long Home Assistant may take to commission (Bluetooth, Thread join, interview).
COMMISSION_TIMEOUT = 600


class Error(Exception):
    pass


# --- Home Assistant WebSocket API ------------------------------------------


class HomeAssistant:
    """A minimal client for Home Assistant's WebSocket API (RFC 6455 text frames)."""

    def __init__(self, url, token, insecure=False, timeout=15):
        parts = urllib.parse.urlsplit(url if "://" in url else "http://" + url)
        if parts.scheme not in ("http", "https", "ws", "wss"):
            raise Error(f"unsupported URL scheme: {parts.scheme}")
        tls = parts.scheme in ("https", "wss")
        host = parts.hostname
        port = parts.port or (443 if tls else 80)
        path = parts.path.rstrip("/")
        if not path.endswith("/api/websocket"):
            path += "/api/websocket"
        self.base_url = urllib.parse.urlunsplit(
            ("https" if tls else "http", parts.netloc, path[: -len("/api/websocket")], "", ""))

        try:
            sock = socket.create_connection((host, port), timeout=timeout)
            if tls:
                context = ssl.create_default_context()
                if insecure:
                    context.check_hostname = False
                    context.verify_mode = ssl.CERT_NONE
                sock = context.wrap_socket(sock, server_hostname=host)
        except (OSError, ssl.SSLError) as err:
            raise Error(f"could not connect to {host}:{port}: {err}") from err
        self.sock = sock
        self.buffer = b""
        self.next_id = 1

        key = base64.b64encode(os.urandom(16)).decode()
        host_header = host if parts.port is None else f"{host}:{parts.port}"
        self.sock.sendall((
            f"GET {path} HTTP/1.1\r\nHost: {host_header}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        head = self._read_until(b"\r\n\r\n").decode(errors="replace")
        if not head.startswith("HTTP/1.1 101"):
            raise Error(f"Home Assistant did not open a WebSocket: {head.splitlines()[0]}")

        hello = self.receive()
        if hello.get("type") != "auth_required":
            raise Error(f"unexpected first message: {hello.get('type')}")
        self.version = hello.get("ha_version", "unknown")
        self._send({"type": "auth", "access_token": token})
        reply = self.receive()
        if reply.get("type") != "auth_ok":
            raise Error("Home Assistant rejected the access token"
                        + (f": {reply['message']}" if reply.get("message") else ""))

    def _read_exact(self, n):
        while len(self.buffer) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise Error("Home Assistant closed the connection")
            self.buffer += chunk
        data, self.buffer = self.buffer[:n], self.buffer[n:]
        return data

    def _read_until(self, marker):
        while marker not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise Error("Home Assistant closed the connection")
            self.buffer += chunk
        data, _, self.buffer = self.buffer.partition(marker)
        return data

    def _send_frame(self, opcode, payload):
        mask = os.urandom(4)
        n = len(payload)
        if n < 126:
            header = struct.pack("!BB", 0x80 | opcode, 0x80 | n)
        elif n < 1 << 16:
            header = struct.pack("!BBH", 0x80 | opcode, 0x80 | 126, n)
        else:
            header = struct.pack("!BBQ", 0x80 | opcode, 0x80 | 127, n)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def _send(self, message):
        self._send_frame(0x1, json.dumps(message).encode())

    def receive(self):
        """The next JSON message. Answers pings; fragments are joined."""
        message = b""
        while True:
            first, second = self._read_exact(2)
            opcode, n = first & 0x0F, second & 0x7F
            if n == 126:
                n = struct.unpack("!H", self._read_exact(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self._read_exact(8))[0]
            mask = self._read_exact(4) if second & 0x80 else None
            payload = self._read_exact(n)
            if mask:
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            if opcode == 0x8:
                raise Error("Home Assistant closed the connection")
            if opcode == 0x9:
                self._send_frame(0xA, payload)
                continue
            if opcode in (0x0, 0x1, 0x2):
                message += payload
                if first & 0x80:
                    return json.loads(message)

    def call(self, command, timeout=30, **data):
        """Runs a command and returns its result. Raises Error with Home
        Assistant's error code and message if it fails."""
        msg_id = self.next_id
        self.next_id += 1
        self._send({"id": msg_id, "type": command, **data})
        self.sock.settimeout(timeout)
        try:
            while True:
                reply = self.receive()
                if reply.get("id") == msg_id and reply.get("type") == "result":
                    break
        except socket.timeout as err:
            raise Error(f"{command}: no answer from Home Assistant within {timeout} s") from err
        if not reply.get("success"):
            error = reply.get("error") or {}
            raise Error(f"{command} failed: {error.get('message') or 'no message'}"
                        f" (code {error.get('code', '?')})")
        return reply.get("result")

    def close(self):
        try:
            self._send_frame(0x8, struct.pack("!H", 1000))
        except OSError:
            pass
        self.sock.close()


def loaded_entry(ha, domain):
    entries = ha.call("config_entries/get", domain=domain) or []
    return next((e for e in entries if e.get("state") == "loaded"), None), entries


def matter_devices(ha):
    """{device id: device} for every device of the Matter integration."""
    return {d["id"]: d for d in ha.call("config/device_registry/list") or []
            if any(i[0] == "matter" for i in d.get("identifiers", []))}


def choose_dataset(ha, network_name):
    datasets = (ha.call("thread/list_datasets") or {}).get("datasets", [])
    if network_name:
        matches = [d for d in datasets if d.get("network_name") == network_name]
        if not matches:
            names = ", ".join(sorted({d.get("network_name", "?") for d in datasets})) or "none"
            raise Error(f"Home Assistant knows no Thread network named {network_name!r} "
                        f"(known: {names})")
        return matches[0]
    preferred = next((d for d in datasets if d.get("preferred")), None)
    if preferred:
        return preferred
    if not datasets:
        raise Error("Home Assistant knows no Thread network. Set up a Thread border router "
                    "first (Settings > Devices & services > Thread).")
    names = ", ".join(sorted({d.get("network_name", "?") for d in datasets}))
    raise Error(f"Home Assistant has no preferred Thread network. Choose one in "
                f"Settings > Devices & services > Thread, or pass --thread-network ({names}).")


# --- The device ------------------------------------------------------------


def find_port(requested):
    if requested:
        return requested
    ports, _ = list_serial_ports()
    ours = [p[0] for p in ports if (p[1], p[2]) == USB_ID]
    if not ours:
        candidates = [p[0] for p in ports if p[1] is None]
        if len(candidates) == 1:
            return candidates[0]
        raise Error("No battery switch found on USB. Connect it (without the battery) and "
                    "check that it runs the firmware, or name its port with --port.")
    if len(ours) > 1:
        raise Error("Several battery switches are connected: choose one with --port "
                    f"({', '.join(ours)}).")
    return ours[0]


def clean(text):
    return [ANSI.sub("", l).rstrip() for l in text.replace("\r", "").split("\n")
            if "\x1b[30;107m" not in l]


class Device:
    """The device's serial port. A thread reads everything it prints and
    shows its pairing events ("[  41.2 s] ...") as they arrive; the rest
    (command output) is kept for wait_for()."""

    def __init__(self, port):
        self.port = port
        self.text = ""
        self.events = []
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.thread = None

    def start(self):
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def close(self):
        self.stop.set()
        if self.thread:
            self.thread.join(2)

    def _run(self):
        pending = ""
        while not self.stop.is_set():
            try:
                chunk = self.port.read(0.3)
            except OSError:
                return
            if not chunk:
                continue
            pending += chunk.replace("\r", "")
            *complete, pending = pending.split("\n")
            with self.lock:
                self.text += chunk
            for line in complete:
                line = ANSI.sub("", line).rstrip()
                if EVENT.match(line):
                    self.events.append(line)
                    print(f"  device: {line}", flush=True)

    def mark(self):
        with self.lock:
            return len(self.text)

    def send(self, key):
        self.port.send(key)

    def wait_for(self, patterns, timeout, since):
        """The first of `patterns` (regexes) printed after position `since`,
        as (index, match), or (None, None) after `timeout` seconds."""
        end = time.monotonic() + timeout
        while True:
            with self.lock:
                text = ANSI.sub("", self.text[since:])
            for i, pattern in enumerate(patterns):
                if m := re.search(pattern, text):
                    return i, m
            if time.monotonic() >= end:
                return None, None
            time.sleep(0.2)

    def command(self, key, patterns, timeout=4):
        return self.wait_for(patterns, timeout, self._send_marked(key))

    def _send_marked(self, key):
        since = self.mark()
        self.send(key)
        return since


def read_banner(port, seconds):
    """Returns (QR payload, serial number, status, has commands) from the
    banner the firmware prints when the port opens."""
    banner = port.read(seconds, quiet=1.0)
    if not banner.strip():
        raise Error("The device printed nothing. Close any other program that has its serial "
                    "port open (screen, a serial monitor) and try again.")
    text = "\n".join(clean(banner))
    qr = QR_PAYLOAD.search(text)
    serial_number = SERIAL_NUMBER.search(text)
    status = STATUS.search(text)
    if not qr:
        raise Error("The device did not print a QR code. It may run firmware that doesn't show "
                    "its pairing code on USB, or other firmware altogether.")
    return (qr.group(1), serial_number.group(1) if serial_number else None,
            status.group(1).strip() if status else None, "Press H for commands" in text)


ALREADY_PAIRED = ("The device is already paired. Remove it from the controller it is paired "
                  "with, or factory reset it (F on the serial port), first.")


def start_pairing_mode(device):
    i, _ = device.command("P", [r"already paired", r"Starting pairing mode|already on"])
    if i == 0:
        raise Error(ALREADY_PAIRED)
    if i is None:
        print("  (The device did not confirm pairing mode; trying anyway.)")


def give_thread_network(device, tlv):
    """Enters the dataset with the T command. Raises Error if the device
    doesn't take it."""
    i, _ = device.command("T", [r"Paste the Thread network", r"Unknown command",
                                r"already paired"])
    if i == 1 or i is None:
        raise Error("This firmware can't take a Thread network over USB. Update the firmware "
                    "(see the user guide), or pair over Bluetooth with --bluetooth.")
    if i == 2:
        raise Error(ALREADY_PAIRED)
    since = device.mark()
    device.send(tlv + "\r")
    i, _ = device.wait_for([r"Thread network set", r"Nothing was changed|Could not set|"
                            r"Cancelled|no network to join"], 5, since)
    if i != 0:
        with device.lock:
            reply = [l for l in clean(device.text[since:]) if l.strip()]
        raise Error("The device did not take the Thread network: "
                    + (" ".join(reply) if reply else "no answer."))


def wait_until_on_network(device, timeout):
    """Polls the status (S) until the device has joined Thread and registered
    its commissionable service with the border router."""
    end = time.monotonic() + timeout
    shown = None
    while time.monotonic() < end:
        _, m = device.command("S", [r"Service registration with the border router \(SRP\): "
                                    r"(.+)\n"], timeout=4)
        with device.lock:
            text = ANSI.sub("", device.text)
        role = re.findall(r"^Thread: (.+?)(?:, network|$)", text, re.M)
        state = (role[-1] if role else "?", m.group(1).strip() if m else "?")
        if state != shown:
            print(f"  Thread: {state[0]}; registration with the border router: {state[1]}",
                  flush=True)
            shown = state
        if state[0].startswith("connected") and state[1] == "registered":
            return
        time.sleep(3)
    if not shown or not shown[0].startswith("connected"):
        raise Error("The device did not join the Thread network. Is the border router on and "
                    "within range of the device? The device keeps the network; a factory "
                    "reset (F) removes it.")
    raise Error("The device joined the Thread network but did not register with the border "
                "router (SRP), so Home Assistant can't find it. Check that the border router "
                "is working (its SRP server), then run this again.")


# --- Main ------------------------------------------------------------------


def confirm(question):
    if not sys.stdin.isatty():
        return True
    return input(f"{question} [Y/n] ").strip().lower() in ("", "y", "yes")


def pair(args):
    token = os.environ.get("HA_TOKEN") or getpass.getpass(
        "Home Assistant long-lived access token (input hidden): ").strip()
    if not token:
        raise Error("No access token given.")

    name = find_port(args.port)
    print(f"Reading the device on {name}...")
    try:
        port = Port(name)
    except OSError as err:
        raise Error(f"Could not open {name}: {err}") from err
    device = Device(port)
    try:
        return pair_device(args, token, port, device)
    finally:
        device.close()
        port.close()


def pair_device(args, token, port, device):
    qr, serial_number, status, has_commands = read_banner(port, args.serial_seconds)
    print(f"  Serial number: {serial_number or 'not printed'}")
    print(f"  Status: {status or 'not printed'}")
    if status == "paired":
        raise Error(ALREADY_PAIRED)
    if not has_commands and not args.bluetooth:
        raise Error("This firmware has no serial commands, so it can't take a Thread network "
                    "over USB. Update the firmware (see the user guide), or pair over "
                    "Bluetooth with --bluetooth.")

    print(f"Connecting to Home Assistant at {args.url}...")
    ha = HomeAssistant(args.url, token, insecure=args.insecure)
    try:
        print(f"  Home Assistant {ha.version}")
        matter, entries = loaded_entry(ha, "matter")
        if not matter:
            raise Error("The Matter integration is not set up in Home Assistant."
                        if not entries else
                        f"The Matter integration is not running ({entries[0].get('state')}). "
                        "Check that the Matter server is running.")

        before = matter_devices(ha)
        if serial_number:
            known = [d for d in before.values()
                     if ["matter", f"serial_{serial_number}"] in d.get("identifiers", [])]
            if known:
                raise Error(f"Home Assistant already has this device: "
                            f"{device_link(ha, known[0])}")

        dataset = None
        if not (args.bluetooth and args.keep_thread_network):
            thread, _ = loaded_entry(ha, "thread")
            if not thread:
                raise Error("The Thread integration is not set up in Home Assistant, so there "
                            "is no Thread network to give the device. Set up a Thread border "
                            "router first.")
            dataset = choose_dataset(ha, args.thread_network)
            print(f"  Thread network: {dataset.get('network_name')} "
                  f"(channel {dataset.get('channel')}, PAN ID {dataset.get('pan_id')})")

        how = "over Bluetooth" if args.bluetooth else "over the Thread network"
        if not confirm(f"Pair the device with this Home Assistant {how}?"):
            print("Cancelled.")
            return 1

        tlv = (ha.call("thread/get_dataset_tlv", dataset_id=dataset["dataset_id"])["tlv"]
               if dataset else None)
        # From here on the device's pairing events are shown as they arrive.
        device.start()
        if has_commands:
            start_pairing_mode(device)
        else:
            print("  (This firmware has no serial commands. If it stopped advertising, press "
                  "a switch to restart pairing mode.)")

        if args.bluetooth:
            if tlv:
                ha.call("matter/set_thread", thread_operation_dataset=tlv)
                print("Gave the Matter server the Thread network.")
            print("Home Assistant is pairing the device over Bluetooth. This can take a few "
                  "minutes.\n", flush=True)
        else:
            give_thread_network(device, tlv)
            print("Gave the device the Thread network over USB. Waiting for it to join...",
                  flush=True)
            wait_until_on_network(device, args.join_timeout)
            print("Home Assistant is pairing the device over the network. This can take a "
                  "minute or two.\n", flush=True)

        started = time.monotonic()
        try:
            ha.call("matter/commission", timeout=COMMISSION_TIMEOUT, code=qr,
                    network_only=not args.bluetooth)
            error = None
        except Error as err:
            error = err
        # Let the device's last events arrive.
        time.sleep(2)

        print()
        if error:
            explain_failure(error, device.events, args.bluetooth)
            return 1
        print(f"Paired in {time.monotonic() - started:.0f} s.")

        for _ in range(15):
            new = [d for i, d in matter_devices(ha).items() if i not in before]
            if new:
                print(f"Home Assistant device: {device_link(ha, new[0])}")
                break
            time.sleep(2)
        else:
            print("Home Assistant has not added the device yet; it appears under "
                  "Settings > Devices & services > Matter shortly.")
        print("\nYou can now disconnect USB and connect the battery. Never connect both.")
        return 0
    finally:
        ha.close()


def device_link(ha, device):
    name = device.get("name_by_user") or device.get("name") or "device"
    return f"{name}, {ha.base_url}/config/devices/device/{device['id']}"


def explain_failure(error, events, bluetooth):
    print(f"Pairing failed: {error}\n")
    if any("Pairing FAILED" in l for l in events):
        print("The device's explanation is above (the lines after \"Pairing FAILED\").")
    elif any("checking the pairing code" in l for l in events):
        print("Home Assistant reached the device but did not finish; the device's events "
              "above show the last step.")
    elif bluetooth:
        print("The device saw no pairing attempt, so Home Assistant could not reach it over "
              "Bluetooth. Check that:\n"
              "  - Bluetooth commissioning is enabled in the Matter server (the Matter Server\n"
              "    add-on's Bluetooth option, or --bluetooth-adapter for python-matter-server),\n"
              "  - the Home Assistant host has a working Bluetooth adapter,\n"
              "  - the device is within Bluetooth range of the Home Assistant host.")
    else:
        print("The device saw no pairing attempt, so Home Assistant did not find or reach it\n"
              "on the network. The Matter server finds it through the border router (mDNS)\n"
              "and reaches it over IPv6. Check that the Home Assistant host and the border\n"
              "router are on the same LAN, with IPv6 enabled and no VLAN or firewall between\n"
              "them, as for any other Thread device.")
    print("\nThe Matter server's log (Settings > Add-ons > Matter Server > Log) has Home "
          "Assistant's side. tools/triage.py can help investigate further.")


def main():
    parser = argparse.ArgumentParser(description=__doc__.strip().split("\n\n")[0])
    parser.add_argument("--url", default=os.environ.get("HA_URL", "http://homeassistant.local:8123"),
                        help="Home Assistant URL (default: $HA_URL or "
                             "http://homeassistant.local:8123)")
    parser.add_argument("--port", help="the device's serial port (default: find it)")
    parser.add_argument("--thread-network",
                        help="Thread network name to use (default: Home Assistant's preferred one)")
    parser.add_argument("--bluetooth", action="store_true",
                        help="let Home Assistant pair over Bluetooth instead (firmware without "
                             "the T command)")
    parser.add_argument("--keep-thread-network", action="store_true",
                        help="with --bluetooth: don't change the Matter server's Thread network")
    parser.add_argument("--join-timeout", type=float, default=120,
                        help="seconds to wait for the device to join Thread (default: 120)")
    parser.add_argument("--insecure", action="store_true",
                        help="don't verify the HTTPS certificate (self-signed certificates)")
    parser.add_argument("--serial-seconds", type=float, default=3.0,
                        help="how long to read the device's banner (default: 3)")
    args = parser.parse_args()
    try:
        return pair(args)
    except Error as err:
        print(f"\n{err}", file=sys.stderr)
        return 1
    except OSError as err:
        print(f"\nConnection failed: {err}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
