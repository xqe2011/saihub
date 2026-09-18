#!/usr/bin/env python3
"""Export review files; --routed additionally requires zero unconnected items."""
from identity import identity
import argparse
import csv
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
for directory in ('agent/validation', 'agent/previews', 'agent/routing', 'docs'):
    (ROOT/directory).mkdir(parents=True, exist_ok=True)
CLI = os.environ.get('KICAD_CLI', shutil.which('kicad-cli') or '/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli')
PYTHON = os.environ.get('KICAD_PYTHON', '/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3')
def run(*args):
    subprocess.run([CLI, *map(str,args)], check=True)

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--routed',action='store_true',help='Require complete routing and write routing reports')
options=parser.parse_args()
stage='routing' if options.routed else 'placement'
board = ROOT/'saihub.kicad_pcb'
schematic = ROOT/'saihub.kicad_sch'
run('sch','export','netlist','--format','kicadxml','-o',ROOT/'agent/validation/saihub-netlist.xml',schematic)
subprocess.run([PYTHON,str(ROOT/'agent/scripts/sync_board.py')],check=True)
run('sch','erc','--exit-code-violations','-o',ROOT/'agent/validation/erc.rpt',schematic)
run('pcb','drc','--schematic-parity','--format','json','-o',ROOT/f'agent/validation/{stage}-drc.json',board)
report=json.loads((ROOT/f'agent/validation/{stage}-drc.json').read_text())
if report['violations'] or report['schematic_parity'] or (options.routed and report['unconnected_items']):
    raise SystemExit('Resolve DRC/parity findings and, in routed mode, every unconnected item before exporting.')
run('pcb','drc','--schematic-parity','-o',ROOT/'agent/validation/drc.rpt',board)
run('sch','export','pdf','-o',ROOT/'docs/schematic.pdf',schematic)
for name,layers,mirror in [
    ('board-top','F.Cu,F.SilkS,F.Fab,Edge.Cuts',False),
    ('board-bottom','B.Cu,B.SilkS,B.Fab,Edge.Cuts',True),
    ('assembly','F.Fab,Edge.Cuts',False),
    ('assembly-bottom','B.Fab,Edge.Cuts',True),
]:
    svg=ROOT/f'agent/previews/{name}.svg'
    args=['pcb','export','svg','--mode-single','--fit-page-to-board','--exclude-drawing-sheet','-l',layers,'-o',svg]
    if mirror:args+=['--mirror']
    run(*args,board)
    if shutil.which('magick'):
        # KiCad emits invisible searchable text as well as stroke paths.
        # ImageMagick does not honor that visibility, so rasterize paths only.
        drawing=re.sub(r'<text\b[^>]*>.*?</text>','',svg.read_text(),flags=re.S)
        subprocess.run(['magick','-background','white','-density','1000','svg:-','-resize','2000x',str(svg.with_suffix('.png'))],input=drawing.encode(),check=True)
if shutil.which('pdftoppm'):
    subprocess.run(['pdftoppm','-scale-to','2400','-singlefile','-png',str(ROOT/'docs/schematic.pdf'),str(ROOT/'agent/previews/schematic')],check=True)
data=json.loads((ROOT/'agent/design.json').read_text())
with (ROOT/'agent/validation/parts-placement.csv').open('w',newline='') as f:
    w=csv.writer(f)
    w.writerow(['Reference','Value','MPN_or_spec','Footprint','X_mm','Y_mm','Rotation_deg','Side','Notes','Source'])
    for a in data['parts']:
        w.writerow([a['ref'],a['value'],a['mpn'],a['foot'],*a['pos'],a['rot'],a['side'],a['desc'],a['url']])
summary={**identity(),'board_mm':data['board_mm'],'status':'routed-cad-validated' if options.routed else 'placement-only-unrouted','drc_violations':len(report['violations']),'schematic_parity_issues':len(report['schematic_parity']),'unconnected_items':len(report['unconnected_items'])}
(ROOT/f'agent/validation/{stage}-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(f'{stage.title()} checks passed; unconnected items:',summary['unconnected_items'])

from export_docs import main as export_docs
export_docs()
