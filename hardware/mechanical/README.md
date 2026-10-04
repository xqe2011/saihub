# SAIHub-Mini enclosure

Parametric **build123d** base and cover for the current 48 × 28 × 1.6 mm board.
The cover uses a monochrome layout with a left-aligned Helvetica wordmark,
smaller MINI model line, and a blank QR sticker bed in the frame at upper right.
The top remains continuous over USB and the button, with four indicator openings nearby. USB and button access is through the side only
and no USB/button text labels.
All 12 connector labels share a baseline along the connector edge.
Each connector label is rotated 90° and centred on its physical 2.54 mm pin pitch,
so power and ground labels use the same column width as the numbers. All dimensions are in mm.

## Generate

From this directory, using Python 3.12:

```sh
python3.12 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python enclosure.py --preview
```

`output/` contains separate `base.step`, `cover.step`, `base.stl`, `cover.stl`,
an assembled `assembly.step`, a `fit-reference.step` with the populated board reference,
`validation.json`, and `preview.png`. The STEP and STL parts retain assembled
coordinates. Generate with `--output PATH` to select another export directory.

## Geometry

- Overall assembled envelope: **50.4 × 30.4 × 9 mm**.
- Ordinary side walls, floor, and cover face: **1 mm**.
- Exterior plan corners: **1.5 mm radius**, with matching nested profiles and a
  **0.3 mm** softened top perimeter. Wall dimensions describe straight sections;
  the top edge blend locally rounds the nominal roof thickness.
- The compact nesting joint uses a **1 mm cover lip** inside the **1 mm base
  wall**, with **0.2 mm** sliding clearance per side. It starts at **z = 6.5 mm**,
  above the board, with local edge-component clearance checked, instead of
  adding width beside the board.
- Lip overlap: **1 mm**; cover seats at **z = 7.5 mm**. Two opposed side snaps
  provide vertical retention with **0.25 mm** engagement and **0.2 mm** vertical
  play. Each free arm is **7.5 × 0.6 × 1.2 mm** with a ramped hook and **0.4 mm**
  clearance above it. Press the hooks through both side windows to release.
  Snap force, fatigue and print tolerances require prototype testing.
- Board side clearance: **0.2 mm**. The board rests **1.8 mm** above the floor for
  solder tails. Four short edge ledges support it, surrounding walls locate it,
  and ceiling fingers limit lift to **0.3 mm**. The board notch has no support below it.
- The footprint follows `48 + 2 × (1 + 0.2)` by `28 + 2 × (1 + 0.2)`.
  The smaller **1.5 mm** outer corner radius preserves clearance around the square
  board corners. The face layout is inset to suit the smaller shell.
- Component space above the PCB: **3.6 mm** to the continuous roof, apart from
  the nesting lip and edge fingers. The available USB model is taller than the
  stated 2.54 mm component height; its top has **0.295 mm** roof clearance.
  Body height is **9 mm**; the QR outline is recessed **0.3 mm**. The top is closed except for the four indicator openings,
  with **1 mm** nominal roof thickness and **0.7 mm** below recessed lettering.
- Bottom allowance is based on the available connector model: its tails extend
  about **1.405 mm** below the board, leaving approximately **0.395 mm** to the
  floor. Verify actual solder joints and trim tails if necessary.
- QR intaglio outline: **22.36 × 22.36 mm outside**, **22 × 22 mm square inside**, **0.18 mm wide
  along the straight edges and recessed 0.3 mm**, with rounded outer corners and
  **0.7 mm** of roof remaining below the groove. The border width matches the
  approximately 0.177 mm stems of the 1.8 mm regular lettering.
  The sticker bed is flat, flush with the cover face, and has no text. All labels are **recessed 0.3 mm**, leaving **0.7 mm** of roof below the lettering.

## Board alignment and prototype checks

Dimensions and connector locations come from `../saihub.kicad_pcb` and
`../agent/validation/parts-placement.csv`. The board is rotated in the face view
so that USB is on the left: board `(x, y)` becomes model `(48 - x, y)`.
The front connector edge is model `y = 0`; the base bottom is `z = 0`.
USB and button side-opening centres are **6.4 mm** and **14.7 mm** from the
left exterior edge. Their openings are **10.4 × 4.2 mm** and **5 × 3 mm**
respectively, with internal relief for the connector bodies. Neither opening
pierces the roof. The 12-contact header side opening is **31.5 × 3.9 mm**,
extending to the seam so no fragile strip remains above it on the base.
The pin labels remain recessed into the continuous roof above it.
The labels follow actual connector positions. Reading left to right in the cover
face view: **8, 7, 6, 5, 4, 3, 2, 1, GND, 5V, GND, 3.3V**. Channels **1–8**
correspond to the board's **IO0–IO7**; they are channel labels, not physical pin
numbers. Power and ground use the same 90° rotation as the numeric labels.

