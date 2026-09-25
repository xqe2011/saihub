#!/usr/bin/env python3
"""One-page, boxed functional schematic; no KiCad Python dependency required."""
import json, math, re, uuid
from identity import identity
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
for directory in ('agent/validation', 'agent/previews', 'agent/routing', 'docs'):
    (ROOT/directory).mkdir(parents=True, exist_ok=True)
NS=uuid.UUID('8ce59343-cd0c-4971-923b-6d37131b8b35')
def uid(s):return str(uuid.uuid5(NS,s))
def q(s):return json.dumps(str(s))
def grid(x):return round(round(x/1.27)*1.27,4)
def effects(size=1,extra=''):return f'(effects (font (size {size} {size})) {extra})'
def parse(s):
    tokens=re.findall(r'"(?:\\.|[^"\\])*"|[()]|[^\s()]+',s);stack=[];root=[];cur=root
    for t in tokens:
        if t=='(':n=[];cur.append(n);stack.append(cur);cur=n
        elif t==')':cur=stack.pop()
        else:cur.append(t)
    return root[0]
def dump(a):return '('+' '.join(dump(x) if isinstance(x,list) else x for x in a)+')'
def child(a,k):return next(x for x in a if isinstance(x,list) and x[0]==k)
def children(a,k):return [x for x in a if isinstance(x,list) and x[0]==k]
def line(points):return '(polyline (pts '+''.join(f'(xy {x} {y})' for x,y in points)+') (stroke (width .254) (type default)) (fill (type none)))'
def circle(x,y,r):return f'(circle (center {x} {y}) (radius {r}) (stroke (width .254) (type default)) (fill (type none)))'
lib=parse((ROOT/'libraries/Saihub.kicad_sym').read_text())
syms={json.loads(s[1]):s for s in children(lib,'symbol')}
graphics={
 'CP':line([(-2.54,0),(-.635,0)])+line([(.635,0),(2.54,0)])+line([(-.635,-1.8),(-.635,1.8)])+line([(.635,-1.8),(.635,1.8)])+line([(-2.2,1.5),(-2.2,2.5)])+line([(-2.7,2),(-1.7,2)]),
 'D':line([(-2.54,0),(-1.27,0)])+line([(-1.27,-1.5),(-1.27,1.5)])+line([(-1.27,0),(1.27,1.5),(1.27,-1.5),(-1.27,0)])+line([(1.27,0),(2.54,0)]),
 'L':''.join(f'(arc (start {x} 0) (mid {x+.635} .635) (end {x+1.27} 0) (stroke (width .254) (type default)) (fill (type none)))' for x in [-2.54,-1.27,0,1.27]),
 'BOOT':circle(-2.54,0,.35)+circle(2.54,0,.35)+line([(-2.54,0),(2.2,1.6)]),
 'BUZZER':circle(0,0,2.5)+line([(-2.54,0),(-2.3,0)])+line([(2.3,0),(2.54,0)])+line([(-1.5,1),(-1.5,2)])+line([(-2,1.5),(-1,1.5)]),
 'F':line([(-2.54,0),(2.54,0)])+'(rectangle (start -2.54 -1) (end 2.54 1) (stroke (width .254) (type default)) (fill (type none)))',
 'TP':circle(-2.0,0,.55)+line([(-2.54,0),(-2,0)]),
 'AO3400A':line([(-2.54,0),(-1.5,0)])+line([(-1.5,-2),(-1.5,2)])+line([(0,-2.54),(0,2.54)])+line([(0,2.54),(2.54,2.54)])+line([(0,-2.54),(2.54,-2.54)])+line([(0,0),(2.54,0),(2.54,-2.54)])+line([(.4,0),(1.4,.6),(1.4,-.6),(.4,0)]),
}
for name,g in graphics.items():
    s=syms[name];body=next(x for x in children(s,'symbol') if json.loads(x[1]).endswith('_0_1'))
    body[:]=['symbol',body[1]]+parse('(dummy '+g+')')[1:]
