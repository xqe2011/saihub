---
name: kicad-workflow
description: Modify, regenerate, route and validate the Saihub KiCad hardware using its project-local scripts. Use for this hardware project, including refreshing its review documents.
---

# Saihub KiCad workflow

Paths below are relative to `hardware/`. Read `README.md` and [design notes](../../docs/design-notes.md) before changes; use [prototype tests](../../docs/prototype-test.md) for bench work. Read `docs/version-log.md` and `agent/identity.json`. Change the revision or date version only when the user explicitly permits it, and log each approved version change in `docs/version-log.md`. Regeneration, routing, export and ordinary edits must preserve the approved identity.

## Preserve the editable design

The main `saihub.kicad_pcb` is the routed board. Keep the project, schematic, library tables and `libraries/` together at the hardware root. `agent/design.json` records parts, placement and connectivity; the generator's source contains the placement definitions that produce it. Reconcile direct KiCad edits with that metadata before exporting a BOM or running the metadata synchronizer.

`agent/scripts/generate.py` regenerates symbols, footprints, schematic and `agent/routing/saihub-unrouted.kicad_pcb`. It preserves existing project settings and does not overwrite the main routed board. Do not run it merely to refresh previews. For a placement-only request, stop before routing and report unconnected items honestly.

Use KiCad's bundled Python for `pcbnew` scripts. On macOS it is normally `/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3`. The generator uses `wx.App(False)` and may require desktop access. Use project-local footprints and inspect manufacturer land patterns when changing parts; similarly named packages may have different pad mappings.

## Routing and checks

Run `agent/scripts/route.py prepare` with KiCad Python, route `agent/validation/saihub.dsn` in Freerouting to `agent/validation/saihub.ses`, then run `route.py import`. The script preserves critical power/USB and protection routes, replaces redundant autorouter ground tracks with filled planes, and applies `agent/validation/finish-routes.json` before ground filling. Fixed coordinates and saved SES apply only to their original placement; revise them when placement changes. The script currently guards Rev A, 48 x 28 mm; that guard alone does not prove placement compatibility.

From the repository root, run `python3 hardware/agent/scripts/export_placement.py --routed`. Set `KICAD_CLI` and `KICAD_PYTHON` if needed. The host Python needs `reportlab`, and ImageMagick must be on PATH. Set `INTERACTIVE_HTML_BOM` to the upstream `InteractiveHtmlBom/generate_interactive_bom.py` script; document exports also compile the offline `docs/bom.html`. The export first writes a netlist, synchronizes symbol UUIDs and explicit NC nets, fills zones, then runs ERC and DRC with schematic parity. Routed acceptance requires zero violations, parity findings and unconnected items. Without `--routed`, unconnected items are allowed for placement review only.

Inspect the top and mirrored bottom pages of `docs/pcb.pdf` and the schematic PDF after exporting. KiCad SVG contains invisible searchable text; strip those text nodes before ImageMagick rasterization to avoid duplicated labels. `export_docs.py` performs that conversion and refreshes the procurement BOM, excluding test pads and including the external antenna. `agent/validation/parts-placement.csv` retains all footprints for placement review.

## Retained evidence

Keep current `agent/validation/` and routing reconstruction inputs in Git. Do not retain obsolete revision archives or documents. Saved reports are evidence of a particular CAD state, not a substitute for rerunning checks. `agent/previews/` is regenerable and ignored; the four human review files live in `docs/`.

Only run `agent/scripts/export.py` when fabrication exports are needed. CAD checks do not establish measured power, thermal or USB performance; use the prototype test procedure for those claims.

`silkscreen.py` places all footprint references on front silkscreen, avoiding exposed pads and other labels. Generation and board synchronization both apply it. Keep the approved date in PCB title metadata for the interactive BOM.
