# NOTICE — Waveshare C5 Piglet

This project is a hardware port of **Piglet** to the **Waveshare
ESP32-C5-Touch-LCD-3.5**. It is a derivative work and carries its sources'
licenses forward. Attribution, license links, and modification notices below
are part of complying with those licenses.

## Piglet (application + mesh) — CC BY-NC-SA 4.0

- Upstream: https://github.com/Hamspiced/piglet
- Pinned source commit: `4e47f41f00ad2a63660d78f8db20bd33457a7b2c` (main, 2026-10-30)
- Baseline ported: `TDongleC5_Piglet/` (plus `WardriveGoBLE.h`, `WebUI_HTML.h`,
  `YieldSched.h`, `wardriver.cfg`).
- License: **Creative Commons Attribution-NonCommercial-ShareAlike 4.0
  International (CC BY-NC-SA 4.0)** — see [LICENSE](LICENSE), preserved verbatim
  from upstream, and [CREDITS.md](CREDITS.md).
- This derivative port is released under the **same CC BY-NC-SA 4.0** terms. The
  ShareAlike obligation is retained; terms are not relaxed.

### Upstream license discrepancy (documented, not resolved here)

Upstream is internally inconsistent about its license, verified by direct
inspection at commit `4e47f41`:

- `LICENSE` (root): **CC BY-NC-SA 4.0** (`Attribution-NonCommercial-ShareAlike
  4.0 International`).
- `CREDITS.md`: **CC BY-NC-SA 4.0** (links `.../by-nc-sa/4.0/`).
- `README.md` §License (line ~588): **CC BY-NC 4.0** (`Attribution-NonCommercial
  4.0`, links `.../by-nc/4.0/`) — i.e. **no ShareAlike**.

The README's license section is **not** consistent with the LICENSE file. This
port follows the more restrictive, more widely-stated term — **CC BY-NC-SA
4.0** — matching the actual LICENSE file, CREDITS.md, and the upstream
PigletNode/T-Dongle headers. No claim is made that the README is consistent.

### Mesh protocol credit (upstream claim, pending bench verification)

Per `CREDITS.md`, Piglet's ESP-NOW Core/Node mesh is described as
**wire-compatible with the JCMK (JustCallMeKoko) ESP32DualBandWardriver**
Core/Node protocol (https://github.com/justcallmekoko/ESP32DualBandWardriver).
That wire-compatibility is an **upstream claim** and is treated as unverified on
this port until bench-tested against JCMK/compatible hardware. The baseline mesh implementation is ported, with local coordinated-control
and reliable-record extensions described below.

## ESP32-C5-BSP (board hardware support) — MIT

- Upstream: https://github.com/slickrick773/ESP32-C5-BSP
- Pinned revision: `47c32ca` (`v0.5.0`)
- License: **MIT** — `Copyright (c) 2026 slickrick773`.
- Consumed as a pinned library dependency (`https://github.com/slickrick773/ESP32-C5-BSP.git` at the full
  commit pinned in `platformio.ini`). In addition, the following proven build artifacts are
  vendored from the BSP's `hello_board` example and remain under the BSP's MIT
  license (full text in [LICENSES/ESP32-C5-BSP-MIT.txt](LICENSES/ESP32-C5-BSP-MIT.txt)): `boards/ws-c5-35.json`, `variants/ws-c5-35/pins_arduino.h`,
  `partitions_ota_16mb.csv`, `wsc5_user_setup.h`, `apply_c5_tft_patch.py`.

## TFT_eSPI + ESP32-C5 processor port

- `TFT_eSPI` (Bodmer), pulled as a pinned library dependency (`@2.5.43`), under
  its upstream license notices, retained in
  [LICENSES/TFT-eSPI.txt](LICENSES/TFT-eSPI.txt).
- `tft_c5/TFT_eSPI_ESP32_C5.{c,h}`: the **RockBase IoT** ESP32-C5 processor port
  for TFT_eSPI (https://github.com/RockBase-iot/NM-CYD-C5), vendored unmodified
  and installed into TFT_eSPI at build time by `apply_c5_tft_patch.py`. The upstream TFT_eSPI license notices are retained in
  [LICENSES/TFT-eSPI.txt](LICENSES/TFT-eSPI.txt).

## Other pinned library dependencies

- `bblanchon/ArduinoJson@6.21.6` (MIT)
- `mikalhart/TinyGPSPlus@1.1.0` (LGPL-2.1)
- `h2zero/NimBLE-Arduino@2.5.2` (Apache-2.0)
- `lewisxhe/XPowersLib` (MIT, transitive via the BSP)

## Modifications made in this port (T-Dongle C5 → Waveshare ESP32-C5-Touch-LCD-3.5)

Summary of changes from the upstream `TDongleC5_Piglet` baseline. The list describes the hardware adaptation and local extensions; untested
upstream feature parity is not claimed.

- **Display**: Adafruit_ST7735 (0.96" ST7735, 160×80) → **ST7796 320×480 via
  TFT_eSPI** (C5-patched), through a drop-in `DisplayC5` shim
  (`src/board_adapter.h`) so the application's draw calls are unchanged.
- **GPS**: UART (GPIO12/11) → **LC76G over the shared I²C bus** via the BSP
  transport, feeding NMEA into the existing TinyGPSPlus parser; GPS reads are
  deferred while the screen is touched.
- **Navigation input**: single hardware button → **FT6336 capacitive touch**
  using labelled Previous/Next navigation and explicit Core/Node/BLE controls.
  Held touches act once, and page changes wait for finger release.
- **Power/backlight/reset**: no backlight or panel-reset GPIO on this board —
  routed through the **CH32V006 expander** (`WsC5_35::begin()` /
  `Expander::backlight()`), called before display init.
- **SD**: shares the display SPI bus using **TFT_eSPI's live SPI instance**
  (CS GPIO9) so it never tears down the display bus.
- **RGB LED**: the T-Dongle's APA102 has no equivalent on this board;
  `apa102Write()` is a no-op (status shown on the display instead).
- **Pins**: display SCLK6/MOSI7/MISO2/CS8/DC5, SD CS9, I²C SDA27/SCL26.
- **CPU clock**: upstream's 80 MHz under-clock left at the BSP-proven 240 MHz
  default (display push-clock timing).
- **Mesh**: legacy paths remain, with a local coordinated-control extension and
  reliable-record protocol for the standalone XIAO node in `node/`. Records use
  session/sequence IDs, bounded retries, duplicate suppression, and checked CSV
  commits before ACK. Legacy interoperability and reverse-role operation are
  not established by the XIAO-to-Waveshare bench results.
- **Standalone node**: derived from upstream `PigletNode/PigletNode.ino` at the
  same pinned Piglet commit; serialized transmission holds channel 6 until the
  active frame and record acknowledgment complete.
- **Storage**: checked full-row writes, flush, and fsync results gate reliable
  record ACKs. A write/commit error stops logging instead of silently retrying
  an uncertain append. Power-cut results and limits are in `docs/VERIFICATION.md`.
