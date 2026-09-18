#!/usr/bin/env python3
"""Rebuild the editable schematic and *unrouted* board from one connectivity source.

Run with KiCad's bundled Python (pcbnew). Routing is a separate, reviewed step;
this script never overwrites saihub.kicad_pcb, only agent/routing/saihub-unrouted.kicad_pcb.
"""
import json
import math
import os
from pathlib import Path
import re
import shutil
import uuid
import pcbnew as p
from identity import identity, apply_board_identity
import wx
app = wx.App(False)

ROOT = Path(__file__).resolve().parents[2]
for directory in ('agent/validation', 'agent/previews', 'agent/routing', 'docs'):
    (ROOT/directory).mkdir(parents=True, exist_ok=True)
LIB = ROOT / 'libraries'
FP = LIB / 'Saihub.pretty'
KICAD = Path(os.environ.get('KICAD_SHARE', '/Applications/KiCad/KiCad.app/Contents/SharedSupport'))
W, H = 48.0, 28.0
NS = uuid.UUID('8ce59343-cd0c-4971-923b-6d37131b8b35')
def uid(s): return str(uuid.uuid5(NS, s))
def q(s): return json.dumps(str(s))
def mm(v): return p.FromMM(v)
def xy(x,y): return p.VECTOR2I(mm(x),mm(y))
def fx(size=1.0,extra=''): return f'(effects (font (size {size} {size})) {extra})'

parts = []
symbols = {}
def symbol(name,pins,kind='block'):
    """Pins: (number, name, electrical type, side). Unit coordinates are mm."""
    sides = {'L':[], 'R':[]}
    for pin in pins: sides[pin[3]].append(pin)
    n=max(len(v) for v in sides.values())
    h=max(2.54,(n+1)*1.27)
    w=10.16 if kind=='block' else 2.54
    loc=[]
    for side,group in sides.items():
        for i,(num,pn,typ,_) in enumerate(group):
            y=(len(group)-1)*1.27-i*2.54
            loc.append((str(num),pn,typ,(-1 if side=='L' else 1)*(w+2.54),y,0 if side=='L' else 180))
    shapes = f'(rectangle (start {-w} {h}) (end {w} {-h}) (stroke (width 0.254) (type default)) (fill (type background)))'
    if kind=='R':
        h=1.016
        shapes=f'(rectangle (start -2.54 1.016) (end 2.54 -1.016) (stroke (width 0.254) (type default)) (fill (type none)))'
    elif kind=='C':
        h=1.8
        shapes=''.join(f'(polyline (pts (xy {x} -1.8) (xy {x} 1.8)) (stroke (width 0.254) (type default)) (fill (type none)))' for x in [-0.635,0.635])
        shapes+=''.join(f'(polyline (pts (xy {a} 0) (xy {b} 0)) (stroke (width 0.254) (type default)) (fill (type none)))' for a,b in [(-2.54,-0.635),(0.635,2.54)])
    text=f'(symbol {q(name)} (pin_names (offset 0.508){" (hide yes)" if kind!="block" else ""}) (in_bom yes) (on_board yes)'
    text+=f'(property "Reference" "U" (at 0 {h+2.54} 0) {fx()}) (property "Value" {q(name)} (at 0 {-h-2.54} 0) {fx()})'
    text+=f'(symbol {q(name+"_0_1")} {shapes})'
    text+=f'(symbol {q(name+"_1_1")} '
    for num,pn,typ,x,y,ang in loc:
        text+=f'(pin {typ} line (at {x} {y} {ang}) (length 2.54) (name {q(pn)} {fx()}) (number {q(num)} {fx()}))'
    text+='))'
    symbols[name]={'text':text,'pins':loc,'h':h}

