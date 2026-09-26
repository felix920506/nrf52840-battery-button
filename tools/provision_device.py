#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Create per-device Matter firmware images with unique onboarding credentials.

Every run generates, for each device: a random setup passcode, a random
discriminator, a random SPAKE2+ salt and a unique serial number. They go into
the device's own factory data, merged with the application of an existing
build into a device-specific UF2 (and hex) file. A pairing label (QR code
image and manual pairing code) is written next to it.

Only the SPAKE2+ verifier is stored on the device, not the passcode itself,
so the passcode can't be read back from the chip. Treat the label files as
secret: anyone who has the code can commission the device while its
commissioning window is open.

Run it with the nRF Connect SDK toolchain's Python, which has the required
modules (cbor2, cryptography, ecdsa, qrcode, jsonschema, intelhex):

    nrfutil sdk-manager toolchain launch --ncs-version v3.4.1 -- \\
        python3 tools/provision_device.py --build-dir build --count 3

To rebuild an existing device's image for a new firmware build while keeping
its credentials, pass its directory: --from-device build/devices/<serial>.

Output, per device, in <build-dir>/devices/<serial>/:
    <serial>.uf2           application + factory data, copy to the UF2 drive
    <serial>.hex           the same for serial DFU / SWD programming
    <serial>.png           pairing QR code
    <serial>.txt           QR code payload and manual pairing code
    factory_data.json      the factory data (verifier, salt, discriminator)
