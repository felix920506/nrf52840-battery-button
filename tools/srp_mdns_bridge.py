#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Republish Matter services registered with an OpenThread SRP server (ot-daemon)
on the host's mDNS, using `dns-sd -P` proxy registrations (macOS).

macOS mDNSResponder does not run on the point-to-point utun interface that
ot-daemon uses, so chip-tool cannot otherwise discover Thread devices. Both
commissionable (_matterc._udp, including its subtypes such as _L3840) and
operational (_matter._tcp) services are bridged.

    python3 tools/srp_mdns_bridge.py --ot-ctl "ot-ctl -I utun11"
"""

import argparse
import re
import shlex
import subprocess
import sys
import time

TYPES = ("_matter._tcp", "_matterc._udp")


def srp_services(ot_ctl):
    out = subprocess.run(ot_ctl + ["srp", "server", "service"], capture_output=True, text=True).stdout
    services, cur = [], None
    for line in out.splitlines():
        if line and not line.startswith(" ") and line != "Done":
            cur = {"name": line.strip()}
            services.append(cur)
        elif cur is not None and ":" in line:
            key, _, value = line.strip().partition(":")
            cur[key] = value.strip()
    return services


def parse(svc):
    """Return (instance, type[,subtypes], port, host, address, txt) or None."""
    full = svc["name"]  # <instance>.<type>.default.service.arpa.
    for t in TYPES:
        marker = "." + t + "."
        if marker not in full or svc.get("deleted") != "false":
            continue
        instance = full.split(marker)[0]
        host = svc["host"].split(".")[0] + ".local"
        addrs = [a.strip() for a in svc.get("addresses", "").strip("[]").split(",") if a.strip()]
        addr = next((a for a in addrs if not a.lower().startswith("fe80")), addrs[0] if addrs else None)
        if not addr:
            return None
        txt = []
        for entry in re.findall(r"[^,\[\]]+", svc.get("TXT", "")):
            key, sep, value = entry.strip().partition("=")
            if sep:
                try:
                    value = bytes.fromhex(value).decode()
                except ValueError:
                    pass
                txt.append(f"{key}={value}")
        subtypes = svc.get("subtypes", "(null)")
        if subtypes and subtypes != "(null)":
            # "_L3840._sub._matterc._udp..." -> "_L3840"
            t = ",".join([t] + [x.strip().split(".")[0] for x in subtypes.split(",") if x.strip()])
        return (instance, t, svc["port"], host, addr, tuple(txt))
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--ot-ctl", default="ot-ctl", help='ot-ctl command, e.g. "ot-ctl -I utun11"')
    parser.add_argument("--interval", type=float, default=3.0, help="SRP polling interval [s]")
    args = parser.parse_args()
    ot_ctl = shlex.split(args.ot_ctl)

    running = {}
    while True:
        wanted = {}
        for svc in srp_services(ot_ctl):
            p = parse(svc)
            if p:
                wanted[p[0] + p[1]] = p
        for key in list(running):
            if key not in wanted or running[key][0] != wanted[key] or running[key][1].poll() is not None:
                running[key][1].terminate()
                del running[key]
                print(time.strftime("%H:%M:%S"), "withdrawn", key, flush=True)
        for key, p in wanted.items():
            if key not in running:
                instance, t, port, host, addr, txt = p
                proc = subprocess.Popen(["dns-sd", "-P", instance, t, "local", port, host, addr, *txt],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                running[key] = (p, proc)
                print(time.strftime("%H:%M:%S"), "published", instance, t, f"[{addr}]:{port}", flush=True)
        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
