"""Build README.pdf from README.md (+ the Phase 1 / Phase 2 docs as appendices).

    python docs/make_readme_pdf.py [path/to/chrome]

Needs the Python 'markdown' package and Google Chrome or Chromium (headless).
On Linux:  pip install markdown && python3 docs/make_readme_pdf.py chromium
"""
import pathlib
import shutil
import subprocess
import sys

import markdown

ROOT = pathlib.Path(__file__).resolve().parent.parent
PARTS = [
    ("README.md", None),
    ("docs/interpreter.md", "Appendix A"),
    ("docs/codegen.md", "Appendix B"),
]

CSS = """
@page { size: A4; margin: 18mm 16mm; }
body { font-family: "Segoe UI", "DejaVu Sans", Arial, sans-serif; font-size: 10.5pt;
       line-height: 1.45; color: #1a1a1a; }
h1 { font-size: 21pt; border-bottom: 2px solid #2b5797; padding-bottom: 4px; color: #1d3f72; }
h2 { font-size: 15pt; color: #1d3f72; border-bottom: 1px solid #ccd6e6; padding-bottom: 2px;
     margin-top: 22px; page-break-after: avoid; }
h3 { font-size: 12pt; color: #2b5797; page-break-after: avoid; }
code { font-family: "Cascadia Mono", Consolas, "DejaVu Sans Mono", monospace; font-size: 9pt;
       background: #f2f4f7; padding: 1px 3px; border-radius: 3px; }
pre { background: #f6f8fa; border: 1px solid #dde3ea; border-radius: 4px; padding: 8px 10px;
      overflow-wrap: anywhere; white-space: pre-wrap; page-break-inside: avoid; line-height: 1.3; }
pre code { background: none; padding: 0; font-size: 8.6pt; }
table { border-collapse: collapse; margin: 8px 0; width: 100%; page-break-inside: avoid; }
th, td { border: 1px solid #cfd7e2; padding: 4px 7px; text-align: left; vertical-align: top; }
th { background: #e8eef7; }
tr:nth-child(even) td { background: #fafbfd; }
a { color: #2b5797; text-decoration: none; }
.appendix { page-break-before: always; }
.appendix-label { color: #777; font-size: 10pt; text-transform: uppercase; letter-spacing: 1px; }
"""


def render(path: str) -> str:
    text = (ROOT / path).read_text(encoding="utf-8")
    return markdown.markdown(text, extensions=["tables", "fenced_code", "sane_lists"])


def main() -> None:
    body = []
    for path, label in PARTS:
        html = render(path)
        if label:
            html = f'<section class="appendix"><div class="appendix-label">{label}</div>{html}</section>'
        body.append(html)
    page = (
        '<!doctype html><html><head><meta charset="utf-8"><title>Floe — README</title>'
        f"<style>{CSS}</style></head><body>{''.join(body)}</body></html>"
    )
    html_path = ROOT / "build" / "README.html"
    html_path.parent.mkdir(exist_ok=True)
    html_path.write_text(page, encoding="utf-8")

    chrome = sys.argv[1] if len(sys.argv) > 1 else (
        shutil.which("chromium") or shutil.which("google-chrome") or shutil.which("chrome"))
    if not chrome:
        sys.exit("Chrome/Chromium not found; pass its path as the first argument")
    pdf_path = ROOT / "README.pdf"
    subprocess.run([chrome, "--headless=new", "--disable-gpu", "--no-pdf-header-footer",
                    f"--print-to-pdf={pdf_path}", html_path.as_uri()], check=True)
    print(f"wrote {pdf_path}")


if __name__ == "__main__":
    main()