fet=syms['AO3400A'];pinbody=next(x for x in children(fet,'symbol') if json.loads(x[1]).endswith('_1_1'))
pinbody[:]=['symbol',pinbody[1]]
for num,name,typ,x,y,ang in [(1,'G','input',-5.08,0,0),(3,'D','passive',2.54,5.08,270),(2,'S','passive',2.54,-5.08,90)]:
    pinbody.append(parse(f'(pin {typ} line (at {x} {y} {ang}) (length 2.54) (name "{name}" {effects()}) (number "{num}" {effects()}))'))
flag=syms['PWR_FLAG'];body=children(flag,'symbol');body[0][:]=['symbol',body[0][1]]+parse('(dummy '+line([(0,0),(0,1.27),(-1.27,1.27),(0,2.54),(1.27,1.27),(0,1.27)])+')')[1:]
child(children(body[1],'pin')[0],'at')[1:]=['0','0','90'];child(children(body[1],'pin')[0],'length')[1]='0'
(ROOT/'libraries/Saihub.kicad_sym').write_text(dump(lib)+'\n')

data=json.loads((ROOT/'agent/design.json').read_text());parts={a['ref']:a for a in data['parts']}
positions={
 'U3':(85,65,0),'C6':(29,35,270),'C7':(43,35,270),
 'R5':(30.48,66.04,270),'C8':(43.18,76.2,270),'R6':(144,43,270),'SW1':(165,60,0),
 'TP1':(143,95,0),'TP2':(180,95,0),'TP3':(143,115,0),'TP4':(180,115,0),
 'J1':(242,60,0),'R1':(282,37,0),'R2':(282,53,0),'U1':(323,78,0),
 'R3':(366,73.66,0),'R4':(366,88.9,0),'F1':(278,106,0),'D2':(300,112,270),'C1':(337,109,270),
 'U2':(104,160,0),'C2':(36,170,270),'C3':(57,170,270),'D1':(131,170,270),
 'L1':(157,158.75,0),'C4':(188,170,270),'C5':(216,170,270),
 'R15':(161,192,0),'R16':(203,192,0),'R17':(117,192,0),'R18':(147,148,0),
 'C14':(190,148,0),'C15':(29,192,0),'C16':(73,192,0),
 'U4':(270,159,0),'U5':(366,159,0),'R7':(285,184,270),'R8':(381,184,270),
 'R9':(249,179,270),'R10':(345,179,270),'C9':(249,204,270),'C10':(345,204,270),
 'C11':(298,168,270),'C12':(394,168,270),
 'TP5':(297,238,0),'TP6':(393,238,0),'J2':(187,243,0),
 'BZ1':(45,232,0),'Q1':(83,250,0),'D3':(60,221,0),'R11':(47,250,0),'R12':(66,260,270),'C13':(122,237,270),
}
positions.update({'U6': (470, 50, 0), 'U7': (470, 110, 0), 'U10': (458, 180, 0), 'C21': (544, 180, 270)})
for a in parts.values():a['sch']=[grid(v) for v in positions[a['ref']][:2]];a['angle']=positions[a['ref']][2]
for i,net in enumerate(['VBUS','V5','V3V3','GND']):
    ref='#FLG0'+str(i+1)
    parts[ref]=dict(ref=ref,value='PWR_FLAG',sym='PWR_FLAG',nets={'1':net},sch=[grid(315+i*24),grid(35)],angle=0,uuid=uid('flag'+net),foot='',mpn='',url='')
def pin(ref,num):
    a=parts[ref];sym=syms[a['sym']]
    pp=next(z for un in children(sym,'symbol') for z in children(un,'pin') if json.loads(child(z,'number')[1])==str(num))
    loc=child(pp,'at');x,y,ang=map(float,loc[1:4]);theta=math.radians(a['angle']);xx=x*math.cos(theta)-y*math.sin(theta);yy=x*math.sin(theta)+y*math.cos(theta)
    return round(a['sch'][0]+xx,4),round(a['sch'][1]-yy,4),(ang+a['angle'])%360
seq=0;items=[];handled=set();junctions=set()
def serial(s):
    global seq
    seq+=1;return uid(f'onepage-{seq}-{s}')
def wire(points):
    for a,b in zip(points,points[1:]):
        if a==b:continue
        items.append(f'(wire (pts (xy {a[0]} {a[1]}) (xy {b[0]} {b[1]})) (stroke (width 0) (type default)) (uuid "{serial("wire")}"))')
