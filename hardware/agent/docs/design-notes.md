# SAIHub-Mini Rev A design review

Version: 2026-09-24. Author: GPT6-Astra. Co-author: xqe2011.
Version updates require explicit user permission; record each approved change in `../../docs/version-log.md`.

**Current layout:** module and button replacement on the unchanged 48 × 28 mm envelope. There are 56 top footprints and one back-side test pad (TP3); all mounted parts remain on top. The final reports describe this placement.

## Requirements and mechanical envelope

The front edge retains J2, SW1 and J1 from left to right. SW1 is TS-2435VS, procurement code C47734518. Its [manufacturer drawing](https://atta.szlcsc.com/upload/public/pdf/source/20250414/8774CEBD0F324351C3CF1F5D7421BC0B.pdf) specifies two 0.6 × 1.2 mm contact lands on 3.4 mm centres, plus two stepped mechanical anchors: 5.3 mm outer span, 3.2 mm inner gap and 1.3 mm depth. There is a 0.7 mm gap between anchor and contact rows. Mounting anchors remain electrically unconnected. The selected variant has no locating posts.

The new module has a built-in antenna and a 16 × 24 mm body. The [manufacturer drawing, pages 9–10](https://atta.szlcsc.com/upload/public/pdf/source/20251016/3D572157C42EA1AA3992B58D4700E12F.pdf) defines its 22 perimeter pads and central ground pad. It is rotated 180 degrees, with the antenna at the bottom-left. The module body runs from (0.85,4) to (16.85,28) mm. The notch runs from (0,20.5) to (21.85,28) mm, opening at both outer edges. Its right edge is exactly 5 mm beyond the antenna. The connected section retains a 0.85 mm left inset to keep the specified solder lands on the PCB with copper-edge clearance. Outer bounds remain 48 × 28 mm.

Two copper layers, 1.6 mm board thickness; all mounted parts are on top. TP3 is a bare test pad on the back at (2.6,19.5) mm. The module supply, support parts and test points move to fit the notch. The power-stage component positions are retained. No mounting holes or external antenna accessory. RF operation and enclosure clearance must be qualified on a prototype.

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

BZ1 is **KELIKING KLJ-5020**. The [manufacturer specification v3.1](https://datasheet.lcsc.com/datasheet/pdf/5a334e56ebfeea427b46ed3bd7f9e2de.pdf?productCode=C556937) specifies a 5 × 5 × 2 mm body, 3.3 V rated drive, 2-4 V operating range, and ≤110 mA mean current at **4 kHz / 50% duty**. Its page-6 recommended top-view lands have 6.3 × 4.52 mm total span and 2.4 × 1.72 mm gaps: three 1.95 × 1.4 mm pads. Positive is upper-left, negative upper-right; the third, lower-left land is mechanical and unconnected. Do not substitute an arbitrary 5020 without checking its footprint and polarity. D3 now uses the shared MDD SS34 flyback part and Q1 is AO3400A; do not drive the coil directly from GPIO.

Native USB retains the 22 ohm series resistors and connector-side protection. These connections must be rerouted for the new module and support-component placement; the old USB routing coordinates are not reusable. The design is not impedance-qualified. Validate enumeration, flashing and traffic in both plug orientations.

## Header protection

U6/U7 use two [TPD4E05U06DQAR](https://www.ti.com/lit/ds/symlink/tpd4e05u06.pdf) four-channel arrays for IO0-IO7. Pins 1, 2, 4 and 5 protect the four signals; pins 3 and 8 connect to ground. Pins 6, 7, 9 and 10 are internally unconnected and explicitly left open. These are not duplicate signal pins. The ground-referenced arrays need no supply or local bypass capacitor. They replace four two-channel arrays and C17-C20: GPIO protection goes from eight added components to two.

Each DQA body is 2.5 x 1.0 mm. The project-local footprint follows DQA0010A recommended lands (0.565 x 0.2 mm signals, 0.565 x 0.4 mm grounds, 0.5 mm pitch, 0.835 mm row-center spacing). Check the supplied package revision and stencil against the manufacturer's drawing before assembly; the datasheet also contains DQA0010B. The 1.9 x 3.0 mm courtyards pass the final placement checks. U6/U7 remain beside the header. U6 has a routed ground return to the header ground; U7 joins the stitched ground pours. Verify ESD performance on the prototype.

U10 remains USBLC6-2SC6 for V3_SW and V5_SW, with V5 reference and C21 bypass. All ten non-ground header contacts retain ESD shunts. Power current must use copper, not flow through the protection package. The arrays are placed beside J2 with short ground returns. The new GPIO arrays have 0.5 pF typical capacitance at the specified bias and device-level ratings of +/-12 kV contact and +/-15 kV air discharge. These ratings do not establish a board-level pass.

TPD4E05U06 has a 5.5 V stand-off rating, 6.5 V minimum breakdown and approximately 10 V clamping at 1 A TLP. It is not a 3.3 V overvoltage limiter, nor a guarantee that protected pins stay below their DC absolute-maximum voltage during an ESD pulse. Its clamp behavior differs from the previous supply-steering topology. Qualify residual stress and ESD performance on the finished layout. Signals remain 3.3 V only; do not drive the board while unpowered or back-power either output. The separate power array still uses supply-referenced steering.


## Output fault monitoring

The two existing fault nets now connect independently to spare controller inputs: FAULT4 (3.3 V channel) to U3 pad 17 / GPIO15, and FAULT5 (5 V channel) to U3 pad 5 / GPIO3. R13/R14 are removed at user request; TP5/TP6 remain accessible. Enable the two internal pullups in firmware. The corresponding footprints and pullup traces are removed from the PCB. These internal inputs are separate from user-facing IO0-IO7.

Pin selection was checked against the module pin table and the controller boot-configuration tables. GPIO15 is an unused general input here. GPIO3 also serves as the MTDI/SDIO-edge strap; its level affects the unused SDIO interface, not selection of the normal flash boot or USB/UART recovery modes. Reserve it for fault sensing; external pad debugging and SDIO use would require reassessment. Test normal boot and recovery with FAULT5 both high and low. Module pin table: the datasheet linked in U3. Boot configuration source: the manufacturer's controller datasheet, sections 3.1-3.4.

The switches provide active-low open-drain status. LOW reports overcurrent, overtemperature or reverse voltage; HIGH means no asserted fault, not proof of a healthy or enabled rail. The three causes cannot be distinguished from the fault pin alone. Their built-in status delays are about 7.5 ms for overcurrent and 4 ms for reverse voltage; thermal faults are reported immediately. The internal pullups are weaker than the removed 10 kΩ resistors. The specified typical pullup is 45 kΩ; combined with the switch's 1 µA maximum off-state fault leakage, this gives about 45 mV drop as an illustrative typical-resistance calculation, not a guaranteed worst-case bound. Validate noise margin, rise time and leakage on the final board. Keep the fault traces short. Before software enables the pulls, released fault lines can float and test-pad readings are not valid.

Firmware monitoring remains outside this hardware layout task. It must configure both pins as inputs with pullup enabled and pulldown disabled (never push-pull outputs), allow settling, read initial levels before enabling loads, and monitor assertion/recovery by interrupts or polling with appropriate startup handling. Reinitialize pullups after reset and any sleep mode that loses their configuration; mask fault reporting until configuration is restored. Independent hardware current limiting remains active without firmware. The fault outputs do not monitor regulator output voltage, fuse continuity or total input failure; an unpowered controller cannot report a live fault. Both fault routes are present, and the old R13/R14 footprints are removed.


## Consolidated component choices

R9/R10/R12 use the approved 10 kΩ value. R7/R8 remain 26.1 kΩ; no lower current-limit setting has been selected. C6 remains 10 µF in 0805, and the DC/DC circuit and its capacitor values remain unchanged. D1/D3 use the same MDD SS34 SMA procurement part; other manufacturers' SS34 package variants are not automatically interchangeable. F1 remains the selected fast fuse. Resistors use eight values and capacitors seven values.

## Validation and fabrication

Final acceptance requires zero electrical-rule violations, physical-rule violations, schematic-parity findings and unconnected items. The saved reconstruction and export steps regenerate those reports. Main power routes retain 0.6 mm widths; input connector escapes are 0.5 mm, and the pull-up / protection-bias branches are 0.2 mm. clearance rules remain 0.15 mm minimum signal, 0.2 mm power and 0.25 mm copper-to-edge. Local routing uses 0.5/0.25 mm vias where required, within the published [fabricator capabilities](https://jlcpcb.com/capabilities/Capab); larger power and exposed-pad vias are retained. No individual DRC exclusions are used. No fabrication exports or component orders have been placed.

CAD checks do not establish thermal, electrical, RF or USB compliance. No Rev A board has been fabricated or bench tested. Complete `prototype-test.md` before accepting production current ratings. Top-side assembly, EP paste/vias, connector pin protrusion and pick-and-place rotation conventions require assembler review. No fabrication or component order has been placed.
