# SAIHub-Mini Rev A design review

Version: 2026-09-24. Author: GPT6-Astra. Co-author: xqe2011.
Version updates require explicit user permission; record each approved change in `../../docs/version-log.md`.

**Current layout:** module and button replacement on the unchanged 48 × 28 mm envelope. All 64 footprints, including every resistor and test pad, are on the front surface. All four LEDs retain their positions. The front-only layout is locally routed with zero ERC, DRC, schematic-parity and unconnected findings.

## Requirements and mechanical envelope

The front edge retains J2, SW1 and J1 from left to right. SW1 follows the [supplied 2X4 drawing](button-2x4.pdf), final PDF page. It retains the stepped anchors and electrical contacts, and adds two non-plated 0.75 mm holes on 1.7 mm centres. Their footprint coordinates are (-0.85, 0.05) and (0.85, 0.05) mm: the hole row is 1.3 mm ahead of the contact centres. The drawing specifies 0.5 mm locating posts; the recommended board holes are 0.75 mm. Confirm the purchased switch matches this drawing; the former no-post procurement variant is no longer specified.

The new module has a built-in antenna and a 16 × 24 mm body. The [manufacturer drawing, pages 9–10](https://atta.szlcsc.com/upload/public/pdf/source/20251016/3D572157C42EA1AA3992B58D4700E12F.pdf) defines its 22 perimeter pads and central ground pad. It is rotated 180 degrees, with the antenna at the bottom-left. The module body runs from (0.85,4) to (16.85,28) mm. The notch runs from (0,20.5) to (21.85,28) mm, opening at both outer edges. Its right edge is exactly 5 mm beyond the antenna. The connected section retains a 0.85 mm left inset to keep the specified solder lands on the PCB with copper-edge clearance. Outer bounds remain 48 × 28 mm.

Two copper layers, 1.6 mm board thickness; all components and test pads must share the front (top) surface. Back-side component placement is prohibited; bottom copper routing and through-hole pads remain allowed. TP3 moves to the front with the resistors. J1, J2, SW1, U3 and D4-D7 retain their approved positions and orientations and are locked. Other components may move to improve routing. U2 shifts 0.6 mm left to open a right-side lane for R19 and R22; R9, R11, R12 and TP3 are also repositioned. R6 moves to (22.4, 8.2) mm at 270 degrees in the space freed beside U10. Routing is complete; all ground islands carrying component pads are connected. No board-fastening holes or external antenna accessory; SW1 has two locating holes. RF operation and enclosure clearance must be qualified on a prototype.

IO0–IO7 now map to internal pins 10, 1, 0, 2, 4, 5, 6, 7. The previous assignments for IO3 and IO7 are not exposed by the replacement module. The firmware configuration changes alongside the schematic. Pin 7 can select a debug source with non-default permanent configuration; the default configuration retains USB debugging regardless of its level. Pin 2 also has a debug function. Verify startup and recovery with attached loads, and do not assume old-board firmware pin mappings are interchangeable.

## Power stage

U2 is **SGM6232YPS8G/TR**, a nonsynchronous 1.4 MHz, 2 A buck in SOIC-8 with exposed pad. The [SGMICRO datasheet](https://www.sg-micro.com/rect/assets/9fe04e94-334c-4982-964b-fc08001cd9ac/SGM6232.pdf) defines BS/IN/SW/GND/FB/COMP/EN/SS as pins 1-8, with the exposed pad grounded. EN is intentionally open for automatic startup. C15 = 100 nF provides soft start. R18 = 10 ohms and C14 = 10 nF form the bootstrap network. R15 = 33 kΩ and R16 = 10.5 kΩ set nominal output to `0.8 × (1 + 33/10.5) = 3.314 V`. C16 = 5.6 nF and R17 = 10 kΩ form the series compensation branch to ground.

The switching stage uses a **4.7 µH SRP5030TA-4R7M**, SS34 catch diode, 22 µF input ceramic C2, 100 nF local input bypass C3, and 47 µF output ceramic C4. C2/C4 use 1210 footprints; procure X7R, ≥10 V parts and check capacitance under DC bias. Nominal capacitance is not effective capacitance. Qualify loop stability and transient response with the exact selected capacitors; the vendor reference values do not establish board-level stability. C6 adds 10 µF at the ESP supply.

The [Bourns drawing](https://bourns.com/docs/product-datasheets/SRP5030TA.pdf), downloaded 2026-09-17, gives 4.6 A Irms and 6 A Isat for -4R7M, with 5.7 × 5.2 × 2.8 mm nominal body. The local footprint follows its 6.5 mm land span, 2.5 mm gap and 1.8 mm pad width. SGM6232's local footprint follows the manufacturer land recommendation: 1.91 × 0.61 mm leads, 1.27 mm pitch, 5.56 mm row spacing and 2.413 × 3.302 mm exposed pad. Four reduced paste apertures cover approximately 49% of the EP. Three 0.6/0.3 mm ground vias connect the regulator exposed pad to the bottom ground copper; the module ground pad has four. Ground vias also connect the input/output capacitors and catch diode. Review exposed-pad via tenting/plugging with the assembler.

SGMICRO currently lists SGM6232 as [not recommended for new designs](https://www.sg-micro.com/product/SGM6232). It is retained as requested; verify supply availability for production.

## Channel limits and input budget

Both TPS2553DDBVR switches retain active-high enables and 10 kΩ pulldowns. **R7/R8 = 26.1 kΩ, 1%**. Using the [TI datasheet equations](https://www.ti.com/lit/ds/symlink/tps2553d.pdf), with R in kΩ and current in mA:

- Nominal: `23950 / 26.1^0.977 = 989 mA`.
- Lower bound including resistor tolerance: `25230 / (26.1 × 1.01)^1.016 = 908 mA`.
- Upper bound including resistor tolerance: `22980 / (26.1 × 0.99)^0.94 = 1081 mA`.

This is a **nominal 1 A protection setting**, not a guaranteed 1 A continuous output and not a hard 1.000 A maximum. Units may enter current limit below 1 A. Qualify rated continuous loading below the lowest measured/guaranteed threshold, accounting for temperature. FAULT outputs now connect to separate controller inputs (FAULT4: U3 pad 17; FAULT5: U3 pad 5) and remain available at TP5/TP6. R13/R14 are removed; firmware must enable internal pullups before monitoring these inputs. Firmware monitoring is pending. There is no controlled output discharge.

F1 is a 3 A, 1206 Littelfuse 0467003.NR fuse. The fuse is fault protection, not a precise 3 A limiter. Supply and cable must be rated for 5 V / 3 A. USB-C still has separate 5.1 kΩ CC pulldowns and has no PD negotiation or source-current detection. External loads must remain disabled on unqualified PC USB supplies; the board cannot identify available source current.

Illustrative budget at the 1.081 A upper channel limits: reserve 0.6 A for ESP peak demand and 0.11 A for the buzzer. Buck demand is then 1.791 A, below its nominal 2 A rating. At 4.75 V and an assumed 80% conversion efficiency, total input is approximately `1.081 + 3.314 × 1.791 / (4.75 × 0.80) = 2.64 A`, before small control losses. These reservations and efficiency are design assumptions, not measured performance.

SGM6232 specifies 4.5 V minimum input and 80% maximum duty cycle. Cable/fuse losses can therefore still compromise 3.3 V at high load. Aim for **at least 4.75 V at U2 IN under load** and verify the actual dropout margin, startup and heat. The SMF5.0A TVS is not a sustained overvoltage disconnect. Input ceramic capacitance still requires hot-plug/inrush verification.

## Interfaces and assembly details

The connector, header, reset pads and buzzer circuit are retained. The module now uses a built-in PCB antenna. User pins are 3.3 V only. Neither switched output may be back-powered.

BZ1 follows the user's supplied 5020 specification and terminal arrangement, documented below. D3 uses the shared MDD SS34 flyback part and Q1 is AO3400A; do not drive the coil directly from a control pin.

Native USB retains the 22 ohm series resistors and connector-side protection. These connections are rerouted for the front-only support-component placement. The design is not impedance-qualified. Validate enumeration, flashing and traffic in both plug orientations.

## Header protection

U6/U7 use two [TPD4E05U06DQAR](https://www.ti.com/lit/ds/symlink/tpd4e05u06.pdf) four-channel arrays for IO0-IO7. Pins 1, 2, 4 and 5 protect the four signals; pins 3 and 8 connect to ground. Pins 6, 7, 9 and 10 are internally unconnected and explicitly left open. These are not duplicate signal pins. The ground-referenced arrays need no supply or local bypass capacitor. They replace four two-channel arrays and C17-C20: GPIO protection goes from eight added components to two.

Each DQA body is 2.5 x 1.0 mm. The project-local footprint follows DQA0010A recommended lands (0.565 x 0.2 mm signals, 0.565 x 0.4 mm grounds, 0.5 mm pitch, 0.835 mm row-center spacing). Check the supplied package revision and stencil against the manufacturer's drawing before assembly; the datasheet also contains DQA0010B. The 1.9 x 3.0 mm courtyards pass the final placement checks. U6/U7 remain beside the header. U6 has a routed ground return to the header ground; U7 joins the stitched ground pours. Verify ESD performance on the prototype.

U10 uses TPD4E05U06DQAR, sharing the U6/U7 part type. Pin 1 shunts V3_SW and pin 5 shunts V5_SW; pins 3 and 8 connect directly to ground copper. Pins 2 and 4 are unused protection channels; pins 6, 7, 9 and 10 are internally unconnected. All six unused pins are explicitly left open. No supply reference is needed, so C21 is removed. D2 remains the separate input TVS. All ten non-ground header contacts retain ESD shunts. Power current must use copper, not flow through the protection package. The arrays are placed beside J2 with short ground returns. The new GPIO arrays have 0.5 pF typical capacitance at the specified bias and device-level ratings of +/-12 kV contact and +/-15 kV air discharge. These ratings do not establish a board-level pass.

TPD4E05U06 has a 5.5 V stand-off rating, 6.5 V minimum breakdown and approximately 10 V clamping at 1 A TLP. It is not a 3.3 V overvoltage limiter, nor a guarantee that protected pins stay below their DC absolute-maximum voltage during an ESD pulse. Its clamp behavior differs from the previous supply-steering topology. Qualify residual stress and ESD performance on the finished layout. Signals remain 3.3 V only; do not drive the board while unpowered or back-power either output. The power array now uses independent ground-referenced shunts; the 0–5.5 V operating range accommodates the nominal output voltages, but does not establish sustained overvoltage protection.


## Output fault monitoring

The two existing fault nets now connect independently to spare controller inputs: FAULT4 (3.3 V channel) to U3 pad 17 / GPIO15, and FAULT5 (5 V channel) to U3 pad 5 / GPIO3. R13/R14 are removed at user request; TP5/TP6 remain accessible. Enable the two internal pullups in firmware. The corresponding footprints and pullup traces are removed from the PCB. These internal inputs are separate from user-facing IO0-IO7.

Pin selection was checked against the module pin table and the controller boot-configuration tables. GPIO15 is an unused general input here. GPIO3 also serves as the MTDI/SDIO-edge strap; its level affects the unused SDIO interface, not selection of the normal flash boot or USB/UART recovery modes. Reserve it for fault sensing; external pad debugging and SDIO use would require reassessment. Test normal boot and recovery with FAULT5 both high and low. Module pin table: the datasheet linked in U3. Boot configuration source: the manufacturer's controller datasheet, sections 3.1-3.4.

The switches provide active-low open-drain status. LOW reports overcurrent, overtemperature or reverse voltage; HIGH means no asserted fault, not proof of a healthy or enabled rail. The three causes cannot be distinguished from the fault pin alone. Their built-in status delays are about 7.5 ms for overcurrent and 4 ms for reverse voltage; thermal faults are reported immediately. The internal pullups are weaker than the removed 10 kΩ resistors. The specified typical pullup is 45 kΩ; combined with the switch's 1 µA maximum off-state fault leakage, this gives about 45 mV drop as an illustrative typical-resistance calculation, not a guaranteed worst-case bound. Validate noise margin, rise time and leakage on the final board. Keep the fault traces short. Before software enables the pulls, released fault lines can float and test-pad readings are not valid.

Firmware monitoring remains outside this hardware layout task. It must configure both pins as inputs with pullup enabled and pulldown disabled (never push-pull outputs), allow settling, read initial levels before enabling loads, and monitor assertion/recovery by interrupts or polling with appropriate startup handling. Reinitialize pullups after reset and any sleep mode that loses their configuration; mask fault reporting until configuration is restored. Independent hardware current limiting remains active without firmware. The fault outputs do not monitor regulator output voltage, fuse continuity or total input failure; an unpowered controller cannot report a live fault. Both fault routes are present, and the old R13/R14 footprints are removed.


## Consolidated component choices

R9/R10/R12 use the approved 10 kΩ value. R7/R8 remain 26.1 kΩ; no lower current-limit setting has been selected. C6 remains 10 µF in 0805, and the DC/DC circuit and its capacitor values remain unchanged. D1/D3 use the same MDD SS34 SMA procurement part; other manufacturers' SS34 package variants are not automatically interchangeable. F1 remains the selected fast fuse. Resistors use nine values and capacitors seven values.

## Validation and fabrication

Final acceptance requires zero electrical-rule violations, physical-rule violations, schematic-parity findings and unconnected items. The saved reconstruction and export steps regenerate those reports. Critical supply paths use at least 0.5 mm copper, retaining 0.6 mm where space permits; pull-up, indicator and protection branches may be 0.2 mm. A short 0.18 mm front-layer ground stitch below header pin 3 joins the second ground contact to the main return; its current and temperature behavior remains part of prototype qualification. Clearance rules remain 0.15 mm minimum signal, 0.2 mm power and 0.25 mm copper-to-edge. Local routing uses 0.5/0.25 mm vias where required, within the published [fabricator capabilities](https://jlcpcb.com/capabilities/Capab); larger power and exposed-pad vias are retained. Project minimums are 0.5 mm via diameter and 0.25 mm through-hole diameter, matching the generator and routing. These restore the documented limits after project settings had reverted to 0.6/0.3 mm; clearance and the 0.1 mm minimum annular ring remain enforced. No individual DRC exclusions are used. No fabrication exports or component orders have been placed.

CAD checks do not establish thermal, electrical, RF or USB compliance. No Rev A board has been fabricated or bench tested. Complete `prototype-test.md` before accepting production current ratings. Single-sided assembly, EP paste/vias, connector pin protrusion and pick-and-place rotation conventions require assembler review. No fabrication or component order has been placed.


## Grouped indicators

D4-D7 form one top-side column at x=30.3 mm, on 3.2 mm pitch, with corresponding
shell openings and recessed legends. D4 and D5 are green and monitor V3_SW and
V5_SW after the output switches. D6 is red and monitors V5 after F1. D7 is blue,
driven active-high by control pin 26 (U3 pad 20); it does not consume a header IO.
Firmware control of STATUS_LED is not added by this hardware change.

D4/D5 remain green (KT-0603G), D6 red (KT-0603R), and D7 blue (KT-0603B).
The supplied red PDF and matching green/blue manufacturer sheets are retained
in this directory as led-kt-0603r/g/b.pdf. They specify a 1.6 x 0.8 x 0.6 mm body,
two 0.7 mm square lands with a 0.7 mm gap, pin 1 anode and pin 2 cathode.
This full-datasheet pattern supersedes the earlier example image's 0.75 x 0.8 mm lands.
The symbols and pad mappings use these polarities; the original colors remain unchanged.
R19-R22 all use **660 ohms, 1%, 0603, >=0.1 W**, consolidating the three
indicator resistor values into one BOM line. Each LED retains its own resistor.
Green/blue forward voltage is specified as 2.6-3.1 V at 5 mA, and red as
1.8-2.4 V at 20 mA in the stored manufacturer sheets (page 3). Using those
voltage ranges gives approximate nominal currents of 0.3-1.1 mA at 3.3 V,
2.9-3.6 mA for the 5 V green LED, and 3.9-4.8 mA for the 5 V red LED.
These are estimates, since forward voltage changes with current and temperature;
the status output also has voltage drop. Equal resistor values do not imply equal brightness. The 660 ohm selection
roughly halves current relative to 330 ohms; check that the 3.3 V indicators
remain sufficiently visible through the enclosure.

At an assumed 5.25 V maximum input and minimum 653.4 ohm resistance, even the
conservative zero-forward-voltage current bound is 8.1 mA, below the datasheet
25 mA red and 30 mA green/blue absolute maximum ratings at 25 C. The maximum
LED dissipation for any forward voltage is V^2/(4R) = 10.6 mW, below the red
40 mW and green/blue 100 mW ratings at 25 C. With the tabulated red voltage
range the resistor dissipation is at most about 18.3 mW. These calculations are
not temperature qualification. Verify brightness, thermal margin and light bleed
in the enclosure on a prototype; the 5 V indicators draw more than with their
former 1.2 kohm / 1.5 kohm resistors.


BZ1 uses the supplied buzzer-5020-supplied.png specification: 5.2 x 5.2 x 2 mm,
3 V nominal, 2-4 V operating, 4 kHz at 50% duty, 100 mA maximum, and
12 +/- 3 ohm coil resistance. Its supplier/part number was not supplied; the BOM
identifies the specification instead of retaining the incompatible KLJ-5020 identity.
Top-view terminals are positive lower-right, negative upper-right and dummy
lower-left. The package outline includes the 1 mm upper-left chamfer and edge
steps. Terminal widths are 0.7 mm with 2.8 mm horizontal inner gap; the 1 mm
terminal length and 3.2 mm vertical inner gap give 4.2 mm vertical center pitch.
PCB lands are derived as 1.1 x 1.4 mm, adding 0.2 mm side, heel and toe allowance,
with centers at x +/-1.75 and y +/-2.1 mm.
These are engineering-derived lands, not a manufacturer-recommended land pattern;
confirm with the assembler and actual supplied part before ordering. The third
land is mechanical only and has no net. The existing passive drive circuit remains.

Package-body artwork remains on the fabrication layers and is also projected to
the corresponding silkscreen layer, preserving native chamfers and contours.
Strokes are clipped around exposed pads and board edges; no outer bounding boxes
are added. Reference labels are placed clear of the resulting artwork.

R7/R8/R9/R10, R19-R22 and TP3 move to the front. Nearby small parts are
repositioned to maintain assembly clearances. Every component and test pad must
remain on this same surface. Board outline and approved revision/date remain
unchanged. The indicator wells and labels follow the unchanged LED placements;
the blank QR area remains clear. The completed routing passes the full routed CAD checks. Prototype qualification remains required before production.