def pt(ref,num):return pin(ref,num)[:2]
def mark(ref,num):handled.add((ref,str(num)));return pt(ref,num)
def junction(x,y):
    if (x,y) not in junctions:
        items.append(f'(junction (at {x} {y}) (diameter 0) (color 0 0 0 0) (uuid "{serial("junction")}"))');junctions.add((x,y))
def label(net,point,angle=0):
    x,y=point
    items.append(f'(global_label {q(net)} (shape input) (at {x} {y} {angle}) {effects(.95,"(justify "+("right" if angle==0 else "left")+")")} (uuid "{serial(net)}") (property "Intersheetrefs" "${{INTERSHEET_REFS}}" (at {x} {y} 0) {effects(1,"(hide yes)")}))')
def stub(ref,num,net=None,length=3.81):
    a=parts[ref];net=net or a['nets'][str(num)];x,y,ang=pin(ref,num);theta=math.radians(ang);end=(round(x-length*math.cos(theta),4),round(y+length*math.sin(theta),4))
    mark(ref,num);wire([(x,y),end]);label(net,end,0 if ang==0 else 180)
def txt(s,x,y,size=1.27):items.append(f'(text {q(s)} (at {x} {y} 0) {effects(size,"(justify left)")} (uuid "{serial(s)}"))')
def box(x1,y1,x2,y2,title):
    items.append(f'(polyline (pts (xy {x1} {y1}) (xy {x2} {y1}) (xy {x2} {y2}) (xy {x1} {y2}) (xy {x1} {y1})) (stroke (width .254) (type default)) (fill (type none)) (uuid "{serial(title)}"))')
    txt(title,x1+3,y1+4,1.5)
box(12,12,208,130,'01  ESP32-C5 / BOOT / RESET')
box(212,12,408,130,'02  USB-C / INPUT PROTECTION')
box(12,134,232,210,'03  SGM6232 / ALWAYS-ON 3.3 V')
box(236,134,408,249,'04  SWITCHED OUTPUTS / 1 A NOMINAL LIMIT')
box(12,214,144,275,'05  PASSIVE 5020 BUZZER')
box(148,214,232,275,'06  12-PIN MALE HEADER')
box(412,12,580,275,'07  HEADER ESD / SHUNT PROTECTION')
txt('U6/U7: four channels each; no supply or bypass needed.',418,225,1.05)
txt('IO0-IO7: 3.3 V only; final-board ESD test required.',418,242,1.05)
txt('U10: switched rails. Power current bypasses the array.',418,249,1.05)
txt('Place arrays and bypass capacitors directly beside J2.',418,256,1.05)
txt('Short, wide ground returns; qualify ESD on final PCB.',418,263,1.05)
txt('48 x 28 mm PCB - top-side component assembly.',17,290,1.5)

# MCU bypass capacitors share actual supply / return buses.
for num,net,yy in [(1,'V3V3',grid(25)),(2,'GND',grid(46))]:
    pp=[mark(r,num) for r in ['C6','C7']]
    for x,y in pp:wire([(x,y),(x,yy)])
    wire([(pp[0][0],yy),(pp[1][0],yy)]);label(net,(pp[0][0],yy))
# EN RC, with explicit reset recovery pads nearby.
stub('R5',1);a=mark('R5',2);b=mark('C8',1);wire([a,(b[0],a[1]),b]);label('EN',(b[0],a[1]),180);stub('C8',2)
a=mark('R6',2);b=mark('SW1',1);wire([a,(a[0],b[1]),b]);label('BOOT',(a[0],b[1]));stub('R6',1);stub('SW1',2)
txt('Hold BOOT, then power on or short EN to GND.',126,77,1.05)
txt('SW1: EVQP7A01P / small side-push / front edge',126,82,1.05)
txt('External dual-band U.FL antenna; 3.3 V GPIO only.',17,124,1.15)