The generator checks solid validity, connectedness, shell-to-shell interference,
and interference with the actual notched board envelope. The exploded preview
also shows the populated board between the shell halves, in the same XY alignment.
`reference/pcb.step` is an export of the committed board with pads and available
component models. The button and module use explicitly named simplified envelopes
because their footprints have no supplied models; their heights are illustrative.
Other missing component models and solder are not reconstructed. This visual
reference is not a complete component-clearance certification. Before production, print a fit
sample and verify component heights, solder tails, edge finger clearance,
cable overmould access, mating-header clearance, button access, lid
retention, and wireless operation. Use nonconductive plastic.

Print the base floor down. The cover has recessed details on its flat top face and lips/snaps underneath;
choose supports and orientation in the slicer to preserve the engraved details. Start with a fine layer height for the small
labels. The preview is rendered directly from the generated solids in neutral
grey, with darker recessed lettering and frame for legibility; it does not specify separate print colours.

To refresh the board reference after board edits, from this directory:

```sh
kicad-cli pcb export step --force --subst-models --include-pads --user-origin 0x0mm \
  -o reference/pcb.step ../saihub.kicad_pcb
.venv/bin/python enclosure.py --preview
```

The exporter uses board-bottom `z=0` and negated board Y. The generator rotates
it 180° around Z and translates it to `(48, 0, 2.8)` to match the enclosure.

Build123d export API reference:
[Import/Export](https://build123d.readthedocs.io/en/latest/import_export.html).

## Dimensioned PDF

The hardware review document `../docs/shell.pdf` contains exploded previews with and
without the PCB, top/front/right views with dimensions, and an ABS mass estimate.
Regenerate it after changing the enclosure:

```sh
.venv/bin/python render_shell_docs.py --output /tmp/saihub-shell-pdf
.venv/bin/python export_shell_pdf.py --assets /tmp/saihub-shell-pdf
```

The drawing uses a 3:1 orthographic scale when printed at 100%. Dimensions govern.
The PDF recomputes the ABS mass from current CAD material volumes at **1.04 g/cm3**,
using the linked manufacturer data sheet. Filled thin walls are assumed; supports,
brim, purge, electronics and sticker are excluded. Confirm consumption with the
intended slicer settings.

## Required refresh after PCB changes

The canonical PDF is **`hardware/docs/shell.pdf`**, never the repository-level
`docs/shell.pdf` or `hardware/shell.pdf`. PCB edits require checking the explicit
board dimensions, connector centres/heights, supports, snap paths and illustrative
component envelopes in `enclosure.py`; these do not adapt automatically to a new
board layout. Update drawing callouts if the geometry changes.

After reconciling those inputs, from this directory run:

```sh
.venv/bin/python refresh_shell.py
```

This exports a fresh board reference, regenerates the enclosure exports/preview,
checks board/component and snap insertion geometry, regenerates all drawing views,
and recomputes the mass in `../docs/shell.pdf`. A failure stops the refresh instead
of publishing a new PDF. `--pdf-python /path/to/python` may select a separate
Python environment with ReportLab. `KICAD_CLI` selects the board export executable.
The hardware document exporter invokes this command automatically; select its
CAD environment with `MECHANICAL_PYTHON` if not using `mechanical/.venv`.

Render every page of the resulting PDF and visually inspect the images before
reporting completion. Geometry checks do not validate missing models, solder,
material strength or printed snap retention. Do not change board revision/date
metadata just to refresh these documents.


## Indicator bank

The four top-side LEDs remain together at board x=30.3 mm, y=5.2/8.4/11.6/14.8 mm.
The shell takes these positions directly from `agent/design.json`. Each has a
1.5 mm circular viewing opening and a separate 2.2 mm outside-diameter light well,
with 0.4 mm clearance above the nominal 0.55 mm LED body. Recessed labels read
`3V3 OUT`, `5V OUT`, `5V IN`, and `STATUS`; the first two are green, then red and blue.
The QR sticker area remains blank. The eight bottom resistors fit inside the
existing solder-tail allowance. Verify visibility, light bleed and resistor/solder
clearance on a physical prototype.