symbol('R',[(1,'1','passive','L'),(2,'2','passive','R')],'R')
symbol('C',[(1,'1','passive','L'),(2,'2','passive','R')],'C')
symbol('CP',[(1,'+','passive','L'),(2,'-','passive','R')],'small')
symbol('L',[(1,'1','passive','L'),(2,'2','passive','R')],'small')
symbol('D',[(1,'K','passive','L'),(2,'A','passive','R')],'small')
symbol('F',[(1,'1','passive','L'),(2,'2','passive','R')],'small')
symbol('TP',[(1,'TEST','passive','L')],'small')
symbol('BOOT',[(1,'BOOT','passive','L'),(2,'GND','passive','R')],'small')
symbol('BUZZER',[(1,'+','passive','L'),(2,'-','passive','R')],'small')
symbol('AO3400A',[(1,'G','input','L'),(2,'S','passive','R'),(3,'D','passive','R')])
symbol('SGM6232',[(2,'IN','power_in','L'),(7,'EN','input','L'),(8,'SS','passive','L'),(4,'GND','power_in','L'),(9,'EP_GND','passive','L'),(1,'BS','passive','R'),(3,'SW','power_out','R'),(5,'FB','input','R'),(6,'COMP','passive','R')])
symbol('TPS2553',[(1,'IN','power_in','L'),(3,'EN','input','L'),(2,'GND','power_in','L'),(6,'OUT','power_out','R'),(5,'ILIM','passive','R'),(4,'FAULT_N','open_collector','R')])
module_names=['GND','3V3','EN','GPIO2','GPIO3','GPIO0','GPIO1','GPIO6','GPIO7','GPIO8','GPIO9','GPIO10','GPIO13','GPIO14','GPIO28','GPIO5','GPIO4','GPIO27','GPIO15','NC','GPIO23','NC','GPIO24','GPIO12','GPIO11','GPIO25','GPIO26','GND','EPAD']
symbol('ESPC5_32E_H4',[(i+1,n,'power_in' if n in ['GND','3V3','EPAD'] else 'input' if n=='EN' else 'no_connect' if n=='NC' else 'bidirectional','L' if i<14 else 'R') for i,n in enumerate(module_names)])
usb_pins=[('A4','VBUS'),('A9','VBUS'),('B4','VBUS'),('B9','VBUS'),('A1','GND'),('A12','GND'),('B1','GND'),('B12','GND'),('SH','SHIELD'),('A5','CC1'),('B5','CC2'),('A6','D+'),('B6','D+'),('A7','D-'),('B7','D-'),('A8','SBU1'),('B8','SBU2')]
symbol('USB_C',[(n,v,'passive','L' if i<9 else 'R') for i,(n,v) in enumerate(usb_pins)])
symbol('USBLC6_2SC6',[(1,'IO1','passive','L'),(2,'GND','power_in','L'),(3,'IO2','passive','L'),(6,'IO1','passive','R'),(5,'VBUS','power_in','R'),(4,'IO2','passive','R')])
symbol('HEADER_12',[(i,str(i),'passive','L' if i<=4 else 'R') for i in range(1,13)])
symbol('PWR_FLAG',[(1,'POWER','power_out','L')],'small')

def add(ref,value,sym,foot,nets,pos,page,sch,mpn='',desc='',url='',rot=0):
    parts.append(dict(ref=ref,value=value,sym=sym,foot=foot,nets={str(k):v for k,v in nets.items()},pos=pos,page=page,sch=sch,mpn=mpn or value,desc=desc,url=url,rot=rot,uuid=uid(ref)))
RFP='Resistor_SMD:R_0603_1608Metric'
CFP='Capacitor_SMD:C_0603_1608Metric'
def r(ref,val,a,b,pos,page,sch,rot=0): add(ref,val,'R',RFP,{1:a,2:b},pos,page,sch,desc='1%, 0603',rot=rot)
def c(ref,val,a,b,pos,page,sch,foot=CFP,rot=0): add(ref,val,'C',foot,{1:a,2:b},pos,page,sch,desc='X7R, 10V minimum',rot=rot)

# Coordinates are board-local, top-side view. Header pins project beyond y=0.
add('J1','USB4105-GF-A','USB_C','Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal',
    {n:('VBUS' if v=='VBUS' else 'GND' if v in ['GND','SHIELD'] else 'USB_DP' if v=='D+' else 'USB_DM' if v=='D-' else v if v.startswith('CC') else None) for n,v in usb_pins},(15.25,57.5),'usb',(65,65),url='https://gct.co/files/drawings/usb4105.pdf',rot=180)
