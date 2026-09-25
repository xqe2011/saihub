"""Place visible reference labels near their footprints without covering pads."""
import pcbnew as p


def apply_reference_labels(board):
    def bounds(item, margin=0):
        box = item.GetEffectiveTextShape().BBox() if isinstance(item, p.PCB_TEXT) else item.GetBoundingBox()
        return (p.ToMM(box.GetX())-margin, p.ToMM(box.GetY())-margin,
                p.ToMM(box.GetRight())+margin, p.ToMM(box.GetBottom())+margin)

    def overlaps(a, b):
        return a[0] < b[2] and a[2] > b[0] and a[1] < b[3] and a[3] > b[1]

    obstacles = [bounds(pad, .05) for fp in board.GetFootprints() for pad in fp.Pads()]
    obstacles += [bounds(item, .12) for item in board.GetDrawings() if item.GetLayer() == p.F_SilkS]
    footprints = sorted(board.GetFootprints(), key=lambda fp: fp.GetReference())
    for fp in footprints:
        text = fp.Reference()
        text.SetVisible(True)
        text.SetLayer(p.F_SilkS)
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
            if not any(overlaps(box, obstacle) for obstacle in obstacles):
                obstacles.append(box)
                break
        else:
            raise RuntimeError(f'No clear silkscreen position for {fp.GetReference()}')
