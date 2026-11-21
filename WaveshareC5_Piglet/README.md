# Waveshare ESP32-C5 Touch LCD 3.5

Piglet port for the Waveshare ESP32-C5-Touch-LCD-3.5, with a standalone
Seeed XIAO ESP32-C5 mesh node. Piglet source and BSP dependencies are pinned;
see [NOTICE.md](NOTICE.md) for provenance and retained licenses.

## Build

Install PlatformIO, then build the Waveshare firmware and standalone node:

```sh
pio run -e ws-c5-35
pio run -d node
```

For flashing, identify the board's serial port and supply it explicitly using
`--upload-port` with the corresponding project's upload command. On Waveshare,
open Mesh and select Core; the standalone node discovers Core on channel 6.
The local record protocol requires the matching Core from this project.

`wardriver.cfg` is a template for the SD card. Upload credentials and home Wi-Fi
fields are empty, and automatic boot uploads are disabled in the template.
The AP password is the public upstream example; configure it locally. Keep real
credentials on the device, outside source control. No saved device configuration
is changed by copying or building this source tree.

## Tested checkpoint

The verified pair is Waveshare Core with XIAO Node. Records are serialized,
acknowledged after checked CSV commits, retried at most three times, and
suppressed on repeat receipt within the active session. Bench evidence includes
record/ACK loss tests, normal reboot readback, and a 49/49-record comparative
power-cut recovery. See [docs/VERIFICATION.md](docs/VERIFICATION.md) for the
results and scope of each test.

Duplicate state is in memory. Persistent exactly-once recovery across Core
resets is not implemented. One successful power-cut run does not guarantee
all cards or cut timings. Waveshare Node mode and legacy upstream peers do not
implement the standalone node's new record protocol; their interoperability
is not established by this checkpoint. A storage write/commit failure stops
logging and withholds ACKs until recovery/restart.

Missing driver send callbacks trigger a bounded node restart. This recovers
transport but discards queued, unconfirmed records; delivery is not persistent.
Multi-node scheduling, adaptive dwell, and broader UI/BLE qualification remain
outside the verified profile.

## Host checks

```sh
mkdir -p build/tests
clang++ -std=c++11 -Wall -Wextra -Werror tests/checked_csv_storage_test.cpp -o build/tests/checked_csv_storage_test
build/tests/checked_csv_storage_test
clang++ -std=c++11 -Wall -Wextra -Werror tests/record_protocol_test.cpp -o build/tests/record_protocol_test
build/tests/record_protocol_test
clang++ -std=c++11 -Wall -Wextra -Werror tests/scan_record_test.cpp -o build/tests/scan_record_test
build/tests/scan_record_test
```

The Python verifiers in `tests/` accept local, private CSV/receipt/serial
artifacts. Those artifacts are excluded from this repository. Bench-only
compile flags and USB diagnostics are disabled in normal builds.