r('R1','5.1k','CC1','GND',(21.8,56),'usb',(155,40),90)
r('R2','5.1k','CC2','GND',(8.7,56),'usb',(245,40),90)
add('U1','USBLC6-2SC6','USBLC6_2SC6','Package_TO_SOT_SMD:SOT-23-6',{1:'USB_DM',2:'GND',3:'USB_DP',4:'USB_DP',5:'VBUS',6:'USB_DM'},(14.6,51.0),'usb',(160,78),url='https://www.st.com/resource/en/datasheet/usblc6-2.pdf',rot=90)
c('C1','100n','VBUS','GND',(19,51),'usb',(245,75))
r('R3','22','USB_DM','MCU_DM',(3.5,26),'usb',(155,115),90)
r('R4','22','USB_DP','MCU_DP',(3.5,29),'usb',(245,115),90)
add('F1','3A fuse','F','Fuse:Fuse_1206_3216Metric',{1:'VBUS',2:'V5'},(21,53.5),'usb',(65,125),mpn='0467003.NR',desc='Littelfuse 467, 3A fast fuse',url='https://www.littelfuse.com/~/media/electronics/datasheets/fuses/littelfuse_fuse_467_datasheet.pdf')
add('D2','SMF5.0A','D','Diode_SMD:D_SOD-123F',{1:'V5',2:'GND'},(25.8,55),'usb',(65,155),url='https://www.yint-electronic.com/products/emsproduct/tvs/smf5-0a.html',rot=90)

SGM_URL='https://www.sg-micro.com/rect/assets/9fe04e94-334c-4982-964b-fc08001cd9ac/SGM6232.pdf'
add('U2','SGM6232YPS8G/TR','SGM6232','Saihub:SGM6232_SOIC8_EP',{1:'BS',2:'V5',3:'SW',4:'GND',5:'FB',6:'COMP',7:None,8:'SS',9:'GND'},(30,14),'power',(155,55),desc='EN open for auto-start; 3.314V divider; exposed pad GND',url=SGM_URL)
add('L1','4.7uH','L','Saihub:SRP5030TA',{1:'SW',2:'V3V3'},(36,20),'power',(245,45),mpn='SRP5030TA-4R7M',desc='Shielded 5.7x5.2x2.8mm; 4.6A Irms, 6A Isat per manufacturer drawing',url='https://bourns.com/docs/product-datasheets/SRP5030TA.pdf')
add('D1','SS34','D','Diode_SMD:D_SMA',{1:'SW',2:'GND'},(36,15),'power',(245,75),url='https://www.diodes.com/assets/Datasheets/ds23013.pdf')
c('C2','22u','V5','GND',(30,9),'power',(65,40),'Capacitor_SMD:C_1210_3225Metric')
c('C3','100n','V5','GND',(33,10),'power',(65,75))
c('C4','47u','V3V3','GND',(30,21),'power',(245,110),'Capacitor_SMD:C_1210_3225Metric')
c('C5','100n','V3V3','GND',(26,21),'power',(155,110))
r('R15','33k','V3V3','FB',(25,18),'power',(155,140))
r('R16','10.5k','FB','GND',(25,15),'power',(155,170))
r('R17','10k','COMP_RC','GND',(23,11),'power',(155,200))
r('R18','10','SW','BOOT_R',(36,10),'power',(155,230))
c('C14','10n','BOOT_R','BS',(36,8),'power',(245,140))
c('C15','100n','SS','GND',(27,9),'power',(245,170))
c('C16','5.6n','COMP','COMP_RC',(23,14),'power',(245,200))

