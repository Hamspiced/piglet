"""Reconcile private SD audit artifacts without displaying network payloads."""
import argparse
import csv
import hashlib
import io
import json
import re
import zlib
from pathlib import Path


def reconcile(receipts, before, after, serial_log):
    items = receipts['receipts']
    assert items, 'No receipts'
    count = len(items)
    acknowledged = {int(x) for x in re.findall(r'node \[NODE\] RECORD_ACK seq=(\d+) attempts=', serial_log)}
    sequences = {item['seq'] for item in items}
    assert sequences == acknowledged == set(range(1, count + 1)), 'Acknowledgment/receipt mismatch'
    assert len({item['session'] for item in items}) == 1, 'Mixed sessions'
    for content in (before, after):
        rows = list(csv.reader(io.StringIO(content.decode('latin1'), newline='')))
        assert rows[0][0] == 'WigleWifi-1.6' and rows[1][0] == 'MAC', 'Wrong CSV headers'
        assert len(rows) == count + 2 and all(len(row) == 14 for row in rows[1:]), 'Wrong CSV structure/count'
        cursor = content.find(b'\n', content.find(b'\n') + 1) + 1
        for item in sorted(items, key=lambda item: item['offset']):
            assert item['offset'] == cursor, 'Gap or overlap between CSV receipts'
            row = content[cursor:cursor + item['length']]
            assert len(row) == item['length'] and row.endswith(b'\r\n'), 'Incomplete CSV row'
            assert zlib.crc32(row) == item['crc'], 'CSV bytes differ from expected append bytes'
            cursor += len(row)
        assert cursor == len(content), 'Extra or missing CSV bytes'
    assert before == after, 'CSV changed across reboot'
    return dict(acknowledged=count, csv_rows=count, matched_receipts=count,
                reboot_bytes_unchanged=True, bytes=len(after), sha256=hashlib.sha256(after).hexdigest())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('receipts', 'before', 'after', 'serial-log'):
        parser.add_argument('--' + option, type=Path, required=True)
    args = parser.parse_args()
    result = reconcile(json.loads(args.receipts.read_text()), args.before.read_bytes(),
                       args.after.read_bytes(), args.serial_log.read_text())
    print(json.dumps(result))
