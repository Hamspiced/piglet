# Changelog

## v2.63 (2026-10-04)

### New Features
- **USB serial file sync** — a host can list and download the SD-card CSV logs over the USB serial port, so a solo drive can be imported by plugging Piglet into a computer or a companion device (e.g. [Ragnar](https://github.com/PierreGode/Ragnar)) without a card reader or Wi-Fi. Commands: `@PIGLET HELLO`, `@PIGLET LIST`, `@PIGLET GET <path> [offset]`. See README "USB Serial File Sync".
  - Read-only, limited to `/logs/*.csv` and `/uploaded/*.csv`.
  - Each 144-byte chunk is sent as base64 with its own CRC32, and transfers resume at an offset after a damaged chunk or a stall.
  - `LIST` reports each file's size, whether it is the file being written right now, and its last-write time (real once the clock is set from GPS), so hosts can fetch the newest drive first.
  - An SD read failure is reported as `@PG ERR read-error <offset>` instead of ending the transfer early.
  - `HELLO` reports firmware, chip, Wi-Fi MAC, the last reset reason and uptime, and whether the SD card is ready.

### Technical Changes
- New `SerialSync.cpp/.h`, polled at the top of `loop()` (normal, Core and mesh-node modes). It does nothing unless a command arrives.
- Data lines are sized to fit the 256-byte HW CDC TX buffer on the C3/C5/C6, and ESP-IDF logging is muted while a file is served, so Wi-Fi driver log lines can't land inside a data line.
- The device id is the Wi-Fi STA MAC (`esp_read_mac`), because `ESP.getEfuseMac()` returns a 64-bit EUI on the C5/C6.

---

## v2.62 (2026-09-28)

### Upload Speed
- **Faster SD card clock** — boot now negotiates the fastest working SD-over-SPI clock from a descending ladder (up to 20 MHz) instead of a fixed 8 MHz/4 MHz fallback, and logs the speed it settles on. Since every upload streams its CSV straight off the SD card, this directly cuts the SD-read portion of upload time for large files, in addition to speeding up scan-result writes during a drive. Also fixes a T-Dongle C5-specific bug where its clock probe iterated slowest-first and returned on first success — meaning it was effectively always running the SD card at 100 kHz instead of anywhere near its real capability. A new `sdMaxSpiHz` config option (default 20000000) lets you cap it lower if your wiring shows SD write errors at the higher speed.
- **Lower-overhead SD logging** — `appendWigleRow()` (the per-network CSV writer) now builds each row in a fixed stack buffer via `snprintf` instead of chaining ~10 Arduino `String` concatenations per row, cutting heap churn during dense scans so uploads that follow aren't competing with a fragmented heap.

### Bug Fixes
- **Cascading "File does not exist" errors during large batch uploads** — after enough sustained SD read/write activity in a single session (e.g. uploading dozens of large CSVs back-to-back), the SD/SPI bus could wedge into a state where every subsequent `SD.exists()`/`SD.open()` call failed, even though the files were still fully intact on the card. Previously this looked like every remaining file in the batch had vanished (`File does not exist`, `could not open ... assuming it HAS data`), when in fact only the SD bus itself needed a reset. Upload and file-move code now detects this condition and attempts a full SD re-mount before giving up on a file — if the remount succeeds, the file is found and processed normally instead of being skipped for the rest of the session. Recovery attempts are rate-limited (~4s) so a genuinely dead card doesn't get hammered on every file.
- **Mesh Node scan results not reliably reaching the Core** — a Node's `HEARTBEAT` and scan-result `TEXT` messages were broadcast over ESP-Now even after the Core was already known, instead of being sent directly (unicast) to it. Broadcast 802.11/ESP-Now frames have no link-layer acknowledgment or retry, so a dropped broadcast is silently lost with no way for either side to detect it — the Node's local "Sent" counter still increments because it only reflects that `esp_now_send()` was *called*, not that the frame was actually delivered. This could make a Node look completely healthy (Core linked, Found/Sent counts climbing) while none of its results ever reached the Core. Node-to-Core `HEARTBEAT`/`TEXT` messages now unicast directly to the already-paired Core's MAC, which gains real link-layer retry/ACK.
- **Mesh Node never detected a silent Core** — unlike the Biscuit protocol's own timeout handling, a Piglet acting as a mesh Node had no way to notice its Core had gone silent (rebooted, moved out of range, etc.) and would keep scanning/sending into the void indefinitely. Node mode now tracks time since the Core was last heard from and returns to actively searching for a Core after 30 seconds of silence.

### New Features
- **Deterministic transmit-slot scheduling for Piglet-to-Piglet mesh nodes** — when a Piglet acting as Core coordinates other Piglets acting as Nodes, each Node now gets an exclusive, non-overlapping time slot in which to send its found networks, instead of every node returning to the shared ESP-NOW channel opportunistically and risking collisions with other active nodes. A short marker exchanged during the existing discovery handshake lets Core and Node positively confirm they're both genuine Piglets before this activates — real JCMK hardware and Biscuit nodes/cores are completely unaffected and keep working exactly as before. Scanning itself is untouched by this: each node keeps scanning its assigned channels at full pace regardless of slot timing; only sending already-found results is deferred to the node's own slot.

### Improvements
- **Cooperative yielding during large scan batches** — added `yield()` calls at the existing periodic flush points (now every 16 lines instead of 25) and inside the scan-result loop, so a large batch of results doesn't monopolize the CPU without giving WiFi, GPS parsing, and the web server a chance to run.

### Diagnostics
- Added an ESP-Now send-status callback that tracks genuine radio-level delivery failures (as opposed to just "attempted") in a new counter, shown as `Fail:` next to `Sent:` on the mesh Node's on-device page. A climbing `Fail` count with a healthy `Sent` count now makes actual transmission failures visible in the field instead of looking identical to success.

### Configuration
- New config option: `sdMaxSpiHz` — SD-over-SPI clock ceiling in Hz (default 20000000). Lower it if you see SD write errors/corruption on marginal wiring. Requires reboot.

---

## v2.59 (2026-09-14)

### Bug Fixes
- **WDGoWars upload delay** — Uploading to WDGoWars queues the file for async server-side processing (HTTP 202 + job ID), but the firmware was blocking for up to 45 seconds per file (15 poll attempts, 3 s apart) waiting for that job to finish before advancing progress, moving the file to `/uploaded`, or refreshing the display. Since WiGLE uploads run right after WDGoWars for the same file, they appeared delayed too, and the OLED/TFT/web UI progress looked frozen the whole time since nothing refreshed it during the poll. The upload now reports success as soon as the file is accepted (202 + job ID) and checks the job result in the background instead of blocking; progress, file moves, and the display all update immediately. A best-effort check is still run before intentionally dropping the WiFi connection (`autoStartAfterUpload`, entering mesh Core mode) so quick jobs are still confirmed when possible.

### Technical Changes
- WDGoWars job polling moved out of `uploadFileToWdgwars()` into a background queue serviced from `loop()` (`wdgwarsServicePendingJobs()`), with a bounded best-effort drain (`wdgwarsDrainPendingJobs()`) used right before STA teardown. Applied to both the main Piglet firmware and the T-Dongle C5 standalone firmware.

---

## v2.58 (2026-07-23)

### Bug Fixes
- **GPS stale/incorrect coordinates** — Fixed a bug where the last-known position cache only updated when scan results were processed *and* networks were found. Driving through an area with no networks froze the cached position; when the fix was later lost those stale (potentially distant) coordinates were written to CSV. The cache now updates every loop iteration, decoupled from scan processing.
- **GPS bad-fix cache poisoning** — Low-quality re-acquisitions (e.g. brief fixes emerging from a tunnel or under heavy tree cover) no longer overwrite a good cached position. A quality gate now requires HDOP ≤†10 and ≥ 3 satellites before accepting a location into the cache.
- **GPS cache expiry** — Cached positions older than 3 minutes are discarded rather than used indefinitely. Networks logged after a 3-minute GPS outage correctly appear at 0,0 (null island) instead of an arbitrarily stale location.

### New Features
- **XIAO ESP32-C3 board support** — The main Piglet firmware now supports the Seeed XIAO ESP32-C3. Set `board=c3` in `/wardriver.cfg` or select **XIAO C3** in the Web UI Board dropdown; also auto-detected from the chip model string at boot. 2.4 GHz only; optional I2C OLED on D4/D5; no dedicated button (GPIO9 conflicts with SPI MISO — wire one externally to any free GPIO if needed).

### Improvements
- **GPS boot wiring check** — After `GPSSerial.begin()`, firmware waits 2 s and reports whether any data arrived: `chars=0` means RX is not connected to GPS TX; checksum errors indicate a baud-rate mismatch. Applied to both main and T-Dongle C5 firmware.
- **GPS 10-second health log** — While waiting for a GPS fix, a diagnostic line prints every 10 s showing chars processed, checksum pass/fail counts, and satellite count — immediately distinguishes no-data (wiring) from data-but-no-fix (sky view) situations.
- **GPS RX buffer increased to 512 bytes** — Prevents UART overflow during WiFi scan blocking windows at 9600 baud.

### T-Dongle C5
- All GPS fixes above applied to the T-Dongle C5 standalone firmware.

---

## v2.57 (2026-06-24)

### New Features
- **Auto-Start Wardriving After Uploads** (`autoStartAfterUpload`): new config option that disconnects from home Wi-Fi immediately after boot uploads complete and begins scanning without delay. Previously the device held the STA connection open, which paused scanning until the link dropped naturally. Configurable via web UI or `wardriver.cfg`. Disabled by default.

  > **Note:** Once enabled, the web UI is not reachable on the home network after boot (device disconnects immediately after uploading). To disable it, either power on away from the home network so the Wardriver AP broadcasts — connect to it and visit `http://192.168.4.1` — or remove the SD card and set `autoStartAfterUpload=false` in `wardriver.cfg` directly.

### Bug Fixes
- **Mesh node mode on S3 / C6**: nodes no longer attempt to scan 5 GHz channels (36–177) on 2.4 GHz-only hardware. Previously those scan attempts failed silently and wasted ~80 ms each per cycle; the node now skips channels > 14 when not running on a C5.

### T-Dongle C5
- Synced mesh WiFi init fix: `enterCoreMode()` and `enterNodeMode()` now use `WiFi.mode(WIFI_OFF) → WIFI_STA` (full deinit/reinit) instead of `WiFi.disconnect`. Matches the XIAO fix that restored Core/Node connectivity.
- Added `[CORE] RX CORE_REQUEST` diagnostic print in `jcmkOnRecv` and channel-verification prints in both enter functions.

---

## v1.3-beta (2026-02-23)

### New Features
- **WiGLE Upload History Tracking**: Web UI now displays upload statistics (new networks discovered, total networks) for uploaded files
- **Automatic Boot Upload with Quota Management**: Configurable `maxBootUploads` setting (default: 25) to control how many files upload automatically at boot
- **24-Hour History Caching**: Upload history API calls are cached for 24 hours to conserve WiGLE API quota (25 calls/day limit)
- **On-Demand History Refresh**: History automatically refreshes in web UI when cache expires (only when connected to home network)

### Improvements
- **Optimized Upload Performance**: Removed token pre-checks and reduced timeouts for faster batch uploads
- **Enhanced WiFi Stability**: Scanning now properly pauses when connected to home network to prevent connection drops
- **Web Server Startup Timing**: Web server now starts after WiGLE operations complete to avoid resource conflicts
- **Improved Configuration Management**: Added `maxBootUploads` and `speedUnits` configuration options
- **Better Status Display**: Config form in web UI now properly displays all saved values including WiGLE token

### Bug Fixes
- Fixed scanning interference causing 100% ping loss when connected to home WiFi
- Fixed web UI configuration display issues (all fields now populate correctly)
- Fixed chunked encoding errors in `/status.json` and `/files.json` endpoints
- Corrected WiGLE token display (now shows actual token instead of "(set)")
- Fixed JSON buffer overflow issues in files endpoint

### Technical Changes
- Increased JSON buffer for files endpoint from 4KB to 8KB to handle upload statistics
- Switched from HTTP/1.1 to HTTP/1.0 for WiGLE API compatibility
- Added proper `client.flush()` to ensure complete data transmission
- Reduced upload timeout from 60s to 25s for better reliability
- History parsing now uses incremental JSON parsing to reduce memory fragmentation

### Configuration
- New config option: `maxBootUploads` - Max CSV files to upload at boot (0-25, default: 25)
- Updated config option: `speedUnits` - Display speed in km/h or mph
- Config file now saves `maxBootUploads` setting to `/wardriver.cfg`

### Requirements
- **CRITICAL**: PSRAM must be enabled in Arduino IDE for reliable TLS/HTTPS uploads
  - ESP32-C5/C6: Use OPI PSRAM
  - ESP32-S3: Use QSPI PSRAM
- Arduino-ESP32 core v3.0.0 or later
- Updated library dependencies documented in README

### Known Issues
- ESP32-C5/C6 require PSRAM enabled or TLS connections will fail due to insufficient heap
- Initial boot may show "Failed to allocate dummy cacheline for PSRAM" warning (can be ignored)

### Migration Notes
- No breaking changes from v1.2
- Existing `/wardriver.cfg` files are compatible
- New `maxBootUploads` setting will default to 25 if not present in config

---

## v1.2 (Previous Release)
- Initial stable release with basic wardriving functionality
- SD card CSV logging
- Web UI for file management
- Manual WiGLE upload support