module_nets={1:'GND',2:'V3V3',3:'EN',4:None,5:None,6:'IO2',7:'IO1',8:'IO6',9:None,10:'PWR3_EN',11:'PWR5_EN',12:'IO0',13:'MCU_DM',14:'MCU_DP',15:'BOOT',16:'IO5',17:'IO4',18:None,19:None,20:None,21:'IO3',22:None,23:'IO7',24:'BUZZ_PWM',25:'UART_TX',26:None,27:None,28:'GND',29:'GND'}
add('U3','ESPC5-32E-H4','ESPC5_32E_H4','Saihub:ESPC5-32E-H4',module_nets,(15.25,17.5),'mcu',(80,70),url='https://atta.szlcsc.com/upload/public/pdf/source/20251016/AE26C73F5F73E1BD024C1C95B3A92329.pdf')
c('C6','10u','V3V3','GND',(4.2,9.4),'mcu',(175,40),'Capacitor_SMD:C_0805_2012Metric',90)
c('C7','100n','V3V3','GND',(4.2,12.5),'mcu',(260,40),rot=90)
r('R5','10k','V3V3','EN',(3.5,15.5),'mcu',(175,75),90)
c('C8','1u','EN','GND',(3.5,18.5),'mcu',(260,75),rot=90)
r('R6','10k','V3V3','BOOT',(27,27.8),'mcu',(175,110),90)
add('SW1','BOOT','BOOT','Button_Switch_SMD:SW_SPST_EVQP7A',{1:'BOOT',2:'GND'},(25.5,31.1),'mcu',(260,110),mpn='EVQP7A01P',desc='3.5 x 2.9 mm body, 1.35 mm high, side-actuated toward connector edge',url='https://industry.panasonic.com/ap/en/products/control/switch/light-touch/number/evqp7a01p')
for ref,net,pos,sch in [('TP1','EN',(2,21.8),(175,145)),('TP2','GND',(4.8,21.8),(260,145)),('TP3','UART_TX',(27.4,12.7),(175,180)),('TP4','V3V3',(27.4,16),(260,180))]:
    add(ref,net,'TP','TestPoint:TestPoint_Pad_D1.5mm',{1:net},pos,'mcu',sch,mpn='PCB test pad',desc='Do not populate')

add('J2','1x12 2.54mm right-angle','HEADER_12','Connector_PinHeader_2.54mm:PinHeader_1x12_P2.54mm_Horizontal',{i+1:n for i,n in enumerate(['V3_SW','GND','V5_SW','GND']+['IO'+str(i) for i in range(8)])},(1.28,2.5),'outputs',(65,60),mpn='61301211021',desc='Wurth WR-PHD 2.54mm 1x12 right-angle male',url='https://www.we-online.com/components/products/datasheet/61301211021.pdf',rot=90)
for idx,vin,vout,en,x,scx in [(4,'V3V3','V3_SW','PWR3_EN',9,165),(5,'V5','V5_SW','PWR5_EN',22,265)]:
    add('U'+str(idx),'TPS2553DBVR','TPS2553','Package_TO_SOT_SMD:SOT-23-6',{1:vin,2:'GND',3:en,4:'FAULT'+str(idx),5:'ILIM'+str(idx),6:vout},(x,5.7),'outputs',(scx,45),url='https://www.ti.com/lit/ds/symlink/tps2553.pdf',rot=90)
    r('R'+str(idx+3),'26.1k','ILIM'+str(idx),'GND',(x+3.3,5.7),'outputs',(scx,80),90)
    r('R'+str(idx+5),'100k',en,'GND',(x-3.3,5.7),'outputs',(scx,110),90)
    c('C'+str(idx+5),'100n',vin,'GND',(x-0.8,8.8 if idx==4 else 9),'outputs',(scx,140))
    c('C'+str(idx+7),'1u',vout,'GND',(x+1.5,3.3),'outputs',(scx,170))
    # Fault outputs are observable on probe pads without consuming GPIO.
    r('R'+str(idx+9),'10k','V3V3','FAULT'+str(idx),(27.7,19+(idx-4)*3),'outputs',(scx,200),90)
    add('TP'+str(idx+1),'FAULT'+str(idx),'TP','TestPoint:TestPoint_Pad_D1.0mm',{1:'FAULT'+str(idx)},(28.1,6.0+(idx-4)*3),'outputs',(scx,230),mpn='PCB test pad',desc='Do not populate')

