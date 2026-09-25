"""Apply the explicitly approved identity; never derive a version from today's date."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def identity():
    return json.loads((ROOT/'agent/identity.json').read_text())

def apply_board_identity(board):
    import pcbnew as p
    info = identity()
    title = board.GetTitleBlock()
    title.SetTitle(info['name'])
    title.SetRevision(info['revision'])
    title.SetDate(info['version'])
    title.SetCompany(f"{info['author']} / {info['co_author']}")
    # Reuse existing text objects to preserve identifiers and avoid remove/re-add ownership issues.
    existing = [item for item in board.GetDrawings() if isinstance(item, p.PCB_TEXT)]
    for text, x, y, author in [
        (f"{info['name']} Rev {info['revision']} {info['version']}", .55, 11, False),
        (f"{info['author']} / {info['co_author']}", 4.6, 10.4, True),
    ]:
        matches = [item for item in existing if
                   (any(tag in item.GetText() for tag in ('GPT6-Astra', 'xqe2011')) if author else
                    any(tag in item.GetText() for tag in ('SAIHUB', 'SAIHub-Mini')))]
        item = matches[0] if matches else p.PCB_TEXT(board)
        item.SetText(text)
        item.SetPosition(p.VECTOR2I(p.FromMM(x), p.FromMM(y)))
        item.SetTextSize(p.VECTOR2I(p.FromMM(.6), p.FromMM(.6)))
        item.SetTextThickness(p.FromMM(.1))
        item.SetTextAngle(p.EDA_ANGLE(90, p.DEGREES_T))
        item.SetLayer(p.B_SilkS if author else p.F_SilkS)
        item.SetMirrored(author)
        if not matches:
            board.Add(item)