"""

import argparse
import datetime
import os
import re
import secrets
import struct
import subprocess
import sys

from intelhex import IntelHex

UF2_FAMILY_NRF52840 = 0xADA52840
# Setup passcodes the Matter specification forbids (section 5.1.7.1).
INVALID_PASSCODES = {0, 11111111, 22222222, 33333333, 44444444, 55555555,
                     66666666, 77777777, 88888888, 99999999, 12345678, 87654321}


def read_kconfig(path):
    config = {}
    for line in open(path):
        m = re.match(r"^(CONFIG_\w+)=(.*)$", line.strip())
        if m:
            config[m.group(1)] = m.group(2).strip('"')
    return config


def find_matter_root(build_dir):
    modules = os.path.join(build_dir, "zephyr_modules.txt")
    for line in open(modules):
        if line.startswith('"connectedhomeip":'):
            return line.split('"')[3]
    raise SystemExit("Matter SDK (connectedhomeip) not found in " + modules)


def factory_partition(app_dir):
    dts = open(os.path.join(app_dir, "zephyr", "zephyr.dts")).read()
    m = re.search(r"factory_data_partition: partition@[0-9a-f]+ \{.*?reg = < (0x[0-9a-f]+) (0x[0-9a-f]+) >", dts, re.S)
    if not m:
        raise SystemExit("No factory_data_partition in the build's devicetree")
    return int(m.group(1), 16), int(m.group(2), 16)


def random_passcode():
    while True:
        passcode = secrets.randbelow(99999998) + 1  # 00000001..99999998
        if passcode not in INVALID_PASSCODES:
            return passcode


def write_uf2(ih, path):
    """Write the hex image as UF2 blocks of 256 bytes (flash writes are page aligned)."""
    blocks = []
    for start, end in ih.segments():
        addr = start & ~0xFF
        while addr < end:
            data = bytes(ih.tobinarray(start=addr, size=256))
            blocks.append((addr, data))
            addr += 256
    with open(path, "wb") as f:
        for i, (addr, data) in enumerate(blocks):
            header = struct.pack("<IIIIIIII", 0x0A324655, 0x9E5D5157, 0x00002000, addr, 256,
                                 i, len(blocks), UF2_FAMILY_NRF52840)
            f.write(header + data + bytes(476 - len(data)) + struct.pack("<I", 0x0AB16F30))
    return len(blocks)


def provision(args, config, matter_root, app_hex, fd_offset, fd_size, serial):
    out_dir = os.path.join(args.out_dir, serial)
    os.makedirs(out_dir, exist_ok=True)
    passcode = random_passcode()
    discriminator = secrets.randbelow(4096)
    salt = secrets.token_bytes(32)

    vid = int(config["CONFIG_CHIP_DEVICE_VENDOR_ID"])
    pid = int(config["CONFIG_CHIP_DEVICE_PRODUCT_ID"])
    attestation = os.path.join(matter_root, "credentials", "development", "attestation")
    dac = f"Matter-Development-DAC-{vid:04X}-{pid:04X}"
    pai = f"Matter-Development-PAI-{vid:04X}-noPID-Cert.der"

    import base64
    generator = os.path.join(matter_root, "scripts", "tools", "nrfconnect",
                             "generate_nrfconnect_chip_factory_data.py")
    cmd = [sys.executable, generator,
           "--sn", serial,
           "--date", datetime.date.today().isoformat(),
           "--vendor_id", str(vid), "--product_id", str(pid),
           "--vendor_name", config["CONFIG_CHIP_DEVICE_VENDOR_NAME"],
           "--product_name", config["CONFIG_CHIP_DEVICE_PRODUCT_NAME"],
           "--hw_ver", config["CONFIG_CHIP_DEVICE_HARDWARE_VERSION"],
           "--hw_ver_str", config["CONFIG_CHIP_DEVICE_HARDWARE_VERSION_STRING"],
           "--product_finish", config.get("CONFIG_CHIP_DEVICE_PRODUCT_FINISH", "other"),
           "--dac_cert", os.path.join(attestation, dac + "-Cert.der"),
           "--dac_key", os.path.join(attestation, dac + "-Key.der"),
           "--pai_cert", os.path.join(attestation, pai),
           "--spake2_it", config["CONFIG_CHIP_DEVICE_SPAKE2_IT"],
           "--spake2_salt", base64.b64encode(salt).decode(),
           "--discriminator", str(discriminator),
           "--passcode", str(passcode),
           "--generate_onboarding",
           "--offset", hex(fd_offset), "--size", hex(fd_size),
           "-s", os.path.join(os.path.dirname(generator), "nrfconnect_factory_data.schema"),
           "-o", os.path.join(out_dir, "factory_data"),
           "--overwrite"]
    if config.get("CONFIG_CHIP_ROTATING_DEVICE_ID") == "y":
        cmd.append("--generate_rd_uid")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"Factory data generation failed:\n{result.stdout}\n{result.stderr}")

    fd_hex = IntelHex(os.path.join(out_dir, "factory_data.hex"))
    image = IntelHex(app_hex)
    if image.maxaddr() >= fd_offset:
        raise SystemExit("Application overlaps the factory data partition")
    image.merge(fd_hex, overlap="error")
    image.write_hex_file(os.path.join(out_dir, serial + ".hex"))
    write_uf2(image, os.path.join(out_dir, serial + ".uf2"))

    # The generator names its onboarding outputs after -o: factory_data.png / .txt
    for ext in ("png", "txt"):
        src = os.path.join(out_dir, "factory_data." + ext)
        if os.path.exists(src):
            os.replace(src, os.path.join(out_dir, f"{serial}.{ext}"))
    codes = open(os.path.join(out_dir, serial + ".txt")).read()
    qr = re.search(r"MT:[0-9A-Z.\-]+", codes)
    manual = re.search(r"\b\d{11}\b", codes)
    return out_dir, qr.group(0) if qr else "?", manual.group(0) if manual else "?", discriminator


def rebuild(app_hex, fd_offset, device_dir):
    """New application, the device's existing factory data (and pairing code)."""
    serial = os.path.basename(os.path.normpath(device_dir))
    fd_hex = IntelHex(os.path.join(device_dir, "factory_data.hex"))
    if fd_hex.minaddr() != fd_offset:
        raise SystemExit(f"{serial}: factory data at {hex(fd_hex.minaddr())}, but this build expects {hex(fd_offset)}")
    image = IntelHex(app_hex)
    if image.maxaddr() >= fd_offset:
        raise SystemExit("Application overlaps the factory data partition")
    image.merge(fd_hex, overlap="error")
    image.write_hex_file(os.path.join(device_dir, serial + ".hex"))
    write_uf2(image, os.path.join(device_dir, serial + ".uf2"))
    return serial


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build-dir", required=True, help="sysbuild build directory of the Matter firmware")
    parser.add_argument("--count", type=int, default=1, help="number of devices to create")
    parser.add_argument("--serial", action="append", default=[],
                        help="serial number(s) to use instead of random ones (max. 20 characters)")
    parser.add_argument("--out-dir", help="output directory (default: <build-dir>/devices)")
    parser.add_argument("--from-device", action="append", default=[], metavar="DIR",
                        help="rebuild the image of an already provisioned device (its directory "
                             "from an earlier run) with this build's application, same credentials")
    args = parser.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    args.out_dir = os.path.abspath(args.out_dir or os.path.join(build_dir, "devices"))
    domain = re.search(r"^default:\s*(\S+)", open(os.path.join(build_dir, "domains.yaml")).read(), re.M).group(1)
    app_dir = os.path.join(build_dir, domain)
    config = read_kconfig(os.path.join(app_dir, "zephyr", ".config"))
    if config.get("CONFIG_CHIP_FACTORY_DATA") != "y":
        raise SystemExit("This build doesn't use factory data (CONFIG_CHIP_FACTORY_DATA is not set)")
    matter_root = find_matter_root(build_dir)
    fd_offset, fd_size = factory_partition(app_dir)
    app_hex = os.path.join(app_dir, "zephyr", "zephyr.hex")

    if args.from_device:
        for device_dir in args.from_device:
            serial = rebuild(app_hex, fd_offset, os.path.abspath(device_dir))
            print(f"{serial}: rebuilt with the new application, pairing code unchanged")
            print(f"    {os.path.relpath(os.path.abspath(device_dir))}/{serial}.uf2")
        return 0

    serials = args.serial or [f"BSW-{secrets.token_hex(6).upper()}" for _ in range(args.count)]
    for serial in serials:
        if len(serial) > 20:
            raise SystemExit(f"Serial number {serial!r} is longer than 20 characters")
        out_dir, qr, manual, discriminator = provision(args, config, matter_root, app_hex,
                                                       fd_offset, fd_size, serial)
        print(f"{serial}: QR {qr}  manual code {manual}  discriminator {discriminator}")
        print(f"    {os.path.relpath(out_dir)}/{serial}.uf2")
    return 0


if __name__ == "__main__":
    sys.exit(main())