# USB duplicate contacts are tied by visible buses.
for nums,net,busx in [(['A4','A9','B4','B9'],'VBUS',grid(223)),(['A1','A12','B1','B12','SH'],'GND',grid(223))]:
    pp=[mark('J1',n) for n in nums]
    for x,y in pp:wire([(x,y),(busx,y)])
    ys=sorted(y for x,y in pp);wire([(busx,y) for y in ys]);label(net,(busx,ys[0]))
    for y in ys[1:-1]:junction(busx,y)
for nums,net,busx in [(['A6','B6'],'USB_DP',grid(261)),(['A7','B7'],'USB_DM',grid(261))]:
    pp=[mark('J1',n) for n in nums]
    for x,y in pp:wire([(x,y),(busx,y)])
    wire([(busx,pp[0][1]),(busx,pp[1][1])]);label(net,(busx,pp[0][1]),180)
for j,r,bx in [('A5','R1',grid(266)),('B5','R2',grid(270))]:
    a=mark('J1',j);b=mark(r,1);wire([a,(bx,a[1]),(bx,b[1]),b]);label(parts[r]['nets']['1'],(bx,b[1]),180);stub(r,2)
for pinno,ref in [(6,'R3'),(4,'R4')]:
    a=mark('U1',pinno);b=mark(ref,1);bx=grid(348);wire([a,(bx,a[1]),(bx,b[1]),b]);label(parts[ref]['nets']['1'],(bx,b[1]),180);stub(ref,2)
a=mark('F1',2);b=mark('D2',1);wire([a,(b[0],a[1]),b]);label('V5',(b[0],a[1]),180);stub('F1',1);stub('D2',2)
txt('5 V / 3 A supply. No PD or OTG. External loads OFF on PC USB.',217,125,1.05)

# Buck power loop; auxiliary bootstrap, soft-start, divider and compensation
# are shown beneath/above the power path with matching named connections.
vin=mark('U2',2);vy=vin[1];ground_y=grid(182)
capsin=[mark(r,1) for r in ['C2','C3']]
wire([(capsin[0][0],vy),vin]);label('V5',(capsin[0][0],vy))
for x,y in capsin:wire([(x,vy),(x,y)]);junction(x,vy)
sw=mark('U2',3);lin=mark('L1',1);wire([sw,lin])
diode=mark('D1',1);wire([diode,(diode[0],sw[1])]);junction(diode[0],sw[1]);label('SW',(diode[0],sw[1]),180)
out=mark('L1',2);co=[mark(r,1) for r in ['C4','C5']]
wire([out,(co[-1][0],out[1])]);label('V3V3',(co[-1][0],out[1]),180)
for x,y in co:wire([(x,out[1]),(x,y)]);junction(x,out[1])
gpts=[mark(r,2) for r in ['C2','C3','D1','C4','C5']]
for x,y in gpts:wire([(x,y),(x,ground_y)])
wire([(gpts[0][0],ground_y),(gpts[-1][0],ground_y)]);label('GND',(gpts[0][0],ground_y))
for x,y in gpts[1:-1]:junction(x,ground_y)
txt('EN open: auto-start. EP to GND. FB divider 33k / 10.5k = 3.314 V.',17,205,1.05)

# Rail switches: visible output capacitor and enable pulldown connections.
for u,ce,rpd in [('U4','C11','R9'),('U5','C12','R10')]:
    a=mark(u,6);b=mark(ce,1);wire([a,(b[0],a[1]),b]);label(parts[u]['nets']['6'],(b[0],a[1]),180);stub(ce,2)
    a=mark(u,3);b=mark(rpd,1);wire([a,(b[0],a[1]),b]);label(parts[u]['nets']['3'],(b[0],a[1]));stub(rpd,2)
txt('Fault inputs: firmware must enable internal pullups.',240,222,1.02)
txt('FAULT4 -> input 15; FAULT5 -> input 3. Active LOW.',240,251,1.02)
txt('26.1k ILIM: nominal 0.989 A; bounds 0.908-1.081 A.',240,244,1.02)

