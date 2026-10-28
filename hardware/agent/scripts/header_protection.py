"""Schematic additions shared by metadata updates and full regeneration.

New parts deliberately have no PCB placement. Assign reviewed coordinates before
regenerating a board; schematic-only work must not invent a placement.
"""
import uuid

NS = uuid.UUID('8ce59343-cd0c-4971-923b-6d37131b8b35')
SS34_URL = 'https://datasheet.lcsc.com/datasheet/pdf/dfa1ff67dea875d0135103ba9ada713a.pdf?productCode=C8678'


def apply(parts):
    by_ref = {part['ref']: part for part in parts}
    for ref in ('U4', 'U5'):
        by_ref[ref].update(value='TPS2553DDBVR', mpn='TPS2553DDBVR',
                          url='https://www.ti.com/lit/ds/symlink/tps2553d.pdf')
    for ref in ('D1', 'D3'):
        by_ref[ref].update(value='SS34', mpn='SS34', url=SS34_URL,
                          desc='MDD SS34, 40V / 3A, SMA (DO-214AC); do not substitute SMC variants')
    # Replace only the superseded GPIO arrays and their local supply bypasses.
    retired = {'U8', 'U9', 'C17', 'C18', 'C19', 'C20', 'C21'}
    parts[:] = [part for part in parts if part['ref'] not in retired]
    for index in range(2):
        ref = f'U{index + 6}'
        part = by_ref.get(ref)
        if part is None:
            part = dict(ref=ref, pos=None, rot=0, side='top', page='header_esd', sch=[0, 0],
                        uuid=str(uuid.uuid5(NS, ref)))
            parts.append(part)
        part.update(value='TPD4E05U06DQAR', sym='TPD4E05U06', foot='Saihub:TPD4E05U06_DQA',
                    nets={str(pin): f'IO{index * 4 + channel}' for channel, pin in enumerate((1, 2, 4, 5))},
                    mpn='TPD4E05U06DQAR', desc='Four GPIO ESD shunts; USON-10 2.5x1mm; no supply required',
                    url='https://www.ti.com/lit/ds/symlink/tpd4e05u06.pdf')
        part['nets'].update({'3': 'GND', '8': 'GND', '6': None, '7': None, '9': None, '10': None})
    # Keep the separate header-power shunts. Load current bypasses the array.
    definitions = [
        dict(ref='U10', value='TPD4E05U06DQAR', sym='TPD4E05U06', foot='Saihub:TPD4E05U06_DQA',
             nets={'1': 'V3_SW', '2': None, '3': 'GND', '4': None, '5': 'V5_SW',
                   '6': None, '7': None, '8': 'GND', '9': None, '10': None},
             mpn='TPD4E05U06DQAR', desc='Two independent header-power ESD shunts; no supply reference; load current bypasses array',
             url='https://www.ti.com/lit/ds/symlink/tpd4e05u06.pdf'),
    ]
    for part in definitions:
        if part['ref'] in by_ref:
            by_ref[part['ref']].update(part)
        else:
            part.update(pos=None, rot=0, side='top', page='header_esd', sch=[0, 0],
                        uuid=str(uuid.uuid5(NS, part['ref'])))
            parts.append(part)
