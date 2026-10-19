"""Place visible reference labels near their footprints without covering pads."""
import math
import pcbnew as p


def apply_component_outlines(board):
    """Project actual Fab geometry to silk, clipping strokes at pads and board edges."""
    for group in list(board.Groups()):
        if group.GetName() == 'Component silkscreen outlines':
            for item in list(group.GetItems()):
                group.RemoveItem(item)
                board.Delete(item)
            board.Delete(group)
    group = p.PCB_GROUP(board)
    group.SetName('Component silkscreen outlines')
    board.Add(group)

    def xy(v):
        return p.ToMM(v.x), p.ToMM(v.y)

    def outside(a, b, box):
        # Subtract the portion inside an axis-aligned keep-out from one segment.
        lo, hi = 0.0, 1.0
        for axis in (0, 1):
            delta = b[axis] - a[axis]
            if abs(delta) < 1e-10:
                if not box[axis] <= a[axis] <= box[axis+2]:
                    return [(a, b)]
            else:
                t0, t1 = sorted(((box[axis]-a[axis])/delta, (box[axis+2]-a[axis])/delta))
                lo, hi = max(lo, t0), min(hi, t1)
        if lo >= hi:
            return [(a, b)]
        at = lambda t: tuple(a[k]+t*(b[k]-a[k]) for k in (0, 1))
        return ([(a, at(lo))] if lo > 0 else []) + ([(at(hi), b)] if hi < 1 else [])

    for fab, silk, mask in [(p.F_Fab, p.F_SilkS, p.F_Mask), (p.B_Fab, p.B_SilkS, p.B_Mask)]:
        boxes = [(-100, -100, .2, 100), (47.8, -100, 100, 100),
                 (-100, -100, 100, .2), (-100, 27.8, 100, 100), (-100, 20.3, 22.05, 100)]
        for fp in board.GetFootprints():
            for pad in fp.Pads():
                if pad.IsOnLayer(mask):
                    bb = pad.GetBoundingBox()
                    margin = .15 + max(0, p.ToMM(pad.GetLocalSolderMaskMargin() or 0))
                    boxes.append((p.ToMM(bb.GetX())-margin, p.ToMM(bb.GetY())-margin,
                                  p.ToMM(bb.GetRight())+margin, p.ToMM(bb.GetBottom())+margin))
        for fp in board.GetFootprints():
            existing = {frozenset((xy(i.GetStart()), xy(i.GetEnd()))) for i in fp.GraphicalItems()
                        if isinstance(i, p.PCB_SHAPE) and i.GetLayer() == silk and i.GetShape() == p.SHAPE_T_SEGMENT}
            for item in fp.GraphicalItems():
                if item.GetLayer() != fab or not isinstance(item, p.PCB_SHAPE):
                    continue
                shape = item.GetShape()
                paths = []
                if shape == p.SHAPE_T_SEGMENT:
                    paths = [[xy(item.GetStart()), xy(item.GetEnd())]]
                elif shape == p.SHAPE_T_RECT:
                    a, b = xy(item.GetStart()), xy(item.GetEnd())
                    paths = [[a, (b[0], a[1]), b, (a[0], b[1]), a]]
                elif shape == p.SHAPE_T_CIRCLE:
                    cx, cy = xy(item.GetCenter())
                    radius = p.ToMM(item.GetRadius())
                    count = max(24, math.ceil(2*math.pi*radius/.15))
                    paths = [[(cx+radius*math.cos(2*math.pi*i/count), cy+radius*math.sin(2*math.pi*i/count))
                              for i in range(count+1)]]
                elif shape == p.SHAPE_T_POLY:
                    poly = item.GetPolyShape()
                    for idx in range(poly.OutlineCount()):
                        contour = poly.Outline(idx)
                        points = [xy(contour.CPoint(i)) for i in range(contour.PointCount())]
                        paths.append(points+[points[0]])
                else:
                    raise RuntimeError(f'Unsupported Fab geometry on {fp.GetReference()}: {shape}')
                for points in paths:
                    for start, end in zip(points, points[1:]):
                        if frozenset((start, end)) in existing:
                            continue
                        pieces = [(start, end)]
                        for box in boxes:
                            pieces = [part for a, b in pieces for part in outside(a, b, box)]
                        for a, b in pieces:
                            if math.dist(a, b) < .03:
                                continue
                            stroke = p.PCB_SHAPE(board)
                            stroke.SetShape(p.SHAPE_T_SEGMENT)
                            stroke.SetStart(p.VECTOR2I(*(round(v*1e6) for v in a)))
                            stroke.SetEnd(p.VECTOR2I(*(round(v*1e6) for v in b)))
                            stroke.SetWidth(p.FromMM(.1))
                            stroke.SetLayer(silk)
                            board.Add(stroke)
                            group.AddItem(stroke)


