#!/usr/bin/env python3
"""Export a procurement BOM with matching components grouped into one row."""
import csv
import json
from pathlib import Path
import re


def reference_key(reference):
    return [int(token) if token.isdigit() else token for token in re.split(r'(\d+)', reference)]


def write_bom(path, parts):
    groups = {}
    for part in parts:
        if part['ref'].startswith('TP'):
            continue
        spec = part['mpn']
        if part['sym'] == 'R':
            spec = f"Generic {part['value']} ohm, 1%, 0603, >=0.1W"
        elif part['sym'] == 'C':
            size = next(size for size in ['1210', '0805', '0603'] if size in part['foot'])
            spec = f"Generic {part['value']}F, X7R, >=10V, +/-10%, {size}"
        key = (part['sym'], part['value'], spec, part['foot'])
        group = groups.setdefault(key, {'refs': [], 'notes': [], 'urls': []})
        group['refs'].append(part['ref'])
        note = part['desc'] + '; ' + part['side'] + ' assembly'
        if note not in group['notes']:
            group['notes'].append(note)
        if part['url'] and part['url'] not in group['urls']:
            group['urls'].append(part['url'])

    with Path(path).open('w', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['Reference', 'Quantity', 'Value', 'MPN_or_procurement_spec', 'Footprint', 'Notes', 'Datasheet'])
        for (_, value, spec, footprint), group in groups.items():
            refs = sorted(group['refs'], key=reference_key)
            writer.writerow([', '.join(refs), len(refs), value, spec, footprint,
                             '; '.join(group['notes']), '; '.join(group['urls'])])
        writer.writerow(['ANT1 (off-board)', 1, 'Dual-band external antenna',
                         '50 ohm 2.4/5GHz U.FL-compatible antenna; qualify with final enclosure',
                         'Not PCB mounted', 'Required accessory; not included in placement file', ''])


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[2]
    data = json.loads((root/'agent/design.json').read_text())
    write_bom(root/'docs/bom.csv', data['parts'])
    print('Updated docs/bom.csv')
