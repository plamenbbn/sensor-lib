# sensor-lib

`sensor-lib` is a small C library that exposes a generic instrument API for radio and positioning actions. The current implementation focuses on:

- Bluetooth adapter discovery and nearby device scanning
- Wi-Fi adapter discovery and nearby network/device scanning
- GPS polling through `gpsd`

The library is intentionally lightweight. It uses caller-provided output buffers, avoids unnecessary heap allocation in hot paths, and keeps the public interface in plain C.

## Status

This repository currently provides:

- A C implementation of [`interfaces/c/instrument_api.h`](./interfaces/c/instrument_api.h)
- A static library target: `sensor-lib`
- A command-line test tool: `instrument-cli`
- A repeated sensing stress test: `repeat-sensing-test`
- Existing example programs under [`examples/`](./examples/)

The implemented actions are:

- `INSTRUMENT_BLUETOOTH_GET_ADAPTER_INFO`
- `INSTRUMENT_BLUETOOTH_DISCOVER_DEVICES`
- `INSTRUMENT_BLUETOOTH_IS_UP`
- `INSTRUMENT_BLUETOOTH_TURN_ON`
- `INSTRUMENT_BLUETOOTH_TURN_OFF`
- `INSTRUMENT_BLUETOOTH_SCAN_ON`
- `INSTRUMENT_BLUETOOTH_SCAN_OFF`
- `INSTRUMENT_WIFI_GET_MY_DEVICE`
- `INSTRUMENT_WIFI_DISCOVER_DEVICES`
- `INSTRUMENT_WIFI_IS_UP`
- `INSTRUMENT_WIFI_TURN_ON`
- `INSTRUMENT_WIFI_TURN_OFF`
- `INSTRUMENT_WIFI_SCAN_ON`
- `INSTRUMENT_WIFI_SCAN_OFF`
- `INSTRUMENT_GPS_GET_POSITION`
- `INSTRUMENT_BLUETOOTH_DISCOVER_BRATISLAVA_LINKS`
- `INSTRUMENT_WIFI_DISCOVER_BRATISLAVA_LINKS`
- `INSTRUMENT_COMMS_DISCOVER_BRATISLAVA_LINKS`

The implemented callback path now includes:

- `registerCallback(INSTRUMENT_COMMS, LINK_DISCOVERED, ...)`
- `registerCallback(INSTRUMENT_COMMS, LINK_DROPPED, ...)`
- `registerCallback(INSTRUMENT_BLUETOOTH, DEVICE_DISCOVERED, ...)`
- `registerCallback(INSTRUMENT_WIFI, DEVICE_DISCOVERED, ...)`

When the API is created, a background comms runtime starts a Bluetooth L2CAP server and a Wi-Fi UDP server. The
runtime scans for Bluetooth peers and Wi-Fi ARP neighbors, attempts the `packet-comms` style `Hello Pi World` /
`Hello Back from ...` handshake, and publishes a `BratislavaLink` with an established socket fd when the handshake
completes. The dedicated link-callback harness runs indefinitely until interrupted and prints a per-iteration status
summary after each scan-and-connect cycle. Bluetooth scan output also filters out BlueZ metadata lines such as
`RSSI:` and renders missing Bluetooth device names as `<NONE>` for consistent formatting.

## Repository Layout

```text
sensor-lib/
├── CMakeLists.txt              # Root build file for library + CLI test app
├── README.md                   # Project documentation
├── examples/
│   ├── c/
│   │   ├── CMakeLists.txt
│   │   └── bluetoothExample.c
│   └── cc/
│       ├── CMakeLists.txt
│       ├── bluetoothExample.cc
│       ├── demoApp.cc
│       ├── exampleCMakeLists.txt
│       ├── linkExample.cc
│       ├── simulationExample.cc
│       ├── socketExample.cc
│       ├── standaloneTelemetryExample.cc
│       ├── telemetryExample.cc
│       └── wifiExample.cc
├── interfaces/
│   └── c/
│       ├── action_typedef.h
│       ├── bluetooth_typedef.h
│       ├── callback_typedef.h
│       ├── comms_typedef.h
│       ├── gps_typedef.h
│       ├── instrument_api.h
│       ├── logger_typedef.h
│       ├── rssi_typedef.h
│       ├── sensing_typedef.h
│       ├── sensor_api.h
│       ├── simstate_typedef.h
│       └── wifi_typedef.h
├── src/
│   ├── instrument_api.c        # Main library implementation and action dispatch
│   ├── bluetooth_scanner.h     # Bluetooth discovery backend interface
│   ├── bluez_scanner.c         # Default BlueZ-backed Bluetooth discovery
│   ├── hci_scanner.c           # Legacy raw HCI inquiry discovery
│   ├── wifi_scanner_bridge.h   # C-to-C++ bridge for Wi-Fi scanning
│   └── wifi_scanner_bridge.cpp # nl80211/libnl Wi-Fi discovery implementation
└── tests/
    ├── instrument_cli.c        # CLI test application
    └── repeat_sensing_test.c   # Repeated sensing + leak check stress test
```

