#!/usr/bin/env python3
"""Prepare locked critical routes / import Specctra results / fill ground copper.

Run with KiCad's bundled Python. Freerouting runs locally between prepare/import.
"""
import json
import sys
from pathlib import Path
import pcbnew as p
import wx
app = wx.App(False)
from identity import identity, apply_board_identity
ROOT=Path(__file__).resolve().parents[2]
for directory in ('agent/validation', 'agent/previews', 'agent/routing', 'docs'):
    (ROOT/directory).mkdir(parents=True, exist_ok=True)
design=json.loads((ROOT/'agent/design.json').read_text())
if any(part['ref'] == 'D4' for part in design['parts']):
    raise SystemExit('The indicator layout uses replay_routing.py replay. The legacy routing recipe has different placement.')
assert next(a for a in design['parts'] if a['ref']=='U3')['value']=='ESPC5-12-H4', 'Routing recipe requires the PCB-antenna module placement'
assert design['revision']==identity()['revision'] and design['board_mm']==[48.0,28.0], 'Routes require SAIHub-Mini 48 x 28 mm placement'
def mm(x): return p.FromMM(x)
def xy(x,y): return p.VECTOR2I(mm(x),mm(y))
mode=sys.argv[1]
project=(ROOT/'saihub.kicad_pro').read_text()
b=p.LoadBoard(str(ROOT/('agent/routing/saihub-unrouted.kicad_pcb' if mode=='prepare' else 'agent/routing/saihub-prerouted.kicad_pcb')))
fps={f.GetReference():f for f in b.GetFootprints()}
nets={n.GetNetname():n.GetNetCode() for n in b.GetNetInfo().NetsByNetcode().values()}
def point(ref,num):
    pad=next(a for a in fps[ref].Pads() if a.GetNumber()==str(num))
    return p.ToMM(pad.GetPosition().x),p.ToMM(pad.GetPosition().y)
def wire(net,points,w=.2,layer=p.F_Cu,locked=True):
    for a,c in zip(points,points[1:]):
        if a==c:continue
        t=p.PCB_TRACK(b);t.SetStart(xy(*a));t.SetEnd(xy(*c));t.SetWidth(mm(w));t.SetLayer(layer);t.SetNetCode(nets[net]);t.SetLocked(locked);b.Add(t)
def via(net,pt,size=.6,drill=.3):
    v=p.PCB_VIA(b);v.SetPosition(xy(*pt));v.SetWidth(mm(size));v.SetDrill(mm(drill));v.SetViaType(p.VIATYPE_THROUGH);v.SetLayerPair(p.F_Cu,p.B_Cu);v.SetNetCode(nets[net]);v.SetLocked(True);b.Add(v)
if mode=='prepare':
    # Buck switching path: short U2-to-diode path, then a wide front-side trunk.
    wire('SW',[point('U2',3),(43.4,13.365),(44.265,12.5),point('D1',1)],.6)
    wire('SW',[(43.4,13.365),(43.4,21.45),(46.35,24.4)],.6)
    wire('SW',[(43.4,19.2),point('R18',1)],.6)
    # Input bypass and output reservoir use wider copper than signal escapes.
    wire('V5',[point('U2',2),(42.6,14.635),(42.6,16.8),(40.9,16.8),(40.9,18.275),point('C2',1)],.6)
    wire('V5',[point('C2',1),(41.225,19.4),point('C3',1)],.6)
    wire('V3V3',[point('L1',2),(41.85,22.9),(39.075,22.9),point('C4',1)],.8)
    for pt in [(37.7,13.2),(37.7,14.0),(37.7,14.8)]:via('GND',pt,.6,.3)
    for pt in [(6.5,8.97),(8.5,8.97),(6.5,10.97),(8.5,10.97)]:via('GND',pt,.6,.3)
    wire('GND',[point('U2',4),(39.8,12.095),(38.3,13.595),point('U2',9)],.6)
    for ref,num in [('C2',2),('C3',2),('C4',2),('D1',2)]:via('GND',point(ref,num),.8,.4)
    # Keep the feedback/compensation on the quiet side of the converter.
    wire('FB',[point('U2',5),(33.8,12.095),(32.4,13.495),point('R15',2)],.2)
    wire('FB',[point('R15',2),point('R16',1)],.2)
    wire('COMP',[point('U2',6),(33.8,13.365),(33.3,13.865),(33.3,15.925),point('C16',1)],.2)
    wire('COMP_RC',[point('C16',2),point('R17',1)],.2)
    # Current limit nodes remain short and direct.
    wire('ILIM4',[point('U4',5),(27.65,3.35),(29.55,3.35),(29.55,5.275),point('R7',1)],.2)
    wire('ILIM5',[point('U5',5),(27.65,9.9),(29.5,9.9),(29.5,11.625),point('R8',1)],.2)
    for ref,cap,net,y in [('U4','C9','V3V3',7.5),('U5','C10','V5',14.0)]:
        wire(net,[point(ref,1),point(cap,1)],.6)
        wire(net,[point(ref,1),(26,y)],.6);via(net,(26,y),.8,.4)
    for ref,cap,net,y in [('U4','C11','V3_SW',3.7),('U5','C12','V5_SW',10.2)]:
        wire(net,[point(ref,6),(26,y)],.6);via(net,(26,y),.8,.4)
        via(net,point(cap,1),.8,.4)
    # Supply bypass next to the module uses a short, wide route on the back.
    via('V3V3',point('U3',8),.8,.4)
    via('V3V3',point('C6',1),.8,.4)
    # Existing fixed coordinates outside the unchanged power stage are obsolete.
    out=ROOT/'agent/routing/saihub-prerouted.kicad_pcb'
    p.SaveBoard(str(out),b)
    (ROOT/'agent/routing/saihub-prerouted.kicad_pro').write_text(project)
    p.ExportSpecctraDSN(b,str(ROOT/'agent/validation/saihub.dsn'))
    print('Prepared SAIHub-Mini locked power routes and revised module ground vias.')
