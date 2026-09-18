#!/usr/bin/env python3
"""Export review and prototype manufacturing files from the checked routed board."""
import csv,json,os,shutil,subprocess,zipfile,hashlib
from pathlib import Path
from identity import identity
R=Path(__file__).resolve().parents[2]
for directory in ('agent/validation', 'agent/previews', 'agent/routing', 'docs'):
    (R/directory).mkdir(parents=True, exist_ok=True)
cli=os.environ.get('KICAD_CLI',shutil.which('kicad-cli') or '/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli')
def run(*args):subprocess.run([cli,*map(str,args)],check=True)
info=identity()
archive_name=f"{info['name']}-Rev{info['revision']}-{info['version']}-gerbers.zip"
b=R/'saihub.kicad_pcb';s=R/'saihub.kicad_sch';m=R/'manufacturing';g=m/'gerbers';g.mkdir(parents=True,exist_ok=True)
run('sch','erc','--exit-code-violations','-o',R/'agent/validation/erc.rpt',s)
run('pcb','drc','--schematic-parity','--exit-code-violations','-o',R/'agent/validation/drc.rpt',b)
run('sch','export','pdf','-o',R/'docs/schematic.pdf',s)
run('pcb','export','gerbers','-o',str(g)+'/', '-l','F.Cu,B.Cu,F.Paste,B.Paste,F.Mask,B.Mask,F.SilkS,B.SilkS,Edge.Cuts',b)
run('pcb','export','drill','-o',str(g)+'/', '--excellon-separate-th','--generate-report',b)
run('pcb','export','pos','--format','csv','--units','mm','--exclude-dnp','-o',m/'placement.csv',b)
for side,layers in [('top','F.Cu,F.SilkS,F.Fab,Edge.Cuts'),('bottom','B.Cu,B.SilkS,B.Fab,Edge.Cuts')]:
 args=['pcb','export','svg','--mode-single','--fit-page-to-board','--exclude-drawing-sheet','-l',layers,'-o',R/f'agent/previews/board-{side}.svg']
 if side=='bottom':args+=['--mirror']
 run(*args,b)
run('pcb','export','svg','--mode-single','--fit-page-to-board','--exclude-drawing-sheet','-l','F.Fab,Edge.Cuts','-o',R/'agent/previews/assembly.svg',b)
run('pcb','export','svg','--mode-single','--fit-page-to-board','--exclude-drawing-sheet','--mirror','-l','B.Fab,Edge.Cuts','-o',R/'agent/previews/assembly-bottom.svg',b)
parts=json.loads((R/'agent/design.json').read_text())['parts']
with (m/'bom.csv').open('w',newline='') as f:
 w=csv.writer(f);w.writerow(['Reference','Quantity','Value','MPN_or_procurement_spec','Footprint','Notes','Datasheet'])
 for a in parts:
  if a['ref'].startswith('TP'):continue
  mpn=a['mpn']
  if a['sym']=='R':mpn=f"Generic {a['value']} ohm, 1%, 0603, >=0.1W"
  elif a['sym']=='C':mpn=f"Generic {a['value']}F, X7R, >=10V, +/-10%, "+next(size for size in ['1210','0805','0603'] if size in a['foot'])
  w.writerow([a['ref'],1,a['value'],mpn,a['foot'],a['desc']+'; '+a['side']+' assembly',a['url']])
 w.writerow(['ANT1 (off-board)',1,'Dual-band external antenna','50 ohm 2.4/5GHz U.FL-compatible antenna; qualify with final enclosure','Not PCB mounted','Required accessory; not included in placement file',''])
# Do not send PCB test pads to the assembly pick-and-place machine.
with (m/'placement.csv').open() as f:rows=list(csv.reader(f))
with (m/'placement.csv').open('w',newline='') as f:csv.writer(f).writerows([rows[0]]+[r for r in rows[1:] if not r[0].startswith('TP')])
with zipfile.ZipFile(m/archive_name,'w',zipfile.ZIP_DEFLATED) as z:
 for f in sorted(g.iterdir()):z.write(f,f.name)
with (m/'SHA256SUMS').open('w') as out:
 for f in [b,s,m/'bom.csv',m/'placement.csv',m/archive_name]:
  out.write(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+str(f.relative_to(R))+'\n')
print('Exported BOM, placement, Gerber/drill archive, schematic PDF and board SVGs.')

from export_docs import main as export_docs
export_docs()
