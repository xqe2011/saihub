# SAIHub-Mini — Rev A

Version: **2026-09-24**. Author: **GPT6-Astra**. Co-author: **xqe2011**.

The date version changes only with explicit user permission. See [the version log](docs/version-log.md); regeneration and exports preserve the approved version.

**Current state: fully routed and CAD-validated.** The updated schematic and PCB include U6/U7 header protection (TPD4E05U06DQAR), U10/C21 power-pin protection, TPS2553DDBVR output switches, and shared MDD SS34 parts for D1/D3. Both output fault nets reach separate controller inputs with test pads retained; R13/R14 are removed. Firmware must enable internal pullups before monitoring faults; that firmware work remains pending. The fast fuse and DC/DC circuit are retained. ERC, DRC, schematic parity and connectivity checks all report zero findings.

Editable KiCad 10 design for the ESP32-C5 GPIO controller. Open `saihub.kicad_pro`; symbols and footprints are project-local. The schematic is one A2 page with seven functional sections.

The PCB is **48 × 28 mm**, with top-only component assembly. Across the front edge, left to right: **12-pin header → small BOOT button → USB-C**. The width follows these three footprints plus their assembly clearances.

SW1 is a **Panasonic EVQP7A01P**, with a 3.5 × 2.9 mm body, 1.35 mm mounting height and side actuator facing the connector edge. Its actuator adds to the body depth. The 5 V / 3 A input, SGM6232 supply, nominal 1 A output protection, ESP module, GPIO mapping and 5020 buzzer are part of this design.

**The PCB remains 48 × 28 mm and is fully routed on two copper layers.** All 57 footprints remain on top. Both layers have filled ground copper; redundant autorouter ground traces have been removed so the pours provide the general ground connections. Short protection and regulator ground connections and stitching vias are retained. The header arrays fit after moving the main module 1.95 mm toward the rear and rearranging nearby passives and test pads. Connector pins and the switch actuator project beyond the outline; PCB dimensions are not the full assembled enclosure envelope.

## Files

- `saihub.kicad_pro`, `saihub.kicad_sch`, `saihub.kicad_pcb`: open the project in KiCad 10 to edit the current design.
- `libraries/`, `sym-lib-table`, `fp-lib-table`: project-local symbols and footprints required by KiCad.
- `docs/bom.csv`: current procurement BOM; excludes test pads and includes the external antenna.
- `docs/bom.html`: self-contained interactive assembly BOM from [InteractiveHtmlBom](https://github.com/openscopeproject/InteractiveHtmlBom), with component lookup, MPNs, copper and net highlighting. Test pads are visible on the board but excluded from the BOM table; use `bom.csv` for the external antenna.
- `docs/schematic.pdf`: schematic review.
- `docs/pcb.pdf`: top and mirrored bottom PCB previews, including silkscreen designators.
- `docs/version-log.md`: approved version and permission policy.
- `agent/`: regeneration scripts, design metadata, design notes, validation evidence, routing intermediates and reusable workflow skills.

Current validation and routing inputs are retained in Git. `agent/previews/` contains regenerable images and is ignored; the review PDFs are committed. KiCad local history, locks, backups and personal settings are also ignored.

**Prototype status:** routed CAD checks passed; the board has not been fabricated or bench tested. Verify thermal behavior, supply loading and USB operation using `agent/docs/prototype-test.md`. The 26.1 kΩ ILIM resistors set approximately 0.989 A nominal, with 0.908–1.081 A bounds including resistor tolerance; a guaranteed 1 A continuous rating is not implied.

## Connections

J2 pin 1 is square and marked `3V3`. Looking at the top with the connector edge at the top, pins run left to right:

`1:3V3_SW  2:GND  3:5V_SW  4:GND  5:IO0  6:IO1  7:IO2  8:IO3  9:IO4  10:IO5  11:IO6  12:IO7`

IO0-IO7 map to GPIO **10, 1, 0, 23, 4, 5, 6, 24**. Logic pins are **3.3 V only**; the 1 A setting applies to the two power channels. GPIO8 enables 3.3 V output and GPIO9 enables 5 V output. Both default off. Do not back-power either rail. The 5 V output follows USB voltage minus fuse, trace and switch losses.

GPIO12 drives the passive 5020 buzzer at 4 kHz / 50% duty. GPIO13/14 carry USB D-/D+. BOOT is GPIO28. TP1/TP2 are EN/GND reset pads on the top: briefly short them to reset. Hold BOOT during reset/power-on for download recovery. TP3 is UART TX, TP4 always-on 3.3 V, and TP5/TP6 active-low switch fault outputs.

Use a specified **5 V / 3 A adapter and cable** for external loads. Keep outputs off on unqualified PC ports. No PD, OTG or source-current detection is implemented. An external dual-band U.FL antenna is required. See `agent/docs/design-notes.md` for SGM6232 input-voltage margin and sourcing status.

## Rebuilding

Normal edits can be made in KiCad. From the repository root, run:

```sh
export INTERACTIVE_HTML_BOM=/path/to/InteractiveHtmlBom/InteractiveHtmlBom/generate_interactive_bom.py
python3 hardware/agent/scripts/export_placement.py --routed
```

The command synchronizes board metadata, requires zero ERC/DRC/parity findings and zero unconnected items, then refreshes the four files in `docs/`. It requires KiCad 10, ImageMagick and Python `reportlab` (`python3 -m pip install reportlab`). Set `KICAD_CLI` and `KICAD_PYTHON` to override the macOS tool defaults; the latter must provide `pcbnew`. Poppler is optional for the separate schematic PNG. Clone [InteractiveHtmlBom](https://github.com/openscopeproject/InteractiveHtmlBom) and set `INTERACTIVE_HTML_BOM` as above (validated at commit `5c192e794cd66fde04bab11810601b711bf8581b`). On macOS, its generator requires a logged-in desktop session. `agent/scripts/export_ibom.py` can refresh only `docs/bom.html`.

`agent/scripts/export_docs.py` refreshes only the review documents, without changing or validating CAD. `agent/scripts/export.py` separately validates and exports fabrication files into `manufacturing/`.

For regeneration and routing, read [the project workflow skill](agent/skills/kicad-workflow/SKILL.md). `generate.py` writes the separate `agent/routing/saihub-unrouted.kicad_pcb`; it preserves the main routed PCB but regenerates the schematic and libraries while preserving existing project settings. Existing SES routing is tied to its exact placement and fixed routes.
