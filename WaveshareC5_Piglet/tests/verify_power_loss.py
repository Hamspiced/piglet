"""Reconcile pre-cut ACK receipts with a recovered CSV; print metadata only."""
import argparse
import csv
import io
import json
import re
import zlib
from pathlib import Path


def reconcile(armed, content, serial_log):
    receipts = armed['receipts']
    expected = set(armed['acks'])
    captured = {int(seq) for seq in re.findall(r'node \[NODE\] RECORD_ACK seq=(\d+) attempts=', serial_log)}
    assert expected == captured == {item['seq'] for item in receipts}, 'ACK evidence mismatch'
    assert len(expected) == len(receipts) and len({item['session'] for item in receipts}) == 1
    present, missing, changed = [], [], []
    for item in sorted(receipts, key=lambda item: item['seq']):
        offset, length = item['offset'], item['length']
        chunk = content[offset:offset + length]
        if offset >= len(content):
            missing.append(item['seq'])
        elif len(chunk) != length or zlib.crc32(chunk) != item['crc']:
            changed.append(item['seq'])
        else:
            present.append(item['seq'])
    rows = list(csv.reader(io.StringIO(content.decode('latin1'), newline='')))
    valid = bool(len(rows) >= 2 and rows[0][0] == 'WigleWifi-1.6' and rows[1][0] == 'MAC'
                 and all(len(row) == 14 for row in rows[1:]))
    flushed_through = armed['limit'] - armed['pending_at_cut']
    return dict(acknowledged=len(expected), matched_csv_receipts=len(present),
                missing_sequences=missing, changed_sequences=changed,
                csv_data_rows=max(0, len(rows) - 2), csv_structure_valid=valid,
                flushed_prefix_recovered=set(range(1, flushed_through + 1)).issubset(present),
                durability_pass=not missing and not changed and valid)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('armed', 'after', 'serial-log'):
        parser.add_argument('--' + option, type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(reconcile(json.loads(args.armed.read_text()), args.after.read_bytes(), args.serial_log.read_text())))