elif mode=='import':
    if not p.ImportSpecctraSES(b,str(ROOT/'agent/validation/saihub.ses')): raise SystemExit('SES import failed')
    # Replay the reviewed cleanup and final routes for this exact placement.
    cleanup=json.loads((ROOT/'agent/validation/routing-cleanup.json').read_text())
    def position(pt): return [round(p.ToMM(pt.x),4),round(p.ToMM(pt.y),4)]
    for track in list(b.GetTracks()):
        if isinstance(track,p.PCB_VIA):
            remove=any(v['net']==track.GetNetname() and position(track.GetPosition())==[round(v['x'],4),round(v['y'],4)] for v in cleanup['remove_vias'])
        else:
            ends=[position(track.GetStart()),position(track.GetEnd())]
            remove=any(v['net']==track.GetNetname() and v['layer']==(0 if track.GetLayer()==p.F_Cu else 1) and (ends==[v['start'],v['end']] or ends==[v['end'],v['start']]) for v in cleanup['remove_tracks'])
        if remove:b.RemoveNative(track)
    # Apply recorded local corrections after the imported session coordinates.
    for change in json.loads((ROOT/'agent/validation/track-adjustments.json').read_text()):
        for track in b.GetTracks():
            if isinstance(track,p.PCB_VIA) or track.GetNetname()!=change['net']:continue
            if (0 if track.GetLayer()==p.F_Cu else 1)!=change['layer']:continue
            ends=[position(track.GetStart()),position(track.GetEnd())]
            if ends==change['old']:
                track.SetStart(xy(*change['new'][0]));track.SetEnd(xy(*change['new'][1]))
    for change in json.loads((ROOT/'agent/validation/via-adjustments.json').read_text()):
        for track in b.GetTracks():
            if isinstance(track,p.PCB_VIA) and track.GetNetname()==change['net'] and position(track.GetPosition())==change['old']:
                track.SetPosition(xy(*change['new']));track.SetWidth(mm(change['size']));track.SetDrill(mm(change['drill']))
    for part in design['parts']:
        fp=fps[part['ref']]
        fp.SetPosition(xy(*part['pos']))
        if fp.IsFlipped()!=(part['side']=='bottom'):fp.Flip(fp.GetPosition(),False)
        fp.SetOrientationDegrees(part['rot'])
    for route in json.loads((ROOT/'agent/validation/finish-routes.json').read_text()):
        for start,end in zip(route['points'],route['points'][1:]):
            if start==end:continue
            if start[2]!=end[2]:
                if not any(isinstance(t,p.PCB_VIA) and t.GetNetname()==route['net'] and position(t.GetPosition())==start[:2] for t in b.GetTracks()):via(route['net'],start[:2],route.get('via_size',.6),route.get('via_drill',.3))
            else:wire(route['net'],[start[:2],end[:2]],route['width'],p.F_Cu if start[2]==0 else p.B_Cu)
    for pt in json.loads((ROOT/'agent/validation/ground-vias.json').read_text()):via('GND',pt,.5,.25)
    for track in b.GetTracks():
        if not isinstance(track,p.PCB_VIA) and track.GetWidth()<mm(.15):
            track.SetWidth(mm(.15))
    # Session files can repeat a via where independently completed paths meet.
    seen_vias=set()
    for track in list(b.GetTracks()):
        if not isinstance(track,p.PCB_VIA):continue
        key=(track.GetNetname(),*position(track.GetPosition()))
        if key in seen_vias:b.RemoveNative(track)
        else:seen_vias.add(key)
    apply_board_identity(b)
    # Ground on both layers; no per-net clearances are weakened by the pours.
    for layer in [p.F_Cu,p.B_Cu]:
        z=p.ZONE(b);z.SetLayer(layer);z.SetNetCode(nets['GND']);z.SetLocalClearance(mm(.2));z.SetThermalReliefGap(mm(.25));z.SetThermalReliefSpokeWidth(mm(.3));z.SetPadConnection(p.ZONE_CONNECTION_THERMAL);z.SetMinThickness(mm(.2))
        outline=z.Outline();outline.NewOutline()
        for x,y in [(.3,.3),(47.7,.3),(47.7,27.7),(.3,27.7)]:outline.Append(int(mm(x)),int(mm(y)))
        b.Add(z)
    # Solid ground connections avoid isolated thermal islands at edge connectors
    # and maximize regulator EP heat spreading. Other pads retain thermals.
    for footprint in fps.values():
        for pad in footprint.Pads():
            if pad.GetNetname()=='GND':pad.SetLocalZoneConnection(p.ZONE_CONNECTION_FULL)
    b.BuildConnectivity();p.ZONE_FILLER(b).Fill(b.Zones())
    p.SaveBoard(str(ROOT/'saihub.kicad_pcb'),b)
    (ROOT/'saihub.kicad_pro').write_text(project)
    print('Imported routing and filled both ground planes.')
else: raise SystemExit('Expected prepare or import')
