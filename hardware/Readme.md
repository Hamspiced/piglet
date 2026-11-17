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


Keeping the firmware 100% stock means you preserve OTA updates while solving this entirely in hardware between your LiPo battery and the XIAO board.
Because the XIAO's 5V pin only receives power when your vehicle's USB is on (while a fully charged LiPo at 4.2V stays isolated on the BAT+ rail), you can use that 5V rail as a trigger to keep the battery connected for a few minutes after you turn off the ignition, then physically disconnect it.

Pure Hardware Solution: 5V-Triggered RC Delay + MOSFET Battery Cutoff

You can build this with 4 tiny, inexpensive parts wired inline between your LiPo battery and the XIAO's BAT+ pad:
• 1x P-Channel MOSFET (logic-level, low \(R_{DS(on)}\), e.g., AO3401 or IRLML6402) — acts as the high-side battery switch.
• 1x Small Signal Diode (e.g., 1N4148 or BAT54 Schottky) — charges the timer capacitor instantly when 5V USB turns on, and prevents it from draining back into the 5V pin when USB turns off.
• 1x Electrolytic or Tantalum Capacitor (e.g., 100µF or 220µF) — holds the "keep-alive" charge after you shut off the vehicle.
• 1x Bleed Resistor (e.g., 1MΩ to 2.2MΩ) — slowly drains the capacitor over 2 to 5 minutes so the MOSFET cleanly switches off after uploads finish.
(Note: Because a P-channel MOSFET turns ON when its gate is pulled LOW relative to its source, you pair it with a tiny N-channel MOSFET like a 2N7002 or BSS138 whose gate is held HIGH by the capacitor—when the capacitor bleeds down, the N-channel turns off, allowing a pull-up resistor to shut off the P-channel MOSFET completely.)

How It Works in Your Vehicle

1. Driving (5V USB ON): The XIAO's 5V pin charges the capacitor through the diode in less than a second. The battery stays connected so the XIAO can charge the LiPo normally.
2. Park & Turn Off Vehicle (5V USB OFF): The XIAO's 5V pin drops to 0V. The diode blocks the capacitor from discharging backward, so the capacitor slowly bleeds through the resistor for ~3 to 5 minutes.
3. Upload Window: During those 3–5 minutes, the LiPo keeps Piglet running so it finishes connecting to your home Wi-Fi and uploading all pending CSVs.
4. Automatic Zero-Drain Cutoff: Once the capacitor voltage drops below the N-channel threshold (~1.5V), the P-channel MOSFET snaps open and physically disconnects the LiPo. Total standby draw drops to near 0µA (microamps), so your battery will never drain overnight.
5. Next Drive (5V USB ON): Turning on the ignition restores 5V, re-engages the battery in milliseconds, and boots Piglet automatically.

One Important Detail: XIAO Charging Path

Because the XIAO charges the LiPo through the same BAT+ pad that supplies power back to the board, make sure the P-channel MOSFET's Source connects to the XIAO BAT+ pad and its Drain connects to the LiPo + terminal (or use a back-to-back MOSFET pair / load switch IC like the TPS22919 or FPF2124) so charging current flows cleanly while USB is on and discharge current is completely blocked once the timer expires.

Here is the complete 5V-triggered delayed-off battery switch schematic and component list. It fits neatly onto a tiny piece of perfboard or a custom PCB and keeps your Piglet firmware 100% stock.

1. Why Back-to-Back MOSFETs Are Used

A single MOSFET contains an internal body diode that conducts in one direction even when the transistor is turned off. By wiring two P-channel MOSFETs back-to-back (Source-to-Source), their body diodes oppose each other. This guarantees zero leakage current in both directions once the timer expires, while allowing normal LiPo charging while your vehicle is running.

2. Bill of Materials (BOM)

All components are common, inexpensive surface-mount (SOT-23 / 0805) or through-hole parts:
• Q1, Q2 (Dual P-Channel MOSFETs): 2× AO3401 (or 1× dual package like SI4925DY / AO4805). Logic-level, low resistance (\(R_{DS(on)} < 50\text{m}\Omega\)).
• Q3 (N-Channel Driver MOSFET): 1× 2N7002 or BSS138 (Threshold voltage \(V_{GS(th)} \approx 1.5\text{V}\)).
• D1 (Blocking Diode): 1× 1N4148 (small signal diode) or BAT54 (Schottky).
• C1 (Timing Capacitor): 1× 220µF (6.3V or 10V rated, low-leakage tantalum or electrolytic).
• R1 (Bleed / Timer Resistor): 1× 1MΩ (gives ~4.5 minutes of run time after 5V turns off).
	• For ~2.2 minutes: use 470kΩ.
	• For ~6.5 minutes: use 1.5MΩ.
