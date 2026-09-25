# SAIHub-Mini Rev A design review

Version: 2026-09-24. Author: GPT6-Astra. Co-author: xqe2011.
Version updates require explicit user permission; record each approved change in `../../docs/version-log.md`.

**Current layout:** the schematic changes are integrated into the 48 × 28 mm board. All 57 footprints are on top; both copper layers are routed and have connected ground pours. U6/U7 and U10/C21 are beside the header, and both fault signals reach their assigned controller inputs. The outline and Rev A are retained; the approved date version is 2026-09-24.

## Requirements and mechanical envelope

The board uses a regulated 5 V / 3 A source, nominal 1 A limits on each of the two external power channels, SGM6232 DC/DC, and USB-C beside the 12-pin right-angle header on the same edge. IO0-IO7 remain logic signals, not 1 A power outputs.

The PCB is **48 × 28 mm**, with all components on top. The front edge is ordered **J2 header, SW1 BOOT, J1 USB-C**, from left to right. Width is set by those three footprints and assembly clearances. Header pins and the switch actuator project beyond the outline.

SW1 is [Panasonic EVQP7A01P](https://industry.panasonic.com/ap/en/products/control/switch/light-touch/number/evqp7a01p), a side-push switch with a 3.5 × 2.9 mm body and 1.35 mm mounting height. The actuator projects beyond the body (3.55 mm overall depth). The project-local `SW_SPST_EVQP7A` footprint matches the manufacturer's straight-terminal land pattern; duplicate pads 1 and 2 retain BOOT-to-GND operation. Rotate 180 degrees so the actuator faces the front edge.

The board has 57 footprints and is routed on both copper layers with filled ground zones. The main module moves 1.95 mm toward the rear; nearby passives, the buzzer transistor and test pads are rearranged to fit the added protection without increasing either board dimension. Redundant autorouter ground traces are removed; short fixed protection/power-stage returns and stitching vias remain. Both pours pass connectivity checks, including their separate filled regions. The author/co-author silkscreen moves to the bottom to keep it clear of the compact top placement.

Two copper layers, 1.6 mm FR-4, nominal 1 oz copper. **All 57 footprints, including all fitted BOM parts and test pads, are on the top face.** The bottom has routed copper and ground fill but no fitted parts; the through-hole connector tails still extend below the PCB. The maximum body height is set by the fitted connector/module combination, not the PCB outline. No mounting holes. Check USB plug and header mating clearance in the final enclosure.

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

This is a **nominal 1 A protection setting**, not a guaranteed 1 A continuous output and not a hard 1.000 A maximum. Units may enter current limit below 1 A. Qualify rated continuous loading below the lowest measured/guaranteed threshold, accounting for temperature. FAULT outputs now connect to separate controller inputs (FAULT4: U3 pad 19; FAULT5: U3 pad 5) and remain available at TP5/TP6. R13/R14 are removed; firmware must enable internal pullups before monitoring these inputs. Firmware monitoring is pending. There is no controlled output discharge.

F1 is a 3 A, 1206 Littelfuse 0467003.NR fuse. The fuse is fault protection, not a precise 3 A limiter. Supply and cable must be rated for 5 V / 3 A. USB-C still has separate 5.1 kΩ CC pulldowns and has no PD negotiation or source-current detection. External loads must remain disabled on unqualified PC USB supplies; the board cannot identify available source current.

Illustrative budget at the 1.081 A upper channel limits: reserve 0.6 A for ESP peak demand and 0.11 A for the buzzer. Buck demand is then 1.791 A, below its nominal 2 A rating. At 4.75 V and an assumed 80% conversion efficiency, total input is approximately `1.081 + 3.314 × 1.791 / (4.75 × 0.80) = 2.64 A`, before small control losses. These reservations and efficiency are design assumptions, not measured performance.

SGM6232 specifies 4.5 V minimum input and 80% maximum duty cycle. Cable/fuse losses can therefore still compromise 3.3 V at high load. Aim for **at least 4.75 V at U2 IN under load** and verify the actual dropout margin, startup and heat. The SMF5.0A TVS is not a sustained overvoltage disconnect. Input ceramic capacitance still requires hot-plug/inrush verification.

## Interfaces and assembly details

The DOIT ESPC5-32E-H4 module, its exact pinout, USB4105-GF-A connector, Würth 61301211021 header, GPIO mapping, reset pads and buzzer circuit remain. An external dual-band U.FL antenna is required. GPIOs are 3.3 V only. Neither switched output may be back-powered.

BZ1 is **KELIKING KLJ-5020**. The [manufacturer specification v3.1](https://datasheet.lcsc.com/datasheet/pdf/5a334e56ebfeea427b46ed3bd7f9e2de.pdf?productCode=C556937) specifies a 5 × 5 × 2 mm body, 3.3 V rated drive, 2-4 V operating range, and ≤110 mA mean current at **4 kHz / 50% duty**. Its page-6 recommended top-view lands have 6.3 × 4.52 mm total span and 2.4 × 1.72 mm gaps: three 1.95 × 1.4 mm pads. Positive is upper-left, negative upper-right; the third, lower-left land is mechanical and unconnected. Do not substitute an arbitrary 5020 without checking its footprint and polarity. D3 now uses the shared MDD SS34 flyback part and Q1 is AO3400A; do not drive the coil directly from GPIO.

Native USB retains the series resistors and USBLC6-2SC6. Its long run uses adjacent 0.2 mm bottom-layer traces on 0.4 mm center spacing, left of the buck stage; the connector and ESD escapes complete the connection. These are not impedance-qualified traces. Validate full-speed enumeration, flashing and traffic in both plug orientations. The additional 1.0 mm bottom VBUS link connects the two connector power-pad groups using 0.8/0.4 mm vias and a short 0.45 mm pad escape.

## Header protection

U6/U7 use two [TPD4E05U06DQAR](https://www.ti.com/lit/ds/symlink/tpd4e05u06.pdf) four-channel arrays for IO0-IO7. Pins 1, 2, 4 and 5 protect the four signals; pins 3 and 8 connect to ground. Pins 6, 7, 9 and 10 are internally unconnected and explicitly left open. These are not duplicate signal pins. The ground-referenced arrays need no supply or local bypass capacitor. They replace four two-channel arrays and C17-C20: GPIO protection goes from eight added components to two.

Each DQA body is 2.5 x 1.0 mm. The project-local footprint follows DQA0010A recommended lands (0.565 x 0.2 mm signals, 0.565 x 0.4 mm grounds, 0.5 mm pitch, 0.835 mm row-center spacing). Check the supplied package revision and stencil against the manufacturer's drawing before assembly; the datasheet also contains DQA0010B. The 1.9 x 3.0 mm courtyards pass the final placement checks. U6/U7 are placed beside the header with short ground traces to vias on both sides of each array.

U10 remains USBLC6-2SC6 for V3_SW and V5_SW, with V5 reference and C21 bypass. All ten non-ground header contacts retain ESD shunts. Power current must use copper, not flow through the protection package. The arrays are placed beside J2 with short ground returns. The new GPIO arrays have 0.5 pF typical capacitance at the specified bias and device-level ratings of +/-12 kV contact and +/-15 kV air discharge. These ratings do not establish a board-level pass.

TPD4E05U06 has a 5.5 V stand-off rating, 6.5 V minimum breakdown and approximately 10 V clamping at 1 A TLP. It is not a 3.3 V overvoltage limiter, nor a guarantee that protected pins stay below their DC absolute-maximum voltage during an ESD pulse. Its clamp behavior differs from the previous supply-steering topology. Qualify residual stress and ESD performance on the finished layout. Signals remain 3.3 V only; do not drive the board while unpowered or back-power either output. The separate power array still uses supply-referenced steering.


## Output fault monitoring

The two existing fault nets now connect independently to spare controller inputs: FAULT4 (3.3 V channel) to U3 pad 19 / GPIO15, and FAULT5 (5 V channel) to U3 pad 5 / GPIO3. R13/R14 are removed at user request; TP5/TP6 remain accessible. Enable the two internal pullups in firmware. The corresponding footprints and pullup traces are removed from the PCB. These internal inputs are separate from user-facing IO0-IO7.

Pin selection was checked against the module pin table and the controller boot-configuration tables. GPIO15 is an unused general input here. GPIO3 also serves as the MTDI/SDIO-edge strap; its level affects the unused SDIO interface, not selection of the normal flash boot or USB/UART recovery modes. Reserve it for fault sensing; external pad debugging and SDIO use would require reassessment. Test normal boot and recovery with FAULT5 both high and low. Module pin table: the datasheet linked in U3. Boot configuration source: the manufacturer's controller datasheet, sections 3.1-3.4.

The switches provide active-low open-drain status. LOW reports overcurrent, overtemperature or reverse voltage; HIGH means no asserted fault, not proof of a healthy or enabled rail. The three causes cannot be distinguished from the fault pin alone. Their built-in status delays are about 7.5 ms for overcurrent and 4 ms for reverse voltage; thermal faults are reported immediately. The internal pullups are weaker than the removed 10 kΩ resistors. The specified typical pullup is 45 kΩ; combined with the switch's 1 µA maximum off-state fault leakage, this gives about 45 mV drop as an illustrative typical-resistance calculation, not a guaranteed worst-case bound. Validate noise margin, rise time and leakage on the final board. Keep the fault traces short. Before software enables the pulls, released fault lines can float and test-pad readings are not valid.

Firmware monitoring remains outside this hardware layout task. It must configure both pins as inputs with pullup enabled and pulldown disabled (never push-pull outputs), allow settling, read initial levels before enabling loads, and monitor assertion/recovery by interrupts or polling with appropriate startup handling. Reinitialize pullups after reset and any sleep mode that loses their configuration; mask fault reporting until configuration is restored. Independent hardware current limiting remains active without firmware. The fault outputs do not monitor regulator output voltage, fuse continuity or total input failure; an unpowered controller cannot report a live fault. Both fault routes are present, and the old R13/R14 footprints are removed.


## Consolidated component choices

R9/R10/R12 use the approved 10 kΩ value. R7/R8 remain 26.1 kΩ; no lower current-limit setting has been selected. C6 remains 10 µF in 0805, and the DC/DC circuit and its capacitor values remain unchanged. D1/D3 use the same MDD SS34 SMA procurement part; other manufacturers' SS34 package variants are not automatically interchangeable. F1 remains the selected fast fuse. Resistors use eight values and capacitors seven values.

## Validation and fabrication

See `../validation/erc.rpt`, `../validation/drc.rpt`, `../validation/routing-drc.json` and `../validation/routing-summary.json`: zero ERC violations, zero DRC violations, zero unconnected items and zero schematic parity findings. No fabrication exports have been produced for this layout. Manufacturing export runs ERC and PCB DRC with schematic parity before generating Gerbers. The power class retains 0.6 mm routing widths, with a 0.8 mm output trunk and short connector neck-downs. Clearance rules remain 0.15 mm minimum signal, 0.2 mm power netclass and 0.25 mm copper-to-edge. No individual DRC exclusions are used.

CAD checks do not establish thermal, electrical, RF or USB compliance. No Rev A board has been fabricated or bench tested. Complete `prototype-test.md` before accepting production current ratings. Top-side assembly, EP paste/vias, connector pin protrusion and pick-and-place rotation conventions require assembler review. No fabrication or component order has been placed.
