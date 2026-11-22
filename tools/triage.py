#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Start a coding agent (Claude Code, Codex or Antigravity) that helps you find
out why a battery switch is not working, and helps you file a good issue.

The script writes a triage folder with two files: context.md, with facts
about this computer and any connected board (serial ports, bootloader drives,
the device's pairing status), and the triage playbook the agent follows. It
then starts the agent in that folder. The agent asks what went wrong and asks
before it runs anything that changes your device or posts anything.

    python3 tools/triage.py
    python3 tools/triage.py --agent codex

Without a clone of this repository, download just this file:

    curl -fsSLO https://raw.githubusercontent.com/felix920506/nrf52840-battery-button/master/tools/triage.py
    python3 triage.py

Only the Python standard library is needed. On Windows, reading the device's
serial port also needs pyserial (pip install pyserial).

The device serial number, Thread network credentials and the home directory
are never written to context.md. The pairing code and QR code payload are
kept: the agent may need them to help the user pair.
"""

import argparse
import datetime
import glob
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

REPO = "felix920506/nrf52840-battery-button"
PLAYBOOK_NAME = "triage_playbook.md"
PLAYBOOK_URL = f"https://raw.githubusercontent.com/{REPO}/master/tools/{PLAYBOOK_NAME}"

# (id, command, label, extra arguments before the prompt)
AGENTS = (
    ("claude", "claude", "Claude Code", ()),
    ("codex", "codex", "Codex", ()),
    ("antigravity", "agy", "Antigravity", ("--prompt-interactive",)),
)

# USB IDs worth naming in context.md.
KNOWN_USB = {
    (0x2FE3, 0x0004): "this firmware's USB serial port (release or usb-logging build)",
    (0x2886, None): "Seeed Studio (XIAO bootloader or other XIAO firmware)",
    (0x239A, None): "Adafruit (UF2 bootloader or other Adafruit/nice!nano firmware)",
    (0x1915, None): "Nordic Semiconductor (nRF52840 Dongle bootloader or Nordic firmware)",
}

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
SECRET_PATTERNS = (
    (re.compile(r"(?i)(serial number[:\s]+)[0-9A-Fa-f]+"), r"\1<redacted>"),
    (re.compile(r"(?i)(network ?key|pskc|dataset)([:\s]+)\S+"), r"\1\2<redacted>"),
)


def base_dir():
    override = os.environ.get("BATTERY_SWITCH_TRIAGE_HOME")
    if override:
        return Path(override).expanduser()
    home = Path.home()
    system = platform.system()
    if system == "Darwin":
        root = home / "Library" / "Application Support"
    elif system == "Windows":
        root = Path(os.environ.get("LOCALAPPDATA", home / "AppData" / "Local"))
    else:
        root = Path(os.environ.get("XDG_STATE_HOME", home / ".local" / "state"))
    return root / "nrf52840-battery-button"


def run(cmd, cwd=None, timeout=10):
    try:
        out = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return None
    return out.stdout.strip() if out.returncode == 0 else None


def redact(text):
    for pattern, replacement in SECRET_PATTERNS:
        text = pattern.sub(replacement, text)
    return text.replace(str(Path.home()), "~")


# --- Serial ports ----------------------------------------------------------


def list_serial_ports():
    """[(device, vid, pid, description)] from pyserial, or a best effort without it."""
    try:
        from serial.tools import list_ports
    except ImportError:
        list_ports = None
    if list_ports is not None:
        return [(p.device, p.vid, p.pid, p.description or "") for p in list_ports.comports()], True

    ports = []
    system = platform.system()
    if system == "Darwin":
        names = sorted(glob.glob("/dev/cu.usbmodem*"))
    elif system == "Linux":
        names = sorted(glob.glob("/dev/ttyACM*"))
    else:
        names = []
    mac_ids = {}
    if system == "Darwin":
        # Top-level "+-o" lines start a USB device; its serial port is nested below.
        vid = pid = None
        for line in (run(["ioreg", "-r", "-c", "IOUSBHostDevice", "-l", "-w0"]) or "").splitlines():
            if line.startswith("+-o"):
                vid = pid = None
            elif (m := re.search(r'"idVendor" = (\d+)', line)) and vid is None:
                vid = int(m.group(1))
            elif (m := re.search(r'"idProduct" = (\d+)', line)) and pid is None:
                pid = int(m.group(1))
            elif m := re.search(r'"IOCalloutDevice" = "([^"]+)"', line):
                mac_ids[m.group(1)] = (vid, pid)
    for name in names:
        vid, pid = mac_ids.get(name, (None, None))
        sys_dev = Path("/sys/class/tty") / Path(name).name / "device"
        for parent in (sys_dev / "..", sys_dev / ".." / ".."):
            try:
                vid = int((parent / "idVendor").read_text(), 16)
                pid = int((parent / "idProduct").read_text(), 16)
                break
            except (OSError, ValueError):
                continue
        ports.append((name, vid, pid, ""))
    return ports, False


def describe_usb(vid, pid):
    if vid is None:
        return "USB ID unknown"
    label = KNOWN_USB.get((vid, pid)) or KNOWN_USB.get((vid, None)) or "not a known board"
    return f"{vid:04x}:{pid:04x}, {label}"


class Port:
    """The device's serial port at 115200 baud with DTR set. Never 1200 baud:
    on usb-logging builds, that reboots the board into the bootloader."""

    def __init__(self, device):
        try:
            import serial
        except ImportError:
            serial = None
        self.serial = None
        self.fd = None
        if serial is not None:
            try:
                self.serial = serial.Serial(device, 115200, timeout=0.1)
            except (OSError, serial.SerialException) as err:
                raise OSError(f"could not open: {err}") from err
            # pyserial empties the input buffer after opening, which can drop
            # the start of the banner: drop DTR and set it again so the
            # firmware prints it once more.
            self.serial.dtr = False
            time.sleep(0.3)
            self.serial.reset_input_buffer()
            self.serial.dtr = True
            return
        if os.name != "posix":
            raise OSError("not read: install pyserial to read serial ports on this OS")

        import fcntl
        import struct
        import termios
        import tty

        self.fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            tty.setraw(self.fd)
            attrs = termios.tcgetattr(self.fd)
            attrs[4] = attrs[5] = termios.B115200
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            # As above: set DTR again so the whole banner arrives after this point.
            dtr = struct.pack("I", termios.TIOCM_DTR)
            fcntl.ioctl(self.fd, termios.TIOCMBIC, dtr)
            time.sleep(0.3)
            termios.tcflush(self.fd, termios.TCIFLUSH)
            fcntl.ioctl(self.fd, termios.TIOCMBIS, dtr)
        except OSError:
            self.close()
            raise

    def _read(self, wait):
        if self.serial is not None:
            return self.serial.read(4096)
        import select

        ready, _, _ = select.select([self.fd], [], [], wait)
        if not ready:
            return b""
        try:
            return os.read(self.fd, 4096)
        except BlockingIOError:
            return b""

    def read(self, seconds, until=None, quiet=None):
        """Reads for `seconds`, or until the text `until` arrived, or until
        nothing arrived for `quiet` seconds after something did."""
        data = b""
        end = time.monotonic() + seconds
        last = None
        while (left := end - time.monotonic()) > 0:
            chunk = self._read(min(left, 0.1))
            now = time.monotonic()
            if chunk:
                data += chunk
                last = now
                if until and until.encode() in data:
                    break
            elif quiet and last and now - last >= quiet:
                break
        return data.decode(errors="replace")

    def send(self, key):
        if self.serial is not None:
            self.serial.write(key.encode())
        else:
            os.write(self.fd, key.encode())

    def close(self):
        if self.serial is not None:
            self.serial.close()
        elif self.fd is not None:
            os.close(self.fd)
            self.fd = None


REPORT_END = "--- END DIAGNOSTICS ---"


def read_device(device, seconds):
    """Returns (banner, report, error). The banner is what the firmware
    prints when the port opens. The report is the output of the read-only
    D command, or of S and L on firmware without it. No other key is ever
    sent: other keys start or stop pairing mode, show the pairing code, or
    begin a factory reset."""
    try:
        port = Port(device)
    except OSError as err:
        return None, None, str(err)
    try:
        banner = port.read(seconds, quiet=1.0)
        # Firmware from before the pairing console prints a banner without
        # this and ignores keys. No banner at all: another terminal probably
        # has the port open, so the firmware didn't print it again.
        if banner.strip() and "Press H for commands" not in banner:
            return banner, None, None
        port.send("D")
        report = port.read(8, until=REPORT_END, quiet=1.5)
        if "Unknown command" in report:
            report = ""
            for key in ("S", "L"):
                port.send(key)
                report += port.read(5, quiet=1.0)
        return banner, report, None
    except OSError as err:
        return None, None, f"read failed: {err}"
    finally:
        port.close()


def summarize_serial(text):
    """The banner without the QR code drawing and without the serial number."""
    lines = []
    for raw in text.replace("\r", "").split("\n"):
        if "\x1b[30;107m" in raw:  # A row of the QR code drawing.
            continue
        line = ANSI.sub("", raw).rstrip()
        if line.strip():
            lines.append(redact(line))
    status = next((l.split(":", 1)[1].strip() for l in lines if l.startswith("Status:")), None)
    return status, lines[-400:]


# --- UF2 bootloader drives -------------------------------------------------


def uf2_drives():
    system = platform.system()
    if system == "Darwin":
        roots = glob.glob("/Volumes/*")
    elif system == "Windows":
        roots = [f"{letter}:\\" for letter in "DEFGHIJKLMNOPQRSTUVWXYZ"]
    else:
        roots = glob.glob("/media/*/*") + glob.glob("/run/media/*/*") + glob.glob("/mnt/*")
    drives = []
    for root in roots:
        info = Path(root) / "INFO_UF2.TXT"
        try:
            # Only INFO_UF2.TXT: CURRENT.UF2 is a dump of the flash, pairing secrets included.
            drives.append((root, info.read_text(errors="replace")[:2000]))
        except OSError:
            continue
    return drives


# --- This repository -------------------------------------------------------


def repo_facts(script_dir):
    top = run(["git", "rev-parse", "--show-toplevel"], cwd=script_dir)
    if not top or not (Path(top) / "src" / "core" / "app_loop.c").exists():
        return ["- Not running from a clone of the repository."]
    facts = [
        f"- Clone: {redact(top)}",
        f"- Branch: {run(['git', 'rev-parse', '--abbrev-ref', 'HEAD'], cwd=top)}",
        f"- Version (git describe): {run(['git', 'describe', '--tags', '--always', '--dirty'], cwd=top)}",
    ]
    for config in sorted(Path(top).glob("build*/*/zephyr/.config")) + sorted(
        Path(top).glob("build*/zephyr/.config")
    ):
        try:
            text = config.read_text(errors="replace")
        except OSError:
            continue
        board = re.search(r'^CONFIG_BOARD_TARGET="([^"]*)"', text, re.M)
        version = re.search(r'^CONFIG_CHIP_DEVICE_SOFTWARE_VERSION_STRING="([^"]*)"', text, re.M)
        facts.append(
            f"- Build: {redact(str(config.parent.parent))} "
            f"(board {board.group(1) if board else 'unknown'}, "
            f"version {version.group(1) if version else 'unknown'})"
        )
    return facts


# --- Context file ----------------------------------------------------------


def build_context(args, paths):
    now = datetime.datetime.now().astimezone()
    script_dir = Path(__file__).resolve().parent
    out = [
        "# Triage context",
        "",
        f"Generated {now.isoformat(timespec='seconds')} by tools/triage.py.",
        "Home directory paths are shown as `~`; serial numbers and Thread network",
        "credentials were removed. The device shows its pairing code only on the",
        "serial console, so the user may not have seen it: if they need it to",
        "pair, give it to them (see the playbook).",
        "",
        "## Computer",
        "",
        f"- OS: {platform.system()} {platform.release()} ({platform.machine()})",
        f"- Python: {platform.python_version()}",
    ]
    tools = ["git", "gh", "west", "nrfutil", "adafruit-nrfutil", "screen", "chip-tool", "ot-ctl"]
    out.append("- Tools on PATH: " + (", ".join(t for t in tools if shutil.which(t)) or "none"))
    modules = []
    for module in ("serial", "bleak", "intelhex"):
        try:
            __import__(module)
            modules.append(module)
        except ImportError:
            pass
    out.append("- Python modules: " + (", ".join(modules) or "none of serial, bleak, intelhex"))

    out += ["", "## This repository", ""] + repo_facts(script_dir)

    out += ["", "## Serial ports", ""]
    ports, from_pyserial = list_serial_ports()
    if not from_pyserial:
        out.append("(pyserial is not installed; the list below may be incomplete.)")
    if not ports:
        out.append("- None found. A board on battery, without USB, or running a")
        out.append("  firmware without a USB serial port does not show one.")
    for device, vid, pid, description in ports:
        out.append(f"- {device}: {describe_usb(vid, pid)}" + (f" ({description})" if description not in ("", "n/a") else ""))

    ours = [p for p in ports if (p[1], p[2]) == (0x2FE3, 0x0004)]
    if ours and not args.no_serial:
        for device, *_ in ours:
            banner, report, error = read_device(device, args.serial_seconds)
            out += ["", f"### {device}", ""]
            if error:
                out.append(f"- {error}")
                continue
            status, banner_lines = summarize_serial(banner or "")
            out.append(f"- Pairing status: {status or 'not printed'}")
            if report and report.strip():
                # The report has the firmware, status and full event history.
                _, report_lines = summarize_serial(report)
                out += ["", "Diagnostics read from the device (read-only command):", "",
                        "```text"] + report_lines + ["```"]
            else:
                if not banner_lines:
                    out.append("- Nothing received. Another program may have the port open.")
                else:
                    out.append("- This firmware has no diagnostics command; banner only.")
                out += ["", "```text"] + (banner_lines or ["(nothing received)"]) + ["```"]
    elif ours:
        out += ["", "The device's serial port was not read (--no-serial)."]

    out += ["", "## UF2 bootloader drives", ""]
    drives = uf2_drives()
    if not drives:
        out.append("- None mounted. The drive only appears after a double-press of reset.")
    for root, info in drives:
        out += [f"- {redact(root)}, INFO_UF2.TXT:", "", "```text", info.strip(), "```"]

    out += [
        "",
        "## Paths",
        "",
        f"- Triage folder: {redact(str(paths['session']))}",
        f"- Source cache (one clone per commit): {redact(str(paths['source']))}",
        "",
        "## Not known from this computer",
        "",
        "- The firmware version, if no device output is shown above: Home",
        "  Assistant shows it on the device page (Firmware); the hardware version",
        "  there is the board the firmware was built for. Newer firmware prints",
        "  both on the serial port.",
        "- Anything about the Matter controller or the Thread border router.",
    ]
    return "\n".join(out) + "\n"


# --- Playbook and agent ----------------------------------------------------


def load_playbook():
    local = Path(__file__).resolve().with_name(PLAYBOOK_NAME)
    if local.exists():
        return local.read_text(), str(local)
    try:
        with urllib.request.urlopen(PLAYBOOK_URL, timeout=15) as response:
            return response.read().decode(), PLAYBOOK_URL
    except OSError as err:
        sys.exit(f"{PLAYBOOK_NAME} is not next to this script and could not be downloaded: {err}")


def seed_prompt(context_path, playbook):
    return (
        "A user of the nRF52840 battery switch firmware has a problem with a device\n"
        "and started this session with tools/triage.py.\n\n"
        "Facts about this computer and any connected board are in the context file:\n\n"
        f"    {context_path}\n\n"
        "Follow the playbook below, starting by asking the user what went wrong.\n\n"
        "---\n\n" + playbook
    )


def pick_agent(installed):
    menu = "\n".join(f"  [{i + 1}] {agent[2]}" for i, agent in enumerate(installed))
    while True:
        answer = input(f"Run triage with:\n{menu}\n> ").strip().lower()
        if answer.isdigit() and 1 <= int(answer) <= len(installed):
            return installed[int(answer) - 1]
        for agent in installed:
            if answer == agent[0]:
                return agent


def main():
    parser = argparse.ArgumentParser(description=__doc__.strip().split("\n\n")[0])
    parser.add_argument("--agent", choices=[a[0] for a in AGENTS],
                        help="agent CLI to use (default: ask when several are installed)")
    parser.add_argument("--model", help="model passed to the agent CLI (default: its own default)")
    parser.add_argument("--no-serial", action="store_true",
                        help="don't open the device's serial port")
    parser.add_argument("--serial-seconds", type=float, default=3.0,
                        help="how long to read the serial port (default: 3)")
    parser.add_argument("--no-launch", action="store_true",
                        help="only write the triage files and print their paths")
    args = parser.parse_args()

    base = base_dir()
    stamp = datetime.datetime.now().strftime("%Y-%m-%dT%H-%M-%S")
    paths = {"session": base / "triage" / stamp, "source": base / "source"}
    paths["session"].mkdir(parents=True, exist_ok=True)
    paths["source"].mkdir(parents=True, exist_ok=True)

    print("Collecting facts about this computer and connected boards...")
    playbook, _ = load_playbook()
    context_path = paths["session"] / "context.md"
    context_path.write_text(build_context(args, paths))
    prompt_path = paths["session"] / "triage-playbook.md"
    prompt_path.write_text(seed_prompt(context_path, playbook))

    files = f"The triage prompt and context were written to:\n  {prompt_path}\n  {context_path}"
    installed = [a for a in AGENTS if shutil.which(a[1])]
    if args.no_launch:
        selected = None
    elif args.agent:
        selected = next((a for a in installed if a[0] == args.agent), None)
        if selected is None:
            sys.exit(f"`{dict((a[0], a[1]) for a in AGENTS)[args.agent]}` was not found on PATH.")
    elif len(installed) == 1:
        selected = installed[0]
    elif installed:
        if not (sys.stdin.isatty() and sys.stdout.isatty()):
            sys.exit("Several agent CLIs are installed; choose one with --agent.")
        selected = pick_agent(installed)
    else:
        selected = None

    if args.no_launch or selected is None:
        if selected is None and not args.no_launch:
            print("No supported agent CLI (claude, codex, agy) was found.\n")
        print(files)
        print("\nPaste the prompt file into any coding agent to run triage by hand.")
        return 0

    # One line only: Windows .cmd shims can't pass multi-line arguments.
    launch = (f"Read the file {prompt_path} and follow its instructions exactly: it is your "
              "triage playbook, and it starts with asking the user what went wrong.")
    command = [shutil.which(selected[1])]
    if args.model:
        command += ["--model", args.model]
    command += [*selected[3], launch]
    print(f"Starting {selected[2]}. It will ask what went wrong.\n", flush=True)
    try:
        code = subprocess.call(command, cwd=paths["session"])
    except OSError as err:
        sys.exit(f"Could not start `{selected[1]}`: {err}\n{files}")
    if code != 0:
        print(f"\n{selected[2]} exited with code {code}.\n{files}")
    return code


if __name__ == "__main__":
    sys.exit(main())