def apply_reference_labels(board):
    apply_component_outlines(board)
    def bounds(item, margin=0):
        box = item.GetEffectiveTextShape().BBox() if isinstance(item, p.PCB_TEXT) else item.GetBoundingBox()
        return (p.ToMM(box.GetX())-margin, p.ToMM(box.GetY())-margin,
                p.ToMM(box.GetRight())+margin, p.ToMM(box.GetBottom())+margin)

    def overlaps(a, b):
        return a[0] < b[2] and a[2] > b[0] and a[1] < b[3] and a[3] > b[1]

    obstacles_by_side = {}
    for copper, silk in [(p.F_Cu, p.F_SilkS), (p.B_Cu, p.B_SilkS)]:
        obstacles_by_side[silk] = [bounds(pad, .05) for fp in board.GetFootprints() for pad in fp.Pads() if pad.IsOnLayer(copper)]
        obstacles_by_side[silk] += [bounds(item, .12) for item in board.GetDrawings() if item.GetLayer() == silk]
        obstacles_by_side[silk] += [bounds(item, .07) for fp in board.GetFootprints()
                                    for item in fp.GraphicalItems() if item.GetLayer() == silk]
    footprints = sorted(board.GetFootprints(), key=lambda fp: fp.GetReference())
    for fp in footprints:
        text = fp.Reference()
        text.SetVisible(True)
        silk = p.B_SilkS if fp.IsFlipped() else p.F_SilkS
        obstacles = obstacles_by_side[silk]
        text.SetLayer(silk)
        text.SetMirrored(fp.IsFlipped())
        text.SetTextSize(p.VECTOR2I(p.FromMM(.6), p.FromMM(.6)))
        text.SetTextThickness(p.FromMM(.1))
        text.SetTextAngle(p.EDA_ANGLE(0, p.DEGREES_T))
        center = fp.GetPosition()
        x, y = p.ToMM(center.x), p.ToMM(center.y)
        if fp.GetReference() == 'J2':
            x += 13.97  # Header origin is pin 1; search around the body center.
        candidates = [(dx*.2, dy*.2, angle) for dx in range(-25, 26) for dy in range(-25, 26) for angle in (0, 90)]
        candidates.sort(key=lambda offset: (offset[0]**2 + offset[1]**2, offset[2], abs(offset[0]), offset[1]))
        for dx, dy, angle in candidates:
            text.SetTextAngle(p.EDA_ANGLE(angle, p.DEGREES_T))
            text.SetPosition(p.VECTOR2I(p.FromMM(x+dx), p.FromMM(y+dy)))
            box = bounds(text, .03)
            if box[0] < .2 or box[1] < .2 or box[2] > 47.8 or box[3] > 27.8:
                continue
            if box[0] < 22.05 and box[3] > 20.3:
                continue
            if not any(overlaps(box, obstacle) for obstacle in obstacles):
                obstacles.append(box)
                break
        else:
            raise RuntimeError(f'No clear silkscreen position for {fp.GetReference()}')
