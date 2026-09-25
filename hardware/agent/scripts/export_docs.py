#!/usr/bin/env python3
"""Refresh human review documents from the current CAD and design metadata.

Requires KiCad CLI, ImageMagick and the Python reportlab package.
Run export_placement.py --routed first when validating a release.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from identity import identity
from export_bom import write_bom
from export_ibom import main as export_ibom

from reportlab.lib.pagesizes import landscape, A4
from reportlab.lib.utils import ImageReader
from reportlab.pdfgen import canvas

ROOT = Path(__file__).resolve().parents[2]
CLI = os.environ.get('KICAD_CLI', shutil.which('kicad-cli') or '/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli')

def main():
    docs = ROOT/'docs'
    previews = ROOT/'agent/previews'
    docs.mkdir(exist_ok=True)
    previews.mkdir(parents=True, exist_ok=True)
    data = json.loads((ROOT/'agent/design.json').read_text())
    info = identity()
    data.update(info)
    subprocess.run([CLI, 'sch', 'export', 'pdf', '-o', str(docs/'schematic.pdf'), str(ROOT/'saihub.kicad_sch')], check=True)
    pdf = canvas.Canvas(str(docs/'pcb.pdf'), pagesize=landscape(A4))
    pdf.setTitle(f"{data['name']} Rev {data['revision']} / {data['version']} - PCB preview")
    width, height = landscape(A4)
    for side, layers in [('top', 'F.Cu,F.SilkS,F.Fab,Edge.Cuts'), ('bottom', 'B.Cu,B.SilkS,B.Fab,Edge.Cuts')]:
        svg = previews/f'board-{side}.svg'
        args = [CLI, 'pcb', 'export', 'svg', '--mode-single', '--fit-page-to-board', '--exclude-drawing-sheet', '-l', layers, '-o', str(svg)]
        if side == 'bottom':
            args.append('--mirror')
        subprocess.run(args + [str(ROOT/'saihub.kicad_pcb')], check=True)
        # KiCad includes invisible searchable text alongside visible stroke paths.
        drawing = re.sub(r'<text\b[^>]*>.*?</text>', '', svg.read_text(), flags=re.S)
        png = svg.with_suffix('.png')
        subprocess.run(['magick', '-background', 'white', '-density', '1000', 'svg:-', '-resize', '2400x', str(png)], input=drawing.encode(), check=True)
        pdf.setFont('Helvetica-Bold', 20)
        pdf.drawString(36, height-42, f"{data['name']} Rev {data['revision']} / {data['version']} | PCB {side}")
        pdf.setFont('Helvetica', 10)
        dimensions = ' x '.join(f'{v:g}' for v in data['board_mm'])
        pdf.drawString(36, height-61, f'{dimensions} mm board | Copper, fabrication outlines and silkscreen' + (' | Mirrored bottom view' if side == 'bottom' else ''))
        img = ImageReader(str(png))
        iw, ih = img.getSize()
        scale = min((width-72)/iw, (height-126)/ih)
        w, h = iw*scale, ih*scale
        pdf.drawImage(img, (width-w)/2, 46+(height-126-h)/2, w, h)
        pdf.setFont('Helvetica', 9)
        pdf.drawRightString(width-36, height-78, f"Author: {info['author']} | Co-author: {info['co_author']}")
        pdf.drawString(36, 23, 'Review preview - not to scale. Use the editable KiCad project for dimensions and fabrication.')
        pdf.showPage()
    pdf.save()
    write_bom(docs/'bom.csv', data['parts'])
    export_ibom()
    print('Updated docs/bom.csv, docs/bom.html, docs/schematic.pdf and docs/pcb.pdf')

if __name__ == '__main__':
    main()
