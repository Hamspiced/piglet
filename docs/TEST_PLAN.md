# Piglet Test Plan

The Piglet codebase has no automated tests today. Every change is validated by
flashing one of four sketches to a real board and walking through the UI. This
document lays out a two-track path for getting that under control:

1. **Host-side unit tests** for the bits of logic that have no business
   reaching for `Serial`, `SD`, or `WiFi`.
2. **`arduino-cli` compile-only CI** so the four sketches at least keep
   building on every PR.

The two tracks are independent. You can land them in either order, but the
unit-test work has a much higher ratio of bugs-caught-per-hour-invested, so
start there.

---

## Track 1 — Host-side unit tests

### Approach

Compile a `lib/piglet-core/` static library with `g++` (or `clang++`) on the
host and link it into a small `doctest`-based test binary. We pick **doctest**
over Unity because:

- doctest is a single header, no extra dep.
- Test cases are written as `TEST_CASE("...")` blocks in normal `.cpp` files.
  No `RUN_TEST(...)` boilerplate, no fixture macros — the senior-Elixir-engineer
  ergonomic that's closest to ExUnit's `test "..." do ... end`.
- `SUBCASE` blocks give you setup-reuse without the BDD/`describe` overhead.
- PlatformIO has first-class doctest support if you ever want `pio test`
  to run the suite on-device too.

