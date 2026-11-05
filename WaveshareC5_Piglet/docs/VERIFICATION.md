# Verification and supported scope

## Supported hardware and source

The hardware-tested profile is a Waveshare ESP32-C5-Touch-LCD-3.5 running Core
with one Seeed XIAO ESP32-C5 running the matching standalone node firmware.
The Waveshare has a 320×480 ST7796 display, FT6336 touch, onboard LC76G GPS,
and SPI SD storage. See [NOTICE](../NOTICE.md) for source pins and licenses.

This contribution packages the tested firmware under a board-specific directory;
existing upstream variants are unchanged. Both relocated PlatformIO projects and
the host checks are built again before submission. Hardware results below come
from the preceding development builds, not a new bench run after packaging.

## Implemented behavior

- Node Wi-Fi scans request active mode with 80 ms minimum and maximum dwell.
  Regulatory restrictions can affect actual RF behavior; RF dwell was not measured.
- Core divides the 40-entry channel table into contiguous index ranges. The final
  node receives the remainder; rank changes the ordering of assigned 5 GHz scans.
  Channel scheduling across several nodes has not been hardware-qualified.
- Reporting holds the radio on channel 6 and drains serialized sends before
  scanning resumes. Reporting duration depends on record count and retries;
  there is no fixed reporting time slice or persistent delivery backlog.
- Record ACK waits are 150 ms, with at most three total attempts. Core checks
  full CSV row writes, `fflush`, and `fsync` before advancing duplicate state and
  acknowledging reliable records. Storage failure stops logging and withholds ACKs.
- Duplicate suppression is in memory for the active session. It is not a durable
  exactly-once journal across Core resets.
- Node receive callbacks enqueue events; the main loop owns pairing and channel
  transitions. Missing driver callbacks cause a bounded restart after roughly
  three seconds. Restart discards unconfirmed queued records. Forced re-search
  waits for an outstanding send to retire before resetting transport state.
- Scan-record parsing preserves commas within SSIDs and rejects malformed fixed
  fields. This is separate from CSV quoting in the storage path.

## Hardware results

| Check | Observed result |
|---|---|
| Touch/navigation | Previous/Next worked, including repeated taps and one action per held press; Core selection and leaving Mesh worked. |
| Pairing/recovery | Stable paired run; node timed out after Core disappeared and rediscovered Core when restarted. |
| Serialized reporting | A 256-record batch and a subsequent 298-record run were received without missing records in those captures. |
| Record ACK loss | Dropping one ACK caused a retry; dropping all ACKs caused expiry at three attempts; subsequent records recovered. Core logged duplicate retransmissions once. |
| Clean reliable run | 333 records reconciled; four retries, no missing records in that run. |
| Normal reboot/readback | 129 acknowledged records matched 129 valid CSV rows; file bytes were unchanged across a normal flush/close/reboot. |
| Abrupt USB power loss | Before commit-before-ACK, two of 49 acknowledged records were lost. With checked commits before ACK, a comparative cut recovered all 49 acknowledged records. |
| Commit failure | Injected sync failure withheld ACKs, exhausted bounded retries, and did not report a successful commit. |
| Missing driver callback | Injected callback loss caused a restart at about three seconds, followed by pairing and acknowledged delivery. |
| Forced re-search | Re-pairing and record delivery recovered. |
| Final normal build | A 60-second capture contained 295 consecutive unique record ACKs, with no expiry, restart, queue drop, or injected fault. |

Power-loss evidence is one card and manually timed cuts, not a guarantee for all
cards or interruption points. The physical power-cut test was not repeated after
the final node/parser stabilization; the checked storage policy was unchanged.
Raw serial captures, receipts, and CSV files are intentionally not distributed.

## Host and build checks

Both firmware targets compile with normal configuration. Bench-only ACK-loss,
callback-loss, readback, power-loss, and commit-failure controls are disabled.
Run the [README host commands](../README.md#host-checks) for storage failure cases,
protocol behavior, and scan-record edge cases. Parser tests were also run with
AddressSanitizer/UndefinedBehaviorSanitizer, including 10,000 malformed boundary
inputs. Python reconciliation tools accept locally captured artifacts.

## Remaining limits

Legacy peers and Waveshare Node mode do not implement this standalone node's
reliable record protocol. Reverse roles and multi-node behavior are unverified.
No persistent replay queue, durable deduplication journal, adaptive dwell, or
multi-node scheduling optimization is claimed. BLE, uploads, all UI pages, and
GPS accuracy have not received comprehensive qualification in this test scope.
Mesh transport is not encrypted by this contribution.