## Design Notes

### Memory and allocation model

The API is designed around caller-owned buffers:

- The caller allocates input and output arrays.
- The library writes results directly into those buffers.
- Output counts are returned through `output_len`.

This keeps memory ownership simple and makes leak avoidance much easier in C.

Current behavior by subsystem:

- Bluetooth discovery is selected at compile time behind a small scanner interface.
- The default `bluez` backend shells out to BlueZ userspace tools to perform a short discovery run and then enriches discovered devices with additional metadata when BlueZ exposes it.
- The optional `hci` backend uses low-level `hci_inquiry()`. BlueZ internally allocates the inquiry result buffer there, and the library frees it before returning.
- Wi-Fi scanning uses stack buffers and writes directly into the caller output array.
- GPS polling uses a stack receive buffer and returns a single `C_PositionInfo`.

### Bluetooth implementation choice: `bluez` vs `hci`

This project now supports two Bluetooth discovery implementations behind the same `INSTRUMENT_BLUETOOTH_DISCOVER_DEVICES` action.

Pros of `bluez`:

- Richer device discovery, especially for BLE advertisers
- Results are closer to what users see from `bluetoothctl scan on`
- Better alignment with the system Bluetooth daemon and its view of the world

Cons of `bluez`:

- More runtime overhead
- Depends on BlueZ userspace tooling being present and functional
- Some transient BLE devices may still expose only minimal metadata

Pros of `hci` / raw HCI:

- Low overhead
- Simple fit for direct classic Bluetooth inquiry
- Good fallback when a very small low-level build is preferred

Cons of `hci` / raw HCI:

- Narrower discovery coverage, especially for BLE-heavy environments
- One-shot inquiry behavior tends to see fewer devices than BlueZ userspace scanning
- More likely to diverge from what desktop tools report

Why `bluez` is the default now:

- The main goal is richer discovery that looks more like `bluetoothctl`
- It produces materially better results on BLE-heavy hosts
- The old HCI path remains available for compile-time selection

## Dependencies

### Build-time

Required:

- C compiler with C11 support
- C++ compiler with C++17 support
- CMake 3.18 or newer
- BlueZ development headers and library
- `libnl-3` and `libnl-genl-3` development headers and libraries

On Debian/Ubuntu/Raspberry Pi OS, install everything needed to build and exercise the current implementation with:

```bash
sudo apt update
sudo apt install -y build-essential cmake g++ libbluetooth-dev libnl-3-dev libnl-genl-3-dev gpsd gpsd-clients pkg-config
```

Bluetooth discovery backends:

- `bluez` is the default backend and uses BlueZ userspace discovery via `bluetoothctl`, which tends to produce results closer to `bluetoothctl scan on`
- `hci` uses the older low-level `hci_inquiry()` path
- Choose the backend at configure time with `-DSENSOR_LIB_BLUETOOTH_SCANNER=bluez` or `-DSENSOR_LIB_BLUETOOTH_SCANNER=hci`

Headers/libraries used by the implementation:

- `bluetooth/bluetooth.h`
- `bluetooth/hci.h`
- `bluetooth/hci_lib.h`
- `linux/nl80211.h`
- `netlink/netlink.h`
- `netlink/genl/genl.h`
- Standard POSIX networking and ioctl headers

### Runtime

Bluetooth:

- Linux with BlueZ support
- A local Bluetooth adapter
- Sufficient permissions to query and control the adapter

Wi-Fi:

- Linux `nl80211`/cfg80211 support in the driver/kernel path used by the interface
- A local wireless interface
- Sufficient permissions to trigger scans or change interface state

GPS:

- `gpsd` listening on `127.0.0.1:2947`
- A GPS receiver visible to `gpsd`

Notes:

- GPS polling can succeed with `mode=MODE_NOT_SEEN` or `mode=MODE_NO_FIX` when `gpsd` is reachable but no valid fix is available yet.
- If `gpsd` is running but has no active receiver, the API returns a structured “no fix/no device yet” result instead of crashing or leaking resources.

## Build

From the repository root:

```bash
cmake -S . -B build
cmake --build build -j
```

To force a specific Bluetooth discovery backend:

```bash
cmake -S . -B build -DSENSOR_LIB_BLUETOOTH_SCANNER=bluez
cmake -S . -B build-hci -DSENSOR_LIB_BLUETOOTH_SCANNER=hci
```

Artifacts:

- `build/libsensor-lib.a`
- `build/libsensor-lib.so`
- `build/instrument-cli`
- `build/repeat-sensing-test`

