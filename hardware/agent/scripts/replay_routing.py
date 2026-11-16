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


def zone_snapshot(board):
    result = []
    for zone in board.Zones():
        assert not zone.GetIsRuleArea() and zone.GetFillMode() == 0, 'Only solid copper pours are supported'
        shape = zone.Outline()
        def contour(line):
            return [point(line.CPoint(i)) for i in range(line.PointCount())]
        result.append({
            'net': zone.GetNetname(), 'layer': board.GetLayerName(zone.GetLayer()),
            'clearance': p.ToMM(zone.GetLocalClearance()), 'minimum_thickness': p.ToMM(zone.GetMinThickness()),
            'thermal_gap': p.ToMM(zone.GetThermalReliefGap()),
            'thermal_spoke_width': p.ToMM(zone.GetThermalReliefSpokeWidth()),
            'pad_connection': int(zone.GetPadConnection()), 'priority': zone.GetAssignedPriority(),
            'island_removal': int(zone.GetIslandRemovalMode()), 'minimum_island_area_internal': zone.GetMinIslandArea(),
            'polygons': [{'outline': contour(shape.COutline(i)),
                          'holes': [contour(shape.CHole(i, j)) for j in range(shape.HoleCount(i))]}
                         for i in range(shape.OutlineCount())],
        })
    return result


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
        overrides = [{'reference': f.GetReference(), 'number': d.GetNumber(), 'position': point(d.GetPosition()),
                      'connection': int(d.GetLocalZoneConnection())} for f in board.GetFootprints() for d in f.Pads()]
        DATA.write_text(json.dumps({'placement': placement(board), 'copper': copper, 'zones': zone_snapshot(board),
                                    'pad_zone_connections': overrides}, indent=2) + '\n')
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
        if 'zones' not in data or 'pad_zone_connections' not in data:
            raise SystemExit('Copper snapshot lacks pour settings. Recapture the validated routed board.')
        for zone in list(board.Zones()):
            board.RemoveNative(zone)
        for item in data['zones']:
            zone = p.ZONE(board)
            zone.SetLayer(board.GetLayerID(item['layer']))
            zone.SetNet(board.FindNet(item['net']))
            zone.SetLocalClearance(p.FromMM(item['clearance']))
            zone.SetThermalReliefGap(p.FromMM(item['thermal_gap']))
            zone.SetThermalReliefSpokeWidth(p.FromMM(item['thermal_spoke_width']))
            zone.SetPadConnection(item['pad_connection'])
            zone.SetMinThickness(p.FromMM(item['minimum_thickness']))
            zone.SetAssignedPriority(item['priority'])
            zone.SetIslandRemovalMode(item['island_removal'])
            zone.SetMinIslandArea(item['minimum_island_area_internal'])
            outline = zone.Outline()
            for polygon in item['polygons']:
                index = outline.NewOutline()
                for xy in polygon['outline']:
                    outline.Append(vector(xy), index, -1)
                for hole in polygon['holes']:
                    hole_index = outline.NewHole(index)
                    for xy in hole:
                        outline.Append(vector(xy), index, hole_index)
            board.Add(zone)
        overrides = {(a['reference'], a['number'], tuple(a['position'])): a['connection']
                     for a in data['pad_zone_connections']}
        for f in board.GetFootprints():
            for d in f.Pads():
                d.SetLocalZoneConnection(overrides[(f.GetReference(), d.GetNumber(), tuple(point(d.GetPosition())))])
        apply_reference_labels(board)
        board.BuildConnectivity()
        p.ZONE_FILLER(board).Fill(board.Zones())
        p.SaveBoard(str(ROOT / 'saihub.kicad_pcb'), board)
        print('Replayed copper. Run export_placement.py --routed to validate and refresh the review files.')
finally:
    (ROOT / 'saihub.kicad_pro').write_text(project)