add('BZ1','KLJ-5020 3.3V','BUZZER','Saihub:KLJ-5020',{1:'V3V3',2:'BUZZ_LOW'},(6.5,50),'buzzer',(75,55),mpn='KLJ-5020',url='https://datasheet.lcsc.com/datasheet/pdf/5a334e56ebfeea427b46ed3bd7f9e2de.pdf?productCode=C556937')
add('Q1','AO3400A','AO3400A','Package_TO_SOT_SMD:SOT-23',{1:'BUZZ_GATE',2:'GND',3:'BUZZ_LOW'},(17,45.5),'buzzer',(175,55),url='https://www.aosmd.com/sites/default/files/res/datasheets/AO3400A.pdf')
add('D3','SS14','D','Diode_SMD:D_SMA',{1:'V3V3',2:'BUZZ_LOW'},(14.8,48),'buzzer',(75,100),url='https://www.diodes.com/assets/Datasheets/ds23001.pdf')
r('R11','100','BUZZ_PWM','BUZZ_GATE',(20,43),'buzzer',(265,40))
r('R12','100k','BUZZ_GATE','GND',(20,46),'buzzer',(265,75))
c('C13','100n','V3V3','GND',(2.1,44.2),'buzzer',(175,105),rot=90)

# Compact top-only placement: header, small side-push BOOT, USB-C left to right.
# Coordinates are board-local; the switch actuator points toward y=0.
placement = {'J2': (1.8, 1.5, 90),
 'SW1': (34.35, 1.75, 180),
 'J1': (42.6, 3.5, 180),
 'U3': (14.5, 13.6, 0),
 'C6': (2.5, 5.5, 90),
 'C7': (2.5, 9, 90),
 'R5': (2.5, 12.5, 90),
 'C8': (2.5, 15.8, 90),
 'R3': (2.5, 18.5, 0),
 'R4': (2.5, 20.3, 0),
 'U4': (27.65, 5.7, 90),
 'C9': (24.95, 5.6, 90),
 'R7': (30.3, 5.2, 90),
 'R9': (30.3, 8.4, 90),
 'C11': (27.3, 8.6, 0),
 'U5': (27.65, 12.2, 90),
 'C10': (24.95, 12.1, 90),
 'R8': (30.3, 11.6, 90),
 'R10': (30.3, 14.8, 90),
 'C12': (27.3, 15.2, 0),
 'TP1': (1.2, 23.1, 0),
 'TP2': (3.4, 23.1, 0),
 'TP3': (5.6, 25.1, 0),
 'TP4': (7.8, 25.1, 0),
 'TP5': (10, 25.1, 0),
 'TP6': (12.2, 25.1, 0),
 'R6': (2.5, 27, 0),
 'R13': (6, 27, 0),
 'R14': (9.5, 27, 0),
 'U1': (33.5, 5.7, 90),
 'R1': (36.4, 5.05, 90),
 'R2': (36.4, 8.25, 90),
 'C1': (33.3, 8.6, 0),
 'F1': (39.7, 9.6, 0),
 'D2': (44.8, 9.6, 0),
 'U2': (38.3, 14, 180),
 'D1': (45.5, 14.5, -90),
 'L1': (44.1, 24.4, 180),
 'C2': (38.3, 19.4, 180),
 'C3': (42, 19.4, 90),
 'C4': (35.8, 24.7, 180),
 'C5': (35.8, 22.2, 0),
 'R15': (32.4, 14.4, 90),
 'R16': (32.4, 11.2, 90),
 'C16': (32.4, 17.6, -90),
 'R17': (32.4, 20.8, -90),
 'C15': (32.4, 24, 90),
 'R18': (45.7, 19.2, 0),
 'C14': (39.1, 25.7, 90),
 'BZ1': (28.1, 23.8, 90),
 'D3': (27.8, 18.2, 0),
 'Q1': (20.5, 25.7, 0),
 'R11': (16.7, 25.1, 0),
 'C13': (16.7, 26.9, 0),
 'R12': (23.4, 25.5, 90)}
