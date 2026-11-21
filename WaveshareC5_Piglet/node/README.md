# Standalone XIAO ESP32-C5 node

Derived from Hamspiced/piglet `PigletNode/PigletNode.ino` at commit
`4e47f41f00ad2a63660d78f8db20bd33457a7b2c`. Piglet-derived code is
CC BY-NC-SA 4.0; see the repository LICENSE and CREDITS.md.

Build with `pio run -d node`. Board selection and platform version are pinned
in platformio.ini. Select the verified XIAO serial port explicitly for upload.

Local changes include coordinated channel-6 configuration windows, unique
record sequence numbers, payload-free diagnostics, and a bounded transmit
queue. Only one frame is in flight. Scanning resumes after the queue and the
in-flight frame have drained. Receive callbacks publish complete input events; the main loop owns pairing,
liveness, configuration, and control responses. Transmit callbacks report
completion through a queue, with overflow treated as a recovery fault.

A missing transmit callback reports a stall after one second and restarts the
node after three seconds, rather than submitting an ambiguous second send.
Queued data at restart is explicitly unconfirmed and is not persisted. Queue-full and driver
rejection conditions are reported. Records use the local `CTRL2` protocol and unicast to their paired Core.
Each record carries a random nonzero boot-session ID and an increasing sequence.
The node waits up to 150 ms per attempt for a matching application ACK and uses
at most three total attempts. Core acknowledges only after a complete CSV row write and successful checked
`fflush()` and `fsync()` calls, suppressing repeated committed records using a
per-node/session sequence high-water mark. Sending remains serialized while waiting for ACKs.

This requires the matching Core implementation in this repository. Legacy
upstream cores do not implement these record ACKs. Duplicate suppression is
in memory and scoped to the current Core/node session; it is not a durable
exactly-once guarantee across Core resets or node expiry. An ACK confirms the filesystem commit calls succeeded; SD-card power-loss
behavior is limited to the comparative bench result described below. A short write or commit error
latches the log handle failed and stops logging rather than appending an
uncertain record again. Logging must be explicitly reopened/restarted after
that failure. Queue-full records and records whose
ACK retries expire are reported and dropped; the node then continues.

`PIGLET_RECORD_ACK_TEST`, when explicitly supplied as a test build flag, ignores
the first application ACK for record 2 and every ACK for record 3 on the node.
Normal builds omit this flag. The ACK is still sent by Core, so this tests
application ACK loss, not injected radio-layer failure.

The verified hardware pairing is XIAO ESP32-C5 node to Waveshare ESP32-C5
Touch LCD 3.5 Core in this repository. Other upstream interoperability claims
in the inherited source header have not been reverified here. See
../docs/VERIFICATION.md for hardware results and remaining limitations.

## Supported profile

The supported profile is Waveshare Core plus one standalone XIAO C5
Node. The application requests active scans with an 80 ms minimum and maximum;
driver country/DFS behavior and actual per-channel airtime are not inferred
from that setting. Channel allocation remains contiguous and reporting time
remains variable. The console reports queue attempts separately from the
record-ACK messages; queue attempts are not a delivery count.

A forced re-search waits for the active send callback, then discards old queued
work as unconfirmed before rediscovery. A missing callback takes the bounded
restart path. Firmware does not maintain a persistent delivery queue. Legacy
peers, multiple scanning nodes, radio encryption, and the Waveshare Node-mode
path remain outside this tested profile. The inherited mesh is unencrypted.
