#!/usr/bin/env python3
"""Synchronize single-page symbol metadata and NC net identities, preserving copper."""
import json, uuid, xml.etree.ElementTree as ET
from pathlib import Path
import pcbnew as p
from identity import apply_board_identity
from silkscreen import apply_reference_labels
ROOT=Path(__file__).resolve().parents[2]
for directory in ('agent/validation', 'agent/previews', 'agent/routing', 'docs'):
    (ROOT/directory).mkdir(parents=True, exist_ok=True)
project=(ROOT/'saihub.kicad_pro').read_text()
b=p.LoadBoard(str(ROOT/'saihub.kicad_pcb'))
parts={a['ref']:a for a in json.loads((ROOT/'agent/design.json').read_text())['parts']}
root=str(uuid.uuid5(uuid.UUID('8ce59343-cd0c-4971-923b-6d37131b8b35'),'root'))
xml=ET.parse(ROOT/'agent/validation/saihub-netlist.xml')
actual={(n.attrib['ref'],n.attrib['pin']):net.attrib['name'] for net in xml.findall('.//nets/net') for n in net.findall('node')}
nets={n.GetNetname():n for n in b.GetNetInfo().NetsByNetcode().values()}
for f in b.GetFootprints():
    ref=f.GetReference();a=parts[ref]
    f.SetPath(p.KIID_PATH('/'+root+'/'+a['uuid']))
    f.SetValue(a['value'])
    f.GetField(p.FIELD_T_DATASHEET).SetText(a['url'])
    field=f.GetField('MPN') if f.HasField('MPN') else p.PCB_FIELD(f,p.FIELD_T_USER,'MPN')
    field.SetText(a['mpn']);field.SetVisible(False)
    if not f.HasField('MPN'):f.Add(field)
    for pad in f.Pads():
        num=pad.GetNumber()
        if not num:continue
        name=actual[(ref,num)]
        expected=a['nets'].get(num)
        assert name==expected or expected is None and name.startswith('unconnected-'),(ref,num,expected,name)
        if name not in nets:
            nets[name]=p.NETINFO_ITEM(b,name);b.Add(nets[name])
        assert not pad.GetNetname() or pad.GetNetname()==name,(ref,num,pad.GetNetname(),name)
        pad.SetNet(nets[name])
apply_board_identity(b)
apply_reference_labels(b)
b.BuildConnectivity();p.ZONE_FILLER(b).Fill(b.Zones())
p.SaveBoard(str(ROOT/'saihub.kicad_pcb'),b)
(ROOT/'saihub.kicad_pro').write_text(project)
print('All schematic terminal nets match design; synchronized board metadata and explicit NC nets.')
