#!/usr/bin/env python3
"""Prepare locked critical routes / import Specctra results / fill ground copper.

Run with KiCad's bundled Python. Freerouting runs locally between prepare/import.
"""
import json
import sys
from pathlib import Path
import pcbnew as p
from identity import identity, apply_board_identity
ROOT=Path(__file__).resolve().parents[2]
for directory in ('agent/validation', 'agent/previews', 'agent/routing', 'docs'):
    (ROOT/directory).mkdir(parents=True, exist_ok=True)
design=json.loads((ROOT/'agent/design.json').read_text())
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
    for pt in [(14.3,11.3),(16.3,11.3),(14.3,13.3),(16.3,13.3)]:via('GND',pt,.6,.3)
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
    # Reserve the long USB pair on B.Cu before routing GPIOs.
    wire('MCU_DM',[point('U3',13),(4.6,20.43),(4.6,18.5),point('R3',2)],.2)
    wire('MCU_DP',[point('U3',14),(4.325,21.7),(3.325,20.7),point('R4',2)],.2)
    for net,ref,pt in [('USB_DM','R3',(1.15,18.5)),('USB_DP','R4',(1.15,20.3))]:
        wire(net,[point(ref,1),pt],.2);via(net,pt)
    via('USB_DM',(31.65,7.05));via('USB_DP',(35.3,6.84))
    wire('USB_DM',[(1.15,18.5),(1.15,19.3),(4.95,23.1),(31.6,23.1),(33.35,21.35),(33.35,9.4),(31.65,7.7),(31.65,7.05)],.2,p.B_Cu)
    wire('USB_DP',[(1.15,20.3),(4.35,23.5),(31.8,23.5),(33.75,21.55),(33.75,9.2),(35.3,7.65),(35.3,6.84)],.2,p.B_Cu)
    wire('USB_DM',[(31.65,7.05),(32.3375,7.05),point('U1',1)],.2)
    wire('USB_DP',[(35.3,6.84),point('U1',3)],.2)
    out=ROOT/'agent/routing/saihub-prerouted.kicad_pcb'
    p.SaveBoard(str(out),b)
    (ROOT/'agent/routing/saihub-prerouted.kicad_pro').write_text(project)
    p.ExportSpecctraDSN(b,str(ROOT/'agent/validation/saihub.dsn'))
    print('Prepared SAIHub-Mini locked buck, current-limit and USB routes.')
elif mode=='import':
    if not p.ImportSpecctraSES(b,str(ROOT/'agent/validation/saihub.ses')): raise SystemExit('SES import failed')
    finish=ROOT/'agent/validation/finish-routes.json'
    for route in json.loads(finish.read_text()) if finish.exists() else []:
        for a,c in zip(route['points'],route['points'][1:]):
            if a[2]!=c[2]:via(route['net'],a[:2],route.get('via_size',.6),route.get('via_drill',.3))
            else:wire(route['net'],[a[:2],c[:2]],route['width'],p.F_Cu if a[2]==0 else p.B_Cu)
    # Enforce the board minimum after the router's automatic neck-downs.
    for track in b.GetTracks():
        if not isinstance(track,p.PCB_VIA) and track.GetWidth()<mm(.15):
            track.SetWidth(mm(.15))
        # Move the router's tiny U1 ground neck away from the USB pad edge.
        if not isinstance(track,p.PCB_VIA) and track.GetLayer()==p.F_Cu and track.GetNetname()=='GND':
            for get,setter in [(track.GetStart,track.SetStart),(track.GetEnd,track.SetEnd)]:
                pt=get();x,y=p.ToMM(pt.x),p.ToMM(pt.y)
                if 32.1<x<32.53 and 7.7<y<7.8:setter(xy(x,y+.02))
    apply_board_identity(b)
    # Ground on both layers; no per-net clearances are weakened by the pours.
    for layer in [p.F_Cu,p.B_Cu]:
        z=p.ZONE(b);z.SetLayer(layer);z.SetNetCode(nets['GND']);z.SetLocalClearance(mm(.2));z.SetThermalReliefGap(mm(.25));z.SetThermalReliefSpokeWidth(mm(.3));z.SetPadConnection(p.ZONE_CONNECTION_THERMAL);z.SetMinThickness(mm(.2))
        outline=z.Outline();outline.NewOutline()
        for x,y in [(.3,.3),(47.7,.3),(47.7,27.7),(.3,27.7)]:outline.Append(int(mm(x)),int(mm(y)))
        b.Add(z)
    # Solid ground connections avoid isolated thermal islands at edge connectors
    # and maximize regulator EP heat spreading. Other pads retain thermals.
    for ref in ['J1','J2','U2','C12']:
        for pad in fps[ref].Pads():
            if pad.GetNetname()=='GND':pad.SetLocalZoneConnection(p.ZONE_CONNECTION_FULL)
    b.BuildConnectivity();p.ZONE_FILLER(b).Fill(b.Zones())
    p.SaveBoard(str(ROOT/'saihub.kicad_pcb'),b)
    (ROOT/'saihub.kicad_pro').write_text(project)
    print('Imported routing and filled both ground planes.')
else: raise SystemExit('Expected prepare or import')