# Buzzer: low-side MOSFET and flyback diode, independent of switched outputs.
a=mark('BZ1',1);d=mark('D3',1);sx=grid(30);sy=d[1]
wire([a,(sx,a[1]),(sx,sy),d]);label('V3V3',(sx,sy))
b=mark('BZ1',2);d=mark('D3',2);dr=mark('Q1',3);bx=dr[0]
wire([d,(bx,d[1]),dr]);wire([b,(bx,b[1])]);junction(bx,b[1]);label('BUZZ_LOW',(bx,b[1]),180)
g=mark('Q1',1);r=mark('R11',2);pd=mark('R12',1);wire([r,g]);wire([pd,(pd[0],g[1])]);junction(pd[0],g[1]);label('BUZZ_GATE',(pd[0],g[1]),180);stub('R11',1);stub('R12',2);stub('Q1',2)
txt('4 kHz / 50% duty. GPIO12 reserved.',17,272,1.02)

# Place symbols and automatically label only remaining inter-block connections.
for ref,a in parts.items():
    x,y=a['sch'];angle=a['angle'];isflag=ref.startswith('#');sym=syms[a['sym']]
    inst=f'(symbol (lib_id "Saihub:{a["sym"]}") (at {x} {y} {angle}) (unit 1) (in_bom {"no" if isflag or ref.startswith("TP") else "yes"}) (on_board {"no" if isflag else "yes"}) (dnp no) (uuid "{a["uuid"]}")'
    h=6 if a['sym']=='AO3400A' else 20.32 if a['sym']=='ESPC5_32E_H4' else 12.7 if a['sym']=='USB_C' else 11.43 if a['sym']=='HEADER_12' else 8.89 if a['sym'] in ['SGM6232','TPD4E05U06'] else 5.08 if a['sym'] in ['TPS2553','USBLC6_2SC6'] else 2.54
    vertical=angle in [90,270]
    val=a['value']
    for name,v,xx,yy,hide in [('Reference',ref,x+3.81 if vertical else x,y-1.27 if vertical else y-h-2.54,isflag),('Value',val,x+3.81 if vertical else x,y+1.27 if vertical else y+h+2.54,isflag),('Footprint',a['foot'],x,y,True),('Datasheet',a['url'],x,y,True),('MPN',a['mpn'],x,y,True)]:
        extra='(hide yes)' if hide else '(justify left)' if vertical else ''
        if ref=='Q1' and name in ['Reference','Value']:xx,yy=96,246 if name=='Reference' else 249
        inst+=f'(property {q(name)} {q(v)} (at {xx} {yy} {90 if vertical else 0}) {effects(.95,extra)})'
    for un in children(sym,'symbol'):
        for pinobj in children(un,'pin'):
            num=json.loads(child(pinobj,'number')[1]);inst+=f'(pin {q(num)} (uuid "{uid(ref+"-"+num)}"))'
            if (ref,num) in handled:continue
            net=a['nets'].get(num)
            if net is None:
                xx,yy,_=pin(ref,num);items.append(f'(no_connect (at {xx} {yy}) (uuid "{serial("nc")}"))')
            else:stub(ref,num)
    inst+=f'(instances (project "saihub" (path "/{uid("root")}" (reference {q(ref)}) (unit 1)))))'
    items.append(inst)
txt('PROTOTYPE VALIDATION',238,255,1.02)
txt('Check startup, dropout, ripple, heat.',238,260,.95)
txt('Use specified 5 V / 3 A adapter.',238,265,.95)
txt('Verify current limit and both loaded rails.',238,270,.95)
info = identity()
text = '(kicad_sch (version 20250114) (generator "eeschema") (uuid '+q(uid('root'))+') (paper "A2")'
text += '(title_block (title '+q(info['name']+' - ESP32-C5 GPIO CONTROLLER')+') (date '+q(info['version'])+') (rev '+q(info['revision'])+') (company '+q(info['author']+' / '+info['co_author'])+') (comment 1 "48 x 28 mm / 2 layers / SGM6232 / prototype"))'
text+='(lib_symbols '+''.join(dump(s).replace('(symbol '+q(name),'(symbol '+q('Saihub:'+name),1) for name,s in syms.items())+')'
text+='\n'.join(items)+')\n'
(ROOT/'saihub.kicad_sch').write_text(text)
for name in ['usb','power','mcu','outputs','buzzer']:
    old=ROOT/(name+'.kicad_sch')
    if old.exists():old.unlink()
print('Created one-page A2 schematic with seven wired functional sections.')
