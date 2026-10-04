"""Refresh the PCB reference, checked enclosure exports and hardware/docs/shell.pdf."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    root = Path(__file__).resolve().parent
    hardware = root.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pdf-python", default=os.environ.get("SHELL_PDF_PYTHON", sys.executable))
    args = parser.parse_args()
    cli = os.environ.get("KICAD_CLI") or shutil.which("kicad-cli")
    if not cli:
        candidate = Path("/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
        if candidate.exists():
            cli = str(candidate)
    if not cli:
        raise SystemExit("Set KICAD_CLI to the board export executable.")
    # Export from the routed board, never from an old placement preview.
    subprocess.run([cli, "pcb", "export", "step", "--force", "--subst-models", "--include-pads",
                    "--user-origin", "0x0mm", "-o", str(root / "reference/pcb.step"),
                    str(hardware / "saihub.kicad_pcb")], check=True)
    subprocess.run([sys.executable, str(root / "enclosure.py"), "--preview", "--check-components"], check=True)
    with tempfile.TemporaryDirectory(prefix="saihub-shell-") as temporary:
        subprocess.run([sys.executable, str(root / "render_shell_docs.py"), "--output", temporary], check=True)
        staged = Path(temporary) / "shell.pdf"
        subprocess.run([args.pdf_python, str(root / "export_shell_pdf.py"), "--assets", temporary,
                        "--output", str(staged)], check=True)
        destination = hardware / "docs/shell.pdf"
        destination.parent.mkdir(exist_ok=True)
        shutil.copy2(staged, destination)
    print(f"Updated {destination}. Render and visually inspect every PDF page before delivery.")


if __name__ == "__main__":
    main()
