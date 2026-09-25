#!/usr/bin/env python3
"""Compile the offline interactive BOM using openscopeproject/InteractiveHtmlBom.

Set INTERACTIVE_HTML_BOM to its generate_interactive_bom.py entry point.
Validated with upstream commit 5c192e794cd66fde04bab11810601b711bf8581b.
"""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PYTHON = os.environ.get('KICAD_PYTHON', '/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3')


def main():
    generator = os.environ.get('INTERACTIVE_HTML_BOM')
    if not generator or not Path(generator).is_file():
        raise SystemExit('Set INTERACTIVE_HTML_BOM to InteractiveHtmlBom/generate_interactive_bom.py before exporting.')
    subprocess.run([
        PYTHON, generator, str(ROOT/'saihub.kicad_pcb'), '--no-browser',
        '--dest-dir', str(ROOT/'docs'), '--name-format', 'bom',
        '--include-tracks', '--include-nets', '--show-silkscreen', '--show-fabrication',
        '--layer-view', 'F', '--extra-fields', 'MPN', '--blacklist', 'TP*',
    ], check=True)


if __name__ == '__main__':
    main()
