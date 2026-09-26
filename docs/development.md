# Development and testing

[Back to the README](../README.md)

This guide collects development workflows and implementation details. Normal
users can install a prebuilt release using the [README Quick start](../README.md#quick-start).

## Code layout

```text
src/main.c                    event loop, switch type changes, battery timer
src/core/app_loop.*           ISR-safe event queue
src/core/switch_input.*       GPIO, debounce, low-power polling of closed rockers
src/core/switch_gesture.*     press/long/multi-press state machine
src/core/battery.*            VDD measurement and discharge curves
src/core/status_led.*         LED patterns
src/core/switch_config.*      stored switch types
src/core/reset_pin.*          factory reset input
src/core/usb_info.*           serial pairing code and QR code
src/transport/transport.h     transport interface
src/transport/matter/         Matter-over-Thread implementation
src/transport/ble/            Bluetooth LE test transport
boards/                       per-board pins and flash layout
tools/                        provisioning and test utilities
```

## Adding a transport

The core has no Matter dependencies. It emits `struct switch_event`s and
`struct battery_state`s through
[`src/transport/transport.h`](../src/transport/transport.h). To add a transport:

1. Implement the interface in a directory such as `src/transport/zigbee/`.
2. Add a `CONFIG_APP_TRANSPORT_ZIGBEE` choice in `Kconfig` and sources in
   `CMakeLists.txt`.
3. Disable `SB_CONFIG_MATTER` in `sysbuild.conf` for that build, for example
   with a separate sysbuild config selected through `-DSB_CONF_FILE=`.

Switch events map to Zigbee Multistate Input or BLE GATT notifications.

## Changing the Matter data model

Edit `src/default_zap/battery_switch.zap` with `west zap-gui`, then regenerate:

```sh
west zap-generate -z src/default_zap/battery_switch.zap
```

This rewrites `battery_switch.matter` and the generated code under
`zap-generated/`.

## Bluetooth LE test build

The BLE variant replaces Matter with a small GATT server and sends equivalent
switch events and battery level. It is a convenient way to test switch inputs
without a Thread network.

```sh
west build -b xiao_ble/nrf52840 --sysbuild -d build-ble -- \
  -DFILE_SUFFIX=ble -DEXTRA_CONF_FILE=usb-logging.conf
pip install bleak
python3 tools/ble_test_client.py
```

The client can change the switch type, for example:
`python3 tools/ble_test_client.py --set-type 2 latching`.

## Testing Matter over Thread on macOS

The macOS test setup uses an nRF52840 Dongle running OpenThread RCP firmware,
`ot-daemon` as the Thread network, and `chip-tool` as the Matter controller.
It requires the nRF Connect SDK v3.4.1, an Apple silicon Mac, and substantial
build tools and disk space (the `chip-tool` build is about 6 GB and 20 minutes).

macOS cannot commission third-party Matter BLE services, and its mDNS responder
ignores the point-to-point `utun` interface used by `ot-daemon`. The commands
below use `tools/srp_mdns_bridge.py` to publish SRP registrations. Start
`ot-daemon` with SIGPIPE ignored to avoid it exiting when an `ot-ctl` client
disconnects early.

> **Be a good neighbour:** scan nearby Thread networks and choose a channel
> that is not in use. Never start Thread before confirming the dataset;
> OpenThread otherwise starts its built-in default network. The dataset
> provisioning utility checks the selected dataset.

Set `$NCS` to the SDK directory, `$WORK` to a scratch directory outside this
repository, and `$REPO` to this repository. Run west through the SDK toolchain.

### 1. Dongle RCP firmware

Put the Dongle into its bootloader with its sideways reset button, then build
and flash the RCP:

```sh
cd $NCS
west build -b nrf52840dongle/nrf52840 --sysbuild -d $WORK/build-rcp nrf/samples/openthread/coprocessor
nrfutil install nrf5sdk-tools
nrfutil nrf5sdk-tools pkg generate --hw-version 52 --sd-req=0x00 \
  --application $WORK/build-rcp/coprocessor/zephyr/zephyr.hex --application-version 1 $WORK/rcp.zip
nrfutil nrf5sdk-tools dfu usb-serial -pkg $WORK/rcp.zip -p /dev/cu.usbmodemXXXX
```

The Dongle reappears as “Thread Co-Processor” and its serial port name changes.
Identify both boards by USB serial number to avoid mixing them up.

### 2. Build and start OpenThread tools

Build tools from the SDK sources so they match the RCP firmware:

```sh
cmake -GNinja -S $NCS/modules/lib/openthread -B $WORK/ot-build \
  -DOT_PLATFORM=posix -DOT_DAEMON=ON -DOT_FTD=ON -DOT_MTD=OFF -DOT_RCP=OFF \
  -DOT_SRP_SERVER=ON -DOT_ECDSA=ON -DOT_SERVICE=ON -DOT_NETDATA_PUBLISHER=ON \
  -DOT_LOG_OUTPUT=PLATFORM_DEFINED -DOT_BUILD_EXECUTABLES=ON
ninja -C $WORK/ot-build ot-daemon ot-ctl
```

Run the daemon in its own terminal session:

```sh
mkdir -p $WORK/ot-run
sudo -b nohup sh -c "trap '' PIPE; cd $WORK/ot-run && exec $WORK/ot-build/src/posix/ot-daemon -v -d 4 'spinel+hdlc+uart:///dev/cu.usbmodem<Dongle>' > ot-daemon.log 2>&1"
sleep 3; sudo chmod 666 /tmp/openthread-utun*.sock
```

It creates a `utunN` interface. Set `OTCTL="$WORK/ot-build/src/posix/ot-ctl -I utunN"` for the commands below. Stop the daemon with `sudo pkill -f ot-build/src/posix/ot-daemon`.

### 3. Create a Thread network

```sh
$OTCTL ifconfig up
$OTCTL scan energy 200
$OTCTL scan
$OTCTL dataset init new
$OTCTL dataset channel 19
$OTCTL dataset channelmask 0x00080000
$OTCTL dataset networkname BattSwitchTest
$OTCTL dataset
$OTCTL dataset commit active
$OTCTL thread start
$OTCTL srp server enable
```

Choose an unused channel based on the scans and set the corresponding channel
mask (`1 << channel`). Restricting the mask prevents leader announcements on
other channels. The Mac becomes the leader; after daemon restart repeat
`ifconfig up`, `thread start`, and `srp server enable`.

### 4. Build `chip-tool`

The Nordic prebuilt tool is for Linux. Build the macOS version (about 6 GB and
20 minutes):

```sh
git clone --depth 1 --branch v3.4.1 https://github.com/nrfconnect/sdk-connectedhomeip.git $WORK/chip
cd $WORK/chip
python3 scripts/checkout_submodules.py --shallow --platform darwin
source scripts/bootstrap.sh -p all,darwin
gn gen out/chip-tool --root=examples/chip-tool && ninja -C out/chip-tool chip-tool
```

### 5. Build development firmware

This test-only image has no factory data and uses public Matter test
credentials; do not use it outside an isolated test setup.

```sh
west build -b xiao_ble/nrf52840 --sysbuild -d $WORK/build-dev $REPO -- \
  -DSB_CONFIG_MATTER_FACTORY_DATA_GENERATE=n \
  "-DEXTRA_CONF_FILE=usb-logging.conf;dev/no-factory-data.conf;dev/ot-shell.conf" \
  -DEXTRA_DTC_OVERLAY_FILE=dev/large-app.overlay
```

Double-press reset and flash with:

```sh
cp -X $WORK/build-dev/battery_switch_app.uf2 /Volumes/XIAO-BOOT/
```

### 6. Put the switch on the network

```sh
pip install pyserial
python3 $REPO/tools/provision_thread_dataset.py --ot-ctl "$OTCTL" \
  --serial-number <XIAO USB serial> --expect-channel 19 --avoid-pan 0x<neighbour PAN>
python3 $REPO/tools/srp_mdns_bridge.py --ot-ctl "$OTCTL"
```

Keep the bridge running. The switch attaches as a sleepy child and registers
its commissionable service with the SRP server.

### 7. Commission and inspect events

```sh
CT=$WORK/chip/out/chip-tool/chip-tool
S="--storage-directory $WORK/chip-storage"
$CT pairing onnetwork-long 1 20202021 3840 $S \
  --paa-trust-store-path $WORK/chip/credentials/development/paa-root-certs \
  --bypass-attestation-verifier true
$CT descriptor read parts-list 1 0 $S
$CT powersource read bat-voltage 1 0 $S
echo "switch subscribe-event-by-id 0xFFFFFFFF 1 60 1 0xFFFF" | $CT interactive start $S
```

Press the inputs to observe Switch cluster events on endpoints 1–6. Clean up
with `$CT pairing unpair 1 $S`, stop `ot-daemon`, and flash normal firmware.

For development firmware, no factory data is generated; it uses the Matter SDK
test passcode `20202021`, discriminator `3840`, an OpenThread shell, and USB
logging. Use it only in an isolated test setup.

## Power design

- The device is a Thread Sleepy End Device and Matter ICD (LIT capable); it
  runs as SIT with a 5-second slow poll until a controller registers for
  check-ins. TX power is 0 dBm.
- GPIO SENSE level interrupts wake the CPU without idle GPIOTE channels.
- Closed latching switches are disconnected and sampled for about 10 µs every
  100 ms rather than drawing continuous pull-up current. This saves power at
  the cost of up to 100 ms detection latency when opened.
- Battery voltage is measured hourly on the internal VDD channel. The QSPI
  flash enters deep power-down; UART, I²C, SPI, PWM, USB, logging, and console
  are disabled in normal builds.
- The XIAO regulator and charger circuitry also consume current. Measure the
  actual board sleep current when estimating battery life.

## Known limitations

- The device attestation certificate is a shared Matter SDK development
  certificate, not a unique production identity. A product needs its own VID,
  approved PAI, and unique DAC per device.
- Automatically provisioned devices store the pairing passcode in plaintext
  so it can be shown over USB. Physical access can expose it.
- Optional diagnostics clusters are omitted to save flash.
- OTA/DFU and MCUboot are not enabled.
