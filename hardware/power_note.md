### Problem / Use Case
When running Piglet in a vehicle powered via switched USB (ignition-switched 12V-to-5V adapter), arriving home triggers the **Home network auto-return** upload mode. 
* **Without a battery:** Turning off the ignition immediately cuts power and interrupts pending uploads.
* **With a LiPo battery attached:** Turning off the ignition allows the upload to finish, but once the upload completes (or home Wi-Fi disconnects), Piglet automatically resumes scanning and drains the LiPo battery completely overnight.

### Proposed Solutions
Since USB `VBUS` is **5V** and a fully charged LiPo is at most **4.2V** (isolated by the XIAO's internal Schottky diode), Piglet can distinguish between **Vehicle ON (5V present)** and **Vehicle OFF (battery only, VBUS = 0V)**.

#### 1. Firmware Option: Ignition-Sense Auto-Sleep (`sleepAfterHomeUpload` / VBUS Sense)
Add an optional config flag in `/wardriver.cfg` (e.g., `sleepAfterHomeUpload=false` or `vbusSensePin=-1`):
* **Software-only mode (`sleepAfterHomeUpload=true`):** When `homeSsid` auto-return triggers and finishes uploading all pending CSVs, Piglet flushes the SD card, powers down the OLED, and enters deep sleep (`esp_deep_sleep_start()`) instead of resuming scanning.
* **Hardware VBUS-sense mode (`vbusSensePin=<GPIO>`):** Using a simple 2-resistor voltage divider from the XIAO `5V` pin to a spare GPIO:
  * When `5V` drops to `0V` (ignition off), Piglet finishes any active `homeSsid` upload (with a configurable timeout if home Wi-Fi isn't in range) and immediately enters deep sleep.
  * When `5V` returns (`HIGH` on next ignition start), it can optionally wake the board automatically.

#### 2. Pure Hardware Option (Documentation / PCB Revision Note)
For users running stock firmware without extra GPIOs, adding a **5V-triggered RC delay + back-to-back P-channel MOSFET cutoff** inline between `BAT+` and the LiPo keeps the battery connected for ~3–5 minutes after `5V` drops (long enough to complete home uploads), then physically disconnects the battery (`<1µA` standby draw) until `5V` returns.

### Benefits
* Prevents overnight LiPo battery depletion in vehicle installations.
* Allows completely hands-off daily driving: park, turn off the ignition, let auto-return finish uploading, and leave the device unattended without manual button holds.
* Fully backwards-compatible (`false` / disabled by default).