## Usage

### CLI test application

The simplest way to exercise the implemented actions is the CLI tool:

```bash
./build/instrument-cli bluetooth
./build/instrument-cli wifi
./build/instrument-cli gps
./build/instrument-cli comms
./build/instrument-cli link-callback
./build/link-callback-test
./build/repeat-sensing-test
./build/repeat-sensing-test 50
```

The link-callback harness supports two Wi-Fi test modes:

```bash
./build/instrument-cli link-callback --wifi-mode client
./build/instrument-cli link-callback --wifi-mode hotspot
./build/instrument-cli link-callback --wifi-mode wap
```

Mode behavior:

- `client` leaves Wi-Fi in normal station/client mode and runs the discovery + handshake loop there.
- `hotspot`/`wap` asks NetworkManager to create a temporary access point named `sensor-lib-link-callback`.
- On exit, hotspot mode attempts to tear the temporary hotspot down and restore the previous active Wi-Fi client connection. If no previous connection is known, it falls back to reconnecting the wireless interface generically.

Hotspot/client transition notes:

- Every `nmcli` step in the harness is bounded to `15 seconds`.
- Timed operations include enabling Wi-Fi, starting hotspot mode, stopping hotspot mode, deleting the temporary hotspot profile, and restoring normal Wi-Fi afterward.
- If one of those steps times out, the harness logs the specific step that hung and continues running instead of failing fast.
- Depending on driver and NetworkManager behavior, hotspot mode may fail to come up cleanly on some hosts. In that case, the harness may still be reporting scans from the normal client connection rather than a true AP-mode vantage point.

### Distributed two-host runners

For repeated two-host testing from the repo root, use the distributed runners:

```bash
./distributed-link-test.sh
./distributed-link-test.sh brat-44
./distributed-link-test.sh --duration 50 --startup-delay 10 --remote-reconnect 150 brat-44

./distributed-bluetooth-link-test.sh
./distributed-bluetooth-link-test.sh brat-44
./distributed-bluetooth-link-test.sh --remote-dir /home/plamen/sensor-lib-bt-run brat-44
```

Runner behavior:

- `distributed-link-test.sh` builds, copies artifacts to the peer, then runs the Wi-Fi hotspot/client harness in both directions.
- `distributed-bluetooth-link-test.sh` builds, copies artifacts to the peer, prepares both hosts for Bluetooth auto-accept, forces the comms runtime into Bluetooth-first / Wi-Fi-skipped mode, then runs the BT-only harness on both hosts at once.
- Both runners save per-side logs under `build/distributed-link-logs/`.

Bluetooth runner notes:

- The Bluetooth path now supports full distributed `LINK_DISCOVERED` callback + `BratislavaSocket` message exchange once peer visibility succeeds.
- For the dedicated two-host runner, each side now exports `SENSOR_LIB_BLUETOOTH_EXPECT_PEERS` with the other host's Bluetooth MAC and hostname. That makes the remote `sensor-lib` interface show up as a deterministic discovery candidate even when BlueZ passive discovery is flaky on a cold start.
- The BT runner performs a small priming phase before the harness starts: local + remote classic inquiry (`hcitool inq`), a remote `l2ping` to the local adapter MAC when passwordless `sudo` is available, and a one-shot `instrument-cli bluetooth` warm-up on both hosts. This helps seed both BlueZ and the library's own scanner path on hosts where pure passive discovery is flaky.
- You can skip that warm-up with `./distributed-bluetooth-link-test.sh --no-bt-prime ...` if you want to test a completely cold discovery path.

Example output:

```text
Bluetooth adapters: 1
  [0] id=0 mac=2C:CF:67:56:88:BF name=hci0 flags=0x0000001D type=3
Bluetooth discoveries: 15
  [0] mac=E4:65:B8:C8:B1:72 name=IPSTube_C8B172 class=0/0/0 major=63
  [1] mac=6A:8C:78:F3:9E:7B name=6A-8C-78-F3-9E-7B class=0/0/0 major=63
  ...
  [14] mac=9C:83:06:F6:09:A5 name=Plamen's S26 Ultra class=12/2/90 major=2
```

```text
Wi-Fi adapters: 1
  [0] mac=2C:CF:67:56:88:BE ssid=Apolo type=1
Wi-Fi discoveries: 1
  [0] mac=74:AC:B9:5F:73:11 ssid=Apolo rssi_valid=1 rssi_dbm=-53
```

```text
GPS count: 1
GPS mode: 3
GPS timestamp: 1783721659
GPS lat/lon/alt: 38.7566712850 -77.2266953040 46.412
```

Example Bluetooth formatting when a device has no reported label:

```text
Bluetooth discoveries: 3
  [0] mac=AA:BB:CC:DD:EE:FF name=<NONE> class=0/0/0 major=63
```