for a in parts:
    a['pos']=placement[a['ref']][:2]
    a['rot']=placement[a['ref']][2]
    a['side']='top'
    if a['ref'].startswith('TP'):
        a['foot']='TestPoint:TestPoint_Pad_D1.0mm'

def custom_fp(name,body,pads,desc):
    s=f'(footprint {q(name)} (version 20241229) (generator "pcbnew") (layer "F.Cu") (descr {q(desc)}) (attr smd)'
    s+='(property "Reference" "REF**" (at 0 -11) (layer "F.SilkS") (effects (font (size 0.8 0.8) (thickness 0.12))))'
    s+=f'(property "Value" {q(name)} (at 0 11) (layer "F.Fab") (effects (font (size 0.8 0.8) (thickness 0.12))))'
    x1,y1,x2,y2=body
    for layer,offset,width in [('F.Fab',0,0.1),('F.CrtYd',0.6,0.05)]:
        s+=f'(fp_rect (start {x1-offset} {y1-offset}) (end {x2+offset} {y2+offset}) (stroke (width {width}) (type default)) (fill none) (layer {q(layer)}))'
    # Corner mark does not intrude on solder lands.
    s+=f'(fp_circle (center {x1+1.5} {y1+1.5}) (end {x1+1.9} {y1+1.5}) (stroke (width 0.15) (type default)) (fill none) (layer "F.SilkS"))'
    for num,x,y,w,h,layers in pads:
        s+=f'(pad {q(num)} smd rect (at {x} {y}) (size {w} {h}) (layers {layers}))'
    s+=')'
    (FP/(name+'.kicad_mod')).write_text(s)

# DOIT Fig 3.3: lands extend 0.8 mm inwards and 0.4 mm outwards;
# bottom pad centre 1.5 mm from bottom, 1.27 mm pitch; EP centre
# 8.2 mm from right edge and 10.9 mm from bottom.
pads=[]
for i in range(14):
    y=9.6-1.5-(13-i)*1.27
    pads.append((str(i+1),-8.8,y,1.2,.85,'"F.Cu" "F.Paste" "F.Mask"'))
    pads.append((str(28-i),8.8,y,1.2,.85,'"F.Cu" "F.Paste" "F.Mask"'))
pads.append(('29',.8,-1.3,4.5,4.5,'"F.Cu" "F.Mask"'))
# Nine reduced paste apertures over the exposed ground pad (44% coverage).
for dx in [-1.5,0,1.5]:
    for dy in [-1.5,0,1.5]: pads.append(('',.8+dx,-1.3+dy,1,1,'"F.Paste"'))
custom_fp('ESPC5-32E-H4',(-9,-9.6,9,9.6),pads,'DOIT ESPC5-32E-H4, datasheet v1.0 Fig 3.3; external U.FL antenna')
custom_fp('KLJ-5020',(-3.15,-2.5,3.15,2.5),[(num,x,y,1.95,1.4,'"F.Cu" "F.Paste" "F.Mask"') for num,x,y in [('1',-2.175,-1.56),('2',2.175,-1.56),('',-2.175,1.56)]],'KELIKING KLJ-5020 v3.1 page 6 top-view lands: 6.3 x 4.52 span, 2.4 x 1.72 gaps; third pad mechanical only')

# SGMICRO recommended lands: 5.56 row centres, 1.91 x 0.61 leads;
# EP 2.413 x 3.302 after rotating the manufacturer's drawing 90 degrees.
sgm_pads=[]
for i in range(4):
    sgm_pads.append((str(i+1),-2.78,-1.905+i*1.27,1.91,.61,'"F.Cu" "F.Paste" "F.Mask"'))
    sgm_pads.append((str(8-i),2.78,-1.905+i*1.27,1.91,.61,'"F.Cu" "F.Paste" "F.Mask"'))
