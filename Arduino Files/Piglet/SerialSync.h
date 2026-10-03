#pragma once
#include <Arduino.h>

// SerialSync — lets a host (e.g. Ragnar) pull the CSV logs off the SD card
// over the USB serial port, so a solo drive can be imported by just plugging
// Piglet in. Call serialSyncPoll() every loop(); it never blocks unless a
// command is being served.
//
// Protocol (host -> Piglet, one line each, '\n' terminated):
//   @PIGLET HELLO           -> @PH <fw> <chip> <mac> rst=<reset reason> up=<s> sd=<0|1>
//   @PIGLET LIST            -> @PL BEGIN
//                              @PL F <path>\t<size>\t<active 0|1>\t<mtime epoch>
//                              @PL END <count>
//   @PIGLET GET <path> [off] -> @PG BEGIN <path> <size> <off>
//                              @PG D <seq> <crc32 hex> <base64 of up to 144 bytes>
//                              @PG END <path> <size> <bytes sent through>
//                              (or @PG ERR <reason>; `read-error <off>` = SD read failed)
//   `off` resumes a transfer (multiple of 144); every data line carries its
//   own CRC32 so a host can detect a damaged chunk and resume right there.
// Only /logs/*.csv and /uploaded/*.csv are served (read-only). Other output
// (boot log, status messages) can interleave between lines; hosts match the
// @P prefixes and verify size + CRC32.
void serialSyncPoll();