The repeated sensing test performs Bluetooth, Wi-Fi, and GPS sensing back-to-back for `N` iterations, defaulting to `20`, with a fixed 2-second delay between iterations:

```bash
./build/repeat-sensing-test
./build/repeat-sensing-test 50
```

Behavior notes:

- The test creates one `InstrumentAPI` handle for the full run and reuses it across all iterations.
- `INSTRUMENT_API_NOT_SUPPORTED` is treated as a valid outcome for hosts that do not currently have Bluetooth, Wi-Fi, or GPS support available.
- At the end of the run, the test prints every unique Bluetooth and Wi-Fi discovery it saw, including the first local timestamp when that device was observed.
- On glibc-based Linux systems, the test also captures allocator usage after a warmup iteration and again at the end, then fails if retained heap usage keeps growing beyond a small allowance.

### Using the library from C

Basic flow:

1. Create an `InstrumentAPI` handle
2. Allocate output buffers sized to the relevant max constants
3. Call `instrumentAction(...)`
4. Inspect `output_len`
5. Destroy the API handle

Example:

```c
#include "instrument_api.h"
#include "bluetooth_typedef.h"

int main(void) {
    InstrumentAPIConfig config = {0};
    InstrumentAPI *api = createInstrumentAPI(config);
    if (api == NULL) {
        return 1;
    }

    BluetoothAdapterInfoBase adapters[BLUETOOTH_MAX_ADAPTERS];
    uint32_t adapter_count = 0;

    instrument_api_status_t status = api->instrumentAction(
        INSTRUMENT_BLUETOOTH_GET_ADAPTER_INFO,
        NULL,
        0,
        adapters,
        &adapter_count);

    destroyInstrumentAPI(api);
    return (status == INSTRUMENT_API_SUCCESS) ? 0 : 1;
}
```

## Test Cases

Current manual test coverage centers on the CLI app and direct runtime validation.

### Bluetooth tests

- Adapter enumeration returns at least one local adapter when hardware is present.
- BlueZ backend returns a richer set of nearby devices visible during the discovery window.
- HCI backend still builds and returns classic inquiry results when explicitly selected.
- Adapter up-state query returns a boolean.
- Adapter power on/off paths invoke the expected HCI device controls.

Suggested manual commands:

```bash
./build/instrument-cli bluetooth
```

### Wi-Fi tests

- Wireless adapter enumeration returns available local Wi-Fi interfaces.
- Scan results return available BSS/device entries visible at scan time.
- Interface up-state query returns a boolean.
- Interface power on/off uses standard network interface flags.

Suggested manual commands:

```bash
./build/instrument-cli wifi
```

### GPS tests

- `gpsd` reachable but no receiver: returns one record with `MODE_NOT_SEEN`
- Receiver present but no fix yet: returns one record with `MODE_NO_FIX`
- Receiver with live fix: returns one record with `MODE_2D` or `MODE_3D` and valid coordinates

Suggested manual commands:

```bash
./build/instrument-cli gps
```

Useful direct daemon inspection:

```bash
python3 - <<'PY'
import socket
s = socket.socket()
s.connect(('127.0.0.1', 2947))
print(s.recv(4096).decode())
s.sendall(b'?WATCH={"enable":true,"json":true};\n?POLL;\n')
print(s.recv(4096).decode())
PY
```

## Troubleshooting

### Bluetooth discovery fails

Possible causes:

- No Bluetooth adapter present
- Adapter is down
- Insufficient privileges
- BlueZ stack or kernel support is unavailable
- BlueZ backend tooling is unavailable or discovery is blocked by the host stack state

### Wi-Fi discovery fails

Possible causes:

- No wireless interface present
- Driver does not support the `nl80211` scan path used here
- Scan trigger requires privileges
- Interface is down

### GPS returns zero coordinates or `MODE_NOT_SEEN`

Possible causes:

- `gpsd` is running but has no active GPS device
- Receiver is attached but no fix has been acquired yet
- `gpsd` is not bound to `127.0.0.1:2947`

Check:

```bash
ss -ltnp | grep 2947
systemctl status gpsd
ls -l /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
```

## Limitations

- Wi-Fi scanning uses `nl80211` via `libnl`.
- Bluetooth discovery supports either a BlueZ-backed scan path or a classic HCI inquiry path. The default BlueZ backend usually finds more BLE devices, but some transient BLE entries still expose only partial metadata.
- GPS parsing currently targets `gpsd` JSON TPV/POLL messages and does not use `libgps`.
- Callback registration is not implemented.

## Future Work

- Add automated unit/integration tests
- Replace the current BlueZ userspace tool integration with a direct BlueZ IPC/client implementation
- Add configurable GPS host/port and longer polling/fix timeout behavior
- Implement callback registration and continuous scan support