sgm_pads.append(('9',0,0,2.413,3.302,'"F.Cu" "F.Mask"'))
for x in [-.6,.6]:
    for y in [-.8,.8]:sgm_pads.append(('',x,y,.85,1.15,'"F.Paste"'))
custom_fp('SGM6232_SOIC8_EP',(-3.735,-2.55,3.735,2.55),sgm_pads,'SGMICRO TX00013.000 SOIC-8 EP; datasheet recommended lands')
custom_fp('SRP5030TA',(-3.25,-2.7,3.25,2.7),[('1',-2.25,0,2,1.8,'"F.Cu" "F.Paste" "F.Mask"'),('2',2.25,0,2,1.8,'"F.Cu" "F.Paste" "F.Mask"')],'Bourns SRP5030TA recommended land: 6.5 span, 2.5 gap, 1.8 width')

# Copy every used footprint into a project-local library.
for a in parts:
    lib,name=a['foot'].split(':')
    if lib!='Saihub': shutil.copyfile(KICAD/'footprints'/(lib+'.pretty')/(name+'.kicad_mod'),FP/(name+'.kicad_mod'))
    local=p.FootprintLoad(str(FP),name)
    for item in local.GraphicalItems():
        if item.GetLayer()==p.F_SilkS: item.SetLayer(p.F_Fab)
    p.FootprintSave(str(FP),local)
    a['foot']='Saihub:'+name
(LIB/'Saihub.kicad_sym').write_text('(kicad_symbol_lib (version 20241209) (generator "kicad_symbol_editor")\n'+'\n'.join(v['text'] for v in symbols.values())+'\n)')
(ROOT/'sym-lib-table').write_text('(sym_lib_table (version 7) (lib (name "Saihub") (type "KiCad") (uri "${KIPRJMOD}/libraries/Saihub.kicad_sym") (options "") (descr "SAIHub-Mini symbols")))\n')
(ROOT/'fp-lib-table').write_text('(fp_lib_table (version 7) (lib (name "Saihub") (type "KiCad") (uri "${KIPRJMOD}/libraries/Saihub.pretty") (options "") (descr "Project-local footprints")))\n')

rootid=uid('root')

board=p.BOARD()
settings=board.GetDesignSettings()
settings.SetBoardThickness(mm(1.6))
settings.m_MinClearance=mm(.15)
settings.m_TrackMinWidth=mm(.15)
settings.m_ViasMinSize=mm(.6)
settings.m_MinThroughDrill=mm(.3)
settings.m_HoleToHoleMin=mm(.25)
settings.m_CopperEdgeClearance=mm(.25)
nets={}
for name in sorted({n for a in parts for n in a['nets'].values() if n}):
    nn=p.NETINFO_ITEM(board,name);board.Add(nn);nets[name]=nn
footprints={}
for a in parts:
    fp=p.FootprintLoad(str(FP),a['foot'].split(':')[1]);fp.SetReference(a['ref']);fp.SetValue(a['value']);fp.SetFPID(p.LIB_ID('Saihub',a['foot'].split(':')[1]))
    fp.SetUuid(p.KIID(a['uuid']))
    fp.SetPath(p.KIID_PATH('/'+rootid+'/'+a['uuid']))
    board.Add(fp)
    fp.SetPosition(xy(*a['pos']))
    if a['side']=='bottom': fp.Flip(fp.GetPosition(),False)
    fp.SetOrientationDegrees(a['rot']);fp.Value().SetVisible(False)
    # Keep assembly labels centered and readable in the compact placement.
    for item in list(fp.GraphicalItems()):
        if hasattr(item,'GetText') and item.GetText() in ['${REFERENCE}','%R']:
            item.SetText('')
        elif item.GetLayer()==p.F_SilkS: item.SetLayer(p.F_Fab)
    size=.85 if a['ref'] in ['U2','U3','L1','BZ1','J1','J2'] else .55
    fp.Reference().SetTextSize(xy(size,size));fp.Reference().SetTextThickness(mm(.1))
    fp.Reference().SetVisible(True);fp.Reference().SetLayer(p.F_Fab)
    fp.Reference().SetPosition(fp.GetPosition());fp.Reference().SetTextAngle(p.EDA_ANGLE(0,p.DEGREES_T))
    for pad in fp.Pads():
        net=a['nets'].get(pad.GetNumber())
        if net: pad.SetNet(nets[net])
    footprints[a['ref']]=fp
