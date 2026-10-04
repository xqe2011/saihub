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

For the four-indicator layout, run `agent/scripts/replay_routing.py replay` with KiCad Python after regeneration. It restores `agent/routing/copper.json` only when every footprint, pad position and net matches the captured placement. For a new placement, route the edited main board, then capture its copper with `replay_routing.py capture` and run the full routed checks before accepting the snapshot. The legacy `route.py`/Specctra inputs predate the indicator placement and are guarded against reuse with it.

From the repository root, run `python3 hardware/agent/scripts/export_placement.py --routed`. Set `KICAD_CLI` and `KICAD_PYTHON` if needed. The host Python needs `reportlab`, and ImageMagick must be on PATH. Set `INTERACTIVE_HTML_BOM` to the upstream `InteractiveHtmlBom/generate_interactive_bom.py` script; document exports also compile the offline `docs/bom.html`. The export first writes a netlist, synchronizes symbol UUIDs and explicit NC nets, fills zones, then runs ERC and DRC with schematic parity. Routed acceptance requires zero violations, parity findings and unconnected items. Without `--routed`, unconnected items are allowed for placement review only.

Inspect the top and mirrored bottom pages of `docs/pcb.pdf` and the schematic PDF after exporting. KiCad SVG contains invisible searchable text; strip those text nodes before ImageMagick rasterization to avoid duplicated labels. `export_docs.py` performs that conversion and refreshes the procurement BOM, excluding test pads; the current antenna is built into the module. `agent/validation/parts-placement.csv` retains all footprints for placement review.

## Retained evidence

Keep current `agent/validation/` and routing reconstruction inputs in Git. Do not retain obsolete revision archives or documents. Saved reports are evidence of a particular CAD state, not a substitute for rerunning checks. `agent/previews/` is regenerable and ignored; the five human review files live in `docs/`, including `docs/shell.pdf`.

## Keep the enclosure synchronized

After any change to the routed PCB, refresh the enclosure reference and re-render **`hardware/docs/shell.pdf`** (repository-relative); never write the shell PDF to the repository-level `docs/` or `hardware/shell.pdf`. `export_docs.py`, and therefore the placement and fabrication export workflows, calls `mechanical/refresh_shell.py` automatically. Install `mechanical/requirements.txt` in `mechanical/.venv`, or set `MECHANICAL_PYTHON` to a Python environment with those dependencies. Shell refresh failures must fail the export; do not silently retain an old PDF as current.

Before exporting a changed board, compare its outline, thickness, connector locations/heights and component keep-outs with `mechanical/enclosure.py`. Update the shell's parameters, openings, supports, text alignment and illustrative component envelopes when those inputs change. The model contains explicit dimensions; re-exporting the PCB alone does not adapt them. Read [the mechanical guide](../../../mechanical/README.md) for design constraints and refresh commands.

The refresh command exports `mechanical/reference/pcb.step` from the routed board, regenerates STEP/STL and previews, checks available component and snap insertion geometry, then recomputes drawing views, dimensions and the ABS mass estimate. Preserve the current design choices: closed top, recessed lettering, blank QR bed, side snap retention, and no thin bridge above the GPIO opening. These are current project requirements, not rules for unrelated hardware.

Render the resulting PDF to page images and inspect every page for clipping, label alignment, connector access and drawing consistency. Missing/illustrative component models and untested snap forces remain explicit limitations; do not describe a geometry-only check as a physical fit test. Preserve the approved board revision/date throughout this refresh.

Only run `agent/scripts/export.py` when fabrication exports are needed. CAD checks do not establish measured power, thermal or USB performance; use the prototype test procedure for those claims.

`silkscreen.py` places references on the same side as each footprint, including the eight back-side resistors and test pad, avoiding exposed pads and other labels. Generation and board synchronization both apply it. Keep the approved date in PCB title metadata for the interactive BOM.
