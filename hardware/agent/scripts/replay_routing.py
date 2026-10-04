#!/usr/bin/env python3
"""Capture or replay copper for the exact generated placement (KiCad Python)."""
import argparse
import json
from pathlib import Path

import pcbnew as p
import wx

from silkscreen import apply_reference_labels

app = wx.App(False)
ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / 'agent/routing/copper.json'


def point(v):
    return [round(p.ToMM(v.x), 6), round(p.ToMM(v.y), 6)]


def vector(v):
    # Round before conversion so binary float error cannot shift endpoints by one unit.
    scale = p.FromMM(1)
    return p.VECTOR2I(int(round(v[0] * scale)), int(round(v[1] * scale)))


def placement(board):
    return sorted([
        {'ref': f.GetReference(), 'footprint': str(f.GetFPID().GetLibNickname()) + ':' + str(f.GetFPID().GetLibItemName()),
         'position': [round(v, 5) for v in point(f.GetPosition())], 'rotation': round(f.GetOrientationDegrees(), 6),
         'bottom': f.IsFlipped(),
         'pads': sorted([{'number': d.GetNumber(), 'net': '' if d.GetNetname().startswith('unconnected-') else d.GetNetname(),
                          'position': [round(v, 5) for v in point(d.GetPosition())]} for d in f.Pads()], key=lambda d: (d['number'], d['position']))}
        for f in board.GetFootprints()
    ], key=lambda f: f['ref'])


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('mode', choices=['capture', 'replay'])
args = parser.parse_args()
project = (ROOT / 'saihub.kicad_pro').read_text()
try:
    if args.mode == 'capture':
        board = p.LoadBoard(str(ROOT / 'saihub.kicad_pcb'))
        copper = []
        for t in board.GetTracks():
            item = {'net': t.GetNetname(), 'locked': t.IsLocked()}
            if isinstance(t, p.PCB_VIA):
                item.update(kind='via', position=point(t.GetPosition()),
                            width=p.ToMM(t.GetWidth(p.F_Cu)), drill=p.ToMM(t.GetDrillValue()))
            else:
                item.update(kind='track', start=point(t.GetStart()), end=point(t.GetEnd()),
                            width=p.ToMM(t.GetWidth()), layer=board.GetLayerName(t.GetLayer()))
            copper.append(item)
        DATA.write_text(json.dumps({'placement': placement(board), 'copper': copper}, indent=2) + '\n')
        print(f'Captured {len(copper)} copper items; run routed validation before accepting this snapshot.')
    else:
        board = p.LoadBoard(str(ROOT / 'agent/routing/saihub-unrouted.kicad_pcb'))
        data = json.loads(DATA.read_text())
        if placement(board) != data['placement']:
            raise SystemExit('Placement or connectivity differs from the copper snapshot. Route the changed board first.')
        for t in list(board.GetTracks()):
            board.RemoveNative(t)
        for item in data['copper']:
            if item['kind'] == 'via':
                t = p.PCB_VIA(board)
                t.SetPosition(vector(item['position']))
                t.SetDrill(p.FromMM(item['drill']))
                t.SetViaType(p.VIATYPE_THROUGH)
                t.SetLayerPair(p.F_Cu, p.B_Cu)
            else:
                t = p.PCB_TRACK(board)
                t.SetStart(vector(item['start']))
                t.SetEnd(vector(item['end']))
                t.SetLayer(board.GetLayerID(item['layer']))
            t.SetWidth(p.FromMM(item['width']))
            board.Add(t)
            t.SetNet(board.FindNet(item['net']))
            t.SetLocked(item['locked'])
        # The placement board has no pours. Restore the two ground planes.
        for layer in (p.F_Cu, p.B_Cu):
            zone = p.ZONE(board)
            zone.SetLayer(layer)
            zone.SetNet(board.FindNet('GND'))
            zone.SetLocalClearance(p.FromMM(.2))
            zone.SetThermalReliefGap(p.FromMM(.25))
            zone.SetThermalReliefSpokeWidth(p.FromMM(.3))
            zone.SetPadConnection(p.ZONE_CONNECTION_THERMAL)
            zone.SetMinThickness(p.FromMM(.2))
            outline = zone.Outline()
            outline.NewOutline()
            for x, y in ((.3, .3), (47.7, .3), (47.7, 27.7), (.3, 27.7)):
                outline.Append(p.FromMM(x), p.FromMM(y))
            board.Add(zone)
        for f in board.GetFootprints():
            for d in f.Pads():
                if d.GetNetname() == 'GND':
                    d.SetLocalZoneConnection(p.ZONE_CONNECTION_FULL)
        apply_reference_labels(board)
        board.BuildConnectivity()
        p.ZONE_FILLER(board).Fill(board.Zones())
        p.SaveBoard(str(ROOT / 'saihub.kicad_pcb'), board)
        print('Replayed copper. Run export_placement.py --routed to validate and refresh the review files.')
finally:
    (ROOT / 'saihub.kicad_pro').write_text(project)