• R2 (Gate Pull-Up Resistor): 1× 100kΩ (holds Q1/Q2 completely OFF when Q3 turns off).
• R3 (Inrush Limiter): 1× 100Ω (protects D1 when C1 charges).

3. Wiring Schematic

text
"  XIAO 5V Pin ────[ R3: 100Ω ]────|>|────┬───────────────────┐
"  (VBUS / USB)                     D1     │                   │
"                                 1N4148   │                   │
"                                       [ C1 ]              [ R1 ]
"                                       220µF               1MΩ
"                                          │                   │
"  GND ────────────────────────────────────┴───────────────────┴──┐
"                                                                 │
"                                                                 │
"  XIAO BAT+ ──────┐                       ┌──────────────────────┼──┐
"  Pad             │                       │                      │  │
"              ┌───┴───┐               ┌───┴───┐                  │  │
"              │  Q1   │               │  Q2   │                  │  │
"              │AO3401 │               │AO3401 │                  │  │
"              │ D   S ├───────┬───────┤ S   D ├─────── LiPo (+)  │  │
"              └───┬───┘       │       └───┬───┘                  │  │
"                  │           │           │                      │  │
"                  │       [  R2  ]        │                      │  │
"                  │        100kΩ          │                      │  │
"                  │           │           │                      │  │
"                  └─────┬─────┴───────────┘                      │  │
"                        │                                        │  │
"                        │ (Common Gates)                         │  │
"                        │                                        │  │
"                    ┌───┴───┐                                    │  │
"                    │ D     │                                    │  │
"                    │   G ──┼────────────────────────────────────┘  │
"                    │   Q3  │  (2N7002 N-Channel)                   │
"                    │ S     │                                       │
"                    └───┬───┘                                       │
"                        │                                           │
"  LiPo (-) & XIAO BAT- ─┴───────────────────────────────────────────┘

Use code with caution.

4. Connection Guide to Seeed XIAO

1. XIAO 5V Pin (Header Pin VBUS): Connect to the input side of R3 (100Ω). Do not connect to the 3V3 pin.
2. XIAO BAT+ Pad (Bottom of XIAO): Connect to the Drain of Q1.
3. LiPo Battery + (Red Wire): Connect to the Drain of Q2.
4. Common Ground (GND): Connect the LiPo - (Black Wire), the XIAO BAT- pad, the Source of Q3, and the negative legs of C1 and R1 all together.

5. Timing & Cutoff Math

When you shut off your vehicle, the XIAO 5V pin drops to 0V and D1 stops C1 from draining backward. C1 discharges through R1 according to:
\(V(t)=V_{start}\times e^{-\frac{t}{R_{1}\cdot C_{1}}}\)
• Starting Voltage (\(V_{start}\)): ≈ 4.3V (5V USB minus the ~0.7V drop across 1N4148).
• Cutoff Threshold (\(V_{GS(th)}\) of 2N7002): ≈ 1.5V.
• With 220µF + 1MΩ: Time constant τ = 220s. It takes roughly \(1.05 \times \tau \approx \mathbf{230\text{ \textbf{seconds} (\sim 3.8\text{ \textbf{minutes}})}}\) to cross the 1.5V gate threshold.
• Standby Drain After Cutoff: Once Q3 turns off, R2 pulls Q1 and Q2 gates up to battery voltage, shutting both MOSFETs off completely. Total standby current drawn from your LiPo is under 1 µA (your battery's own self-discharge will be higher than this circuit's draw).

Assembly Tip

If you use a standard electrolytic capacitor for C1, choose a reputable low-leakage brand (like Nichicon or Panasonic) so internal capacitor leakage doesn't shorten your 4-minute timer window. Ceramic capacitors (220µF MLCC) lose significant capacitance under DC bias, so stick with electrolytic or tantalum for C1.