for start,end in [((0,0),(W,0)),((W,0),(W,H)),((W,H),(0,H)),((0,H),(0,0))]:
    e=p.PCB_SHAPE();e.SetShape(p.SHAPE_T_SEGMENT);e.SetStart(xy(*start));e.SetEnd(xy(*end));e.SetLayer(p.Edge_Cuts);e.SetWidth(mm(.05));board.Add(e)

def label(txt,x,y,size=.8,layer=p.F_SilkS,angle=0):
    t=p.PCB_TEXT(board);t.SetText(txt);t.SetPosition(xy(x,y));t.SetTextSize(xy(size,size));t.SetTextThickness(mm(.12));t.SetLayer(layer);t.SetTextAngle(p.EDA_ANGLE(angle,p.DEGREES_T));
    if layer==p.B_SilkS:t.SetMirrored(True)
    board.Add(t)
for i,txt in enumerate(['3V3','G','5V','G','0','1','2','3','4','5','6','7']): label(txt,1.8+2.54*i,3.3,.65)
apply_board_identity(board)
label('5V 3A',42.6,5,.85,p.B_SilkS)
label('BOOT',34.35,4.5,.65,p.B_SilkS)

# Project settings explicitly define signals vs. power copper widths.
project={'meta':{'filename':'saihub.kicad_pro','version':1},'board':{'design_settings':{'rules':{'min_clearance':.15,'min_track_width':.15,'min_via_diameter':.6,'min_through_hole_diameter':.3,'min_hole_to_hole':.25,'min_copper_edge_clearance':.25,'min_hole_clearance':.15,'min_text_height':.6},'rule_severities':{'silk_over_copper':'warning','silk_overlap':'warning'},'defaults':{'board_outline_line_width':.05,'copper_line_width':.2}}},'net_settings':{'classes':[{'name':'Default','clearance':.15,'track_width':.2,'via_diameter':.6,'via_drill':.3,'microvia_diameter':.3,'microvia_drill':.1,'diff_pair_width':.25,'diff_pair_gap':.15,'diff_pair_via_gap':.25},{'name':'Power','clearance':.2,'track_width':.6,'via_diameter':.8,'via_drill':.4,'microvia_diameter':.3,'microvia_drill':.1,'diff_pair_width':.25,'diff_pair_gap':.15,'diff_pair_via_gap':.25}],'netclass_assignments':{},'netclass_patterns':[{'netclass':'Power','pattern':n} for n in ['VBUS','V5','V3V3','V3_SW','V5_SW','SW']],'meta':{'version':4}},'schematic':{'annotate_start_num':0,'drawing':{'default_line_thickness':6.0},'meta':{'version':1}}}
(ROOT/'saihub.kicad_pro').write_text(json.dumps(project,indent=2)+'\n')
(ROOT/'agent/routing/saihub-unrouted.kicad_pro').write_text(json.dumps(project,indent=2)+'\n')
p.SaveBoard(str(ROOT/'agent/routing/saihub-unrouted.kicad_pcb'),board)
(ROOT/'agent/routing/saihub-unrouted.kicad_pro').write_text(json.dumps(project,indent=2)+'\n')
(ROOT/'agent/design.json').write_text(json.dumps({**identity(),'status':'placement-only-unrouted','board_mm':[W,H],'input':{'voltage_v':5,'source_current_a':3},'channel_limit_nominal_a':1,'assembly':'top-only','parts':parts},indent=2)+'\n')
print(f'Generated {len(parts)} components, {len(nets)} nets, one schematic sheet, {W} x {H} mm placement.')

import runpy
runpy.run_path(str(ROOT/'agent/scripts/schematic.py'),run_name='__main__')
