"""Apply the explicitly approved identity; never derive a version from today's date."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def identity():
    return json.loads((ROOT/'agent/identity.json').read_text())

def apply_board_identity(board):
    import pcbnew as p
    info = identity()
    # Replace identity text while preserving connection labels and all copper.
    for item in list(board.GetDrawings()):
        if isinstance(item, p.PCB_TEXT) and any(tag in item.GetText() for tag in ('SAIHUB', 'SAIHub-Mini', 'GPT6-Astra', 'xqe2011')):
            board.Remove(item)
    for text, x, y in [
        (f"{info['name']} Rev {info['revision']} {info['version']}", .55, 11),
        (f"{info['author']} / {info['co_author']}", 4.15, 10.4),
    ]:
        item = p.PCB_TEXT(board)
        item.SetText(text)
        item.SetPosition(p.VECTOR2I(p.FromMM(x), p.FromMM(y)))
        item.SetTextSize(p.VECTOR2I(p.FromMM(.6), p.FromMM(.6)))
        item.SetTextThickness(p.FromMM(.1))
        item.SetTextAngle(p.EDA_ANGLE(90, p.DEGREES_T))
        item.SetLayer(p.F_SilkS)
        board.Add(item)