(Unity is the right call only if you plan to run the same suite on-target in
addition to host. We don't, day one — see "on-device integration tests" below.)

### What to lift out of Arduino TUs first

Most of these functions are already pure: they take `String` / primitive args
and return a value. They're entangled with Arduino only because they live in
`.cpp` files that also include `<WiFi.h>`, `<SD.h>`, etc. The fix is mechanical:
move them to `lib/piglet-core/` and replace `String` with `std::string`
(or keep `String` and ship a 50-line `ArduinoString.h` shim on host — see
[ArduinoString shim](#arduinostring-shim) below).

Priority order (top = easiest + highest value):

| Function | Source | Why it's worth lifting |
|---|---|---|
| `parseKeyValueLine` | `Config.cpp:59` | Pure string parsing. No deps. Misparsing config silently breaks the device — high blast radius, no current coverage. |
| `cfgAssignKV` | `Config.cpp:87` | Mutates `cfg` based on key/value. Add a fake `cfg` struct in the test and assert each branch validates input (e.g. `gpsBaud` rejects negative, `scanMode` rejects unknown). |
| `normalizeSdPath` | `SDUtils.cpp:12` | Path joining + dedup-of-leading-slash. Pure. Used everywhere in upload/file listing — a bug here mis-routes files. |
| `pathBasename` | `SDUtils.cpp:6` | Pure string slicing. Used in upload logs and delete operations. |
| `sanitiseDeviceName` | `SDUtils.cpp:132` | Whitelist filter. Output drives filenames written to SD — exactly the kind of thing where you want unit tests for the boundary chars. |
| Channel→freq math | `appendWigleRow`, `SDUtils.cpp:240-242` | Extract as a free function `uint32_t channelToFreqMHz(int)`. Three branches, easy to cover; WiGLE 1.6 rejects rows with wrong freq, so a regression here breaks uploads silently. |
| `csvHasDataRows` | `WigleUpload.cpp:596` | Reads a file, but the *logic* (header detection, empty-row skip) can be tested by feeding a `std::istream` instead of a `File`. Worth the small refactor — empty-CSV detection drives the auto-delete branch. |
| `authModeToString` | `Scanner.cpp:7` | After Fix 3 this is the single source of truth. Trivial table-driven test pins the WiGLE auth-string vocabulary so a future ESP-IDF upgrade can't silently change it. |
| JCMK packet builders | `MeshNode.cpp:148-230` (`jcmkSendCoreRequest`, `jcmkSendHeartbeat`, `jcmkSendText`, role/config builders) | These build a `jcmk_text_msg_t` byte-for-byte. Refactor so the build step returns a `std::array<uint8_t, 212>` and the `esp_now_send` call is a thin wrapper. Then unit-test the byte layout: magic prefix, type byte, counter, len, payload, **and that the buffer is always exactly 212 bytes** (the Biscuit Pro compat rule called out in the comment at MeshNode.cpp:144). |
| HTTP status-line parsing | `WigleUpload.cpp` (currently inline in `uploadFileToWigle` / `uploadFileToWdgwars`) | Extract the "read first line, find HTTP code" loops into `int parseHttpStatus(const std::string& line)`. Two-line function, but the upload-success/fail decision turns on it. |

Don't try to lift `uploadFileToWigle` itself — it's `WiFiClientSecure` all the
way down. Lift the *pieces* it leans on.

### First PR shape

```
piglet/
├── Arduino Files/Piglet/       (unchanged — sketch still builds)
├── lib/
│   └── piglet-core/
│       ├── include/piglet/
│       │   ├── config_parse.h
│       │   ├── sd_path.h
│       │   ├── wigle_csv.h
│       │   └── jcmk_packet.h
│       └── src/
│           ├── config_parse.cpp
│           ├── sd_path.cpp
│           ├── wigle_csv.cpp
│           └── jcmk_packet.cpp
├── test/
│   ├── doctest.h               (single-header drop-in)
│   ├── main.cpp                (DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
│   ├── test_config_parse.cpp
│   ├── test_sd_path.cpp
│   ├── test_wigle_csv.cpp
│   └── test_jcmk_packet.cpp
├── Makefile                    (or test/Makefile)
└── docs/TEST_PLAN.md
```

`make test`:

```make
CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Ilib/piglet-core/include -Itest
SRC      := $(wildcard lib/piglet-core/src/*.cpp test/*.cpp)
test/runner: $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^
test: test/runner
	./test/runner
```

Smoke-test file (`test/test_config_parse.cpp`):

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "piglet/config_parse.h"

using piglet::parseKeyValueLine;

TEST_CASE("parseKeyValueLine") {
    std::string k, v;

    SUBCASE("normal key=value") {
        CHECK(parseKeyValueLine("scanMode=aggressive", k, v));
        CHECK(k == "scanMode");
        CHECK(v == "aggressive");
    }

    SUBCASE("strips surrounding whitespace") {
        CHECK(parseKeyValueLine("  scanMode = aggressive  ", k, v));
        CHECK(k == "scanMode");
        CHECK(v == "aggressive");
    }

    SUBCASE("handles double-quoted value") {
        CHECK(parseKeyValueLine("homeSsid=\"my home\"", k, v));
        CHECK(v == "my home");
    }

    SUBCASE("rejects comment lines") {
        CHECK_FALSE(parseKeyValueLine("# scanMode=aggressive", k, v));
        CHECK_FALSE(parseKeyValueLine("// scanMode=aggressive", k, v));
    }

    SUBCASE("rejects blank and malformed lines") {
        CHECK_FALSE(parseKeyValueLine("", k, v));
        CHECK_FALSE(parseKeyValueLine("noEqualsSign", k, v));
        CHECK_FALSE(parseKeyValueLine("=valueWithNoKey", k, v));
    }
}
```

Invocation:

```sh
make test                # runs all
./test/runner -tc='*csv*' # filter by test-case glob
./test/runner --reporters=junit > junit.xml   # CI-friendly output
```

### ArduinoString shim

Two ways to handle `String`:

1. **Recommended** — rewrite the lifted functions to use `std::string`.
   The Arduino sketch then converts at the boundary
   (`String(stdString.c_str())`). Cleaner host code, no surprises.
2. **Fallback** — ship `test/ArduinoString.h` that `#define`s `String` to
   `std::string`. Faster to land, but you'll trip over `String::reserve()`
   (which exists on both, fine) vs `String::toFloat()` (Arduino-only).

Start with (1) for newly extracted code. The String API is small enough that
greenfield code should just use `std::string` — leave `String` to the Arduino
boundary layer (sketch + libraries that take `WiFiClient` etc).

### Edge cases worth covering on day one

These are bugs or behaviors that aren't covered by any existing test and are
all reproducible from a unit test once the relevant function is lifted:

- **`ch == 0` 2.4GHz misclassification** — `Scanner.cpp:36-43` treats
  `ch == 0` as `is2g`. That's a workaround for the ESP-IDF returning 0 for
  some encrypted/hidden networks, but it means a row written to the CSV gets
  `channel=0` and (with the existing `channelToFreqMHz` logic) `freq=0`, which
  WiGLE may reject. Pin the current behavior in a test, then decide if we
  want to either default to a sentinel channel or drop the row entirely.
- **`wardriver.cfg` malformed-line handling** — `Config.cpp:152` calls
  `parseKeyValueLine` and silently ignores `false` returns. Cover:
  - lines with `\r\n` (CRLF) line endings
  - lines with only whitespace
  - lines with `=` but no value (`scanMode=`)
  - lines with embedded `=` in the value (`homePsk=foo=bar`)
  - duplicate keys (last-write-wins is the current behavior — pin it)
- **CSV row with embedded quotes/commas** — `appendWigleRow`
  (`SDUtils.cpp:223`) escapes `"` → `""` in the SSID field but doesn't
  quote-wrap any other field. Cover SSIDs with `"`, `,`, embedded newline,
  and emoji. Bonus: roundtrip the row through a CSV parser and assert it
  parses back identical.
- **JCMK 212-byte minimum padding rule** — MeshNode.cpp:144 comments that
  Biscuit Pro drops packets `< 212 bytes`. Every `jcmkSend*` builder zeroes
  a `jcmk_text_msg_t` (= 212 bytes) and sends `sizeof(msg)`. The test should
  assert that for every builder, the returned buffer is exactly 212 bytes,
  even when the payload is 0 (heartbeat) or much shorter than `JCMK_TEXT_MAX`.
- **WiGLE freq calculation boundary** — `SDUtils.cpp:240-242`:
  - `channel == 13` → 2472 MHz ✓
  - `channel == 14` → 2484 MHz ✓ (special case)
  - `channel == 15` → 0 MHz (gap — pin or fix)
  - `channel == 31` → 0 MHz (gap before 5GHz block)
  - `channel == 32` → 5160 MHz ✓
  - `channel == 165` → 5825 MHz ✓
- **`normalizeSdPath`** — bare filename vs leading-slash vs trailing-slash
  on `dir`; empty filename; filename containing `..` (does it strip? does it
  pass through?).
- **`sanitiseDeviceName`** — leading/trailing dots, slashes, NUL bytes,
  empty string, all-whitespace, >maximum length.
- **HTTP status-line parsing** — `HTTP/1.1 200 OK`,
  `HTTP/1.0 200 OK`, `HTTP/1.1 401 Unauthorized`, garbage prefix,
  truncated line, `HTTP/1.1 200` (no reason phrase).

---

## Track 2 — `arduino-cli` compile-only CI

Doesn't catch logic bugs; does catch the "I broke the build for one of the
four sketches and didn't notice because I only flash the main one" failure
mode. This is the most common breakage in this repo.

### Sketches and target FQBNs

| Sketch | Target board | Suggested FQBN |
|---|---|---|
| `Arduino Files/Piglet/Piglet.ino` | LilyGo T-Display S3 / ESP32-S3 / ESP32-C6 (depends on `cfg.board`) | `esp32:esp32:esp32s3:PartitionScheme=huge_app,USBMode=hwcdc,CDCOnBoot=cdc` (S3 baseline; add a second matrix entry for `esp32:esp32:esp32c6` if C6 builds matter) |
| `PigletNode/PigletNode.ino` | ESP32-C3 or whatever the Node hardware actually is | confirm against your build instructions and add |
| `TDongleC5_Piglet/TDongleC5_Piglet.ino` | LilyGo T-Dongle ESP32-C5 | `esp32:esp32:esp32c5` (verify; C5 support may need a specific core version) |
| `waveshareDisplayMiniPiglet/waveshareDisplayMiniPiglet.ino` | Waveshare ESP32-S3-Mini variant | `esp32:esp32:esp32s3:...` matching the Waveshare config |

### Workflow skeleton

`.github/workflows/build.yml`:

```yaml
name: build
on:
  push:
    branches: [main]
  pull_request:

jobs:
  build:
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        sketch:
          - { path: "Arduino Files/Piglet",            fqbn: "esp32:esp32:esp32s3" }
          - { path: "PigletNode",                      fqbn: "esp32:esp32:esp32c3" }
          - { path: "TDongleC5_Piglet",                fqbn: "esp32:esp32:esp32c5" }
          - { path: "waveshareDisplayMiniPiglet",      fqbn: "esp32:esp32:esp32s3" }
    steps:
      - uses: actions/checkout@v4
      - uses: arduino/setup-arduino-cli@v2
      - run: |
          arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
          arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
          arduino-cli lib install "ArduinoJson" "TinyGPSPlus" "U8g2"   # confirm full list
      - run: arduino-cli compile --fqbn "${{ matrix.fqbn }}" "${{ matrix.sketch.path }}"
```

The `--additional-urls`, `lib install` list, and exact FQBN board options
(partition scheme, PSRAM, USB CDC, etc.) need to match whatever the user is
flashing today — pull those from the IDE settings on first run, then pin them
in the workflow.

### Caching

Add `actions/cache@v4` on `~/.arduino15` to avoid re-installing the ESP32
core (~500 MB) every run. Cuts CI from ~6 min to ~90 s.

---

## Unit-test test cases — concrete list for the first PR

```cpp
// test_config_parse.cpp
TEST_CASE("parseKeyValueLine: normal, whitespace, quotes, comments, malformed")
TEST_CASE("parseKeyValueLine: embedded '=' in value preserved")
TEST_CASE("parseKeyValueLine: CRLF line endings")
TEST_CASE("cfgAssignKV: scanMode rejects unknown values")
TEST_CASE("cfgAssignKV: gpsBaud rejects 0 and negatives")
TEST_CASE("cfgAssignKV: battPin distinguishes '0' from 'disabled'")
TEST_CASE("cfgAssignKV: maxBootUploads accepts -1/0/positive, rejects -2")
TEST_CASE("cfgAssignKV: meshModeOnBoot whitelists core/node/none")

// test_sd_path.cpp
TEST_CASE("pathBasename: typical, no-slash, trailing-slash, empty")
TEST_CASE("normalizeSdPath: dir+name combinations, leading slashes")
TEST_CASE("sanitiseDeviceName: whitelist, length cap, empty, all-bad-chars")

// test_wigle_csv.cpp
TEST_CASE("channelToFreqMHz: 2.4GHz channels 1-13")
TEST_CASE("channelToFreqMHz: channel 14 = 2484 MHz")
TEST_CASE("channelToFreqMHz: 5GHz channels 32, 36, 165")
TEST_CASE("channelToFreqMHz: channel 0 and 15-31 (gap behavior — pin or fix)")
TEST_CASE("appendWigleRow: SSID with embedded quote → doubled")
TEST_CASE("appendWigleRow: SSID with comma round-trips through CSV parser")
TEST_CASE("csvHasDataRows: header only returns false")
TEST_CASE("csvHasDataRows: header + one data row returns true")
TEST_CASE("csvHasDataRows: empty file returns false")
TEST_CASE("authModeToString: every WIFI_AUTH_* enum value")

// test_jcmk_packet.cpp
TEST_CASE("jcmkBuildCoreRequest: returns exactly 212 bytes")
TEST_CASE("jcmkBuildHeartbeat: magic prefix correct, type=HEARTBEAT, counter increments")
TEST_CASE("jcmkBuildText: payload truncated to JCMK_TEXT_MAX (200)")
TEST_CASE("jcmkBuildText: null-terminator preserved at JCMK_TEXT_MAX position")
TEST_CASE("every builder: buffer is exactly sizeof(jcmk_text_msg_t)")
```

That's ~30 test cases, all small. Realistic to land the lib extraction + this
suite in one PR.

---

## What is NOT testable host-side

Some things will never run under `g++` on Linux. Don't waste time mocking them:

- **SD card I/O** — `File`, `SD.open`, `SD.remove`, `openNextFile`. These
  cover real filesystem behavior (FAT32 quirks, card wear, write-failure
  recovery). Mocking them gives you tests that pass while the device fails.
- **TLS handshakes** — `WiFiClientSecure`, server cert handling, the
  WiGLE/WDGoWars API contract. The actual upload functions can't be
  meaningfully unit-tested; they need an integration test against the real
  endpoint (or a local mitmproxy-style replay).
- **ESP-Now timing** — the mesh code in `MeshNode.cpp` has ordering
  requirements that depend on `esp_wifi_set_channel`, peer-add timing, and
  the IDF's internal queue. Host tests can verify the *bytes on the wire*
  (Track 1 above) but not whether they're sent at the right moment.
- **Display rendering** — `Display.cpp` is U8G2 against real OLED hardware.
- **GPS parsing** — `TinyGPS++` against real NMEA streams from the GPS
  module. (The library itself has tests upstream; we'd be testing our
  integration, which is more easily verified by looking at the OLED.)

### How to cover those — on-device integration tests

Once Track 1 is established, the next step is a `pio test` suite that runs
on real hardware:

- **SD I/O smoke test** — boots a "test mode" sketch, writes a known CSV,
  reads it back, asserts the bytes. Run before each release on a known card.
- **Upload integration** — point WiGLE config at a staging endpoint
  (or use the real one with a throwaway token), trigger an upload, assert
  the device transitions through the expected `uploadLastResult` states.
- **Mesh handshake** — two devices on the bench, one Core + one Node,
  assert the Core sees the Node within N seconds and that role/config
  messages are exchanged. This is the highest-value on-device test because
  the mesh code has the most subtle timing bugs.

These are manual-to-semi-automated for now. The point of Track 1 is to make
the host-side suite cheap enough that you only need to reach for the bench
when the bug genuinely depends on real radio behavior.
