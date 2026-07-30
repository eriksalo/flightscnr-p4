"""Render docs/office-guide.html to a three-page landscape PDF with headless Chrome.

Chrome refuses to load `file://` fonts and images from a `file://` document unless
launched with permissive flags, and even then the font fetch is unreliable. So this
script inlines every local asset the page references as a `data:` URI, writes the
result to a temporary file, and points Chrome at that instead. The committed HTML
stays readable and asset-free.

    python tools/build_office_guide.py
    python tools/build_office_guide.py --dry-run
    python tools/build_office_guide.py --out docs/handout.pdf
"""

from __future__ import annotations

import argparse
import base64
import logging
import mimetypes
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

logging.basicConfig(level=logging.INFO, format="%(message)s")
log = logging.getLogger("office-guide")

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = REPO_ROOT / "docs" / "office-guide.html"
DEFAULT_OUTPUT = REPO_ROOT / "docs" / "FlightScnr-office-guide.pdf"

# url('...') in CSS and href="..." on SVG <image> — both point at repo-relative files.
ASSET_PATTERNS = (
    re.compile(r"""url\(['"]?(?P<path>[^'")]+\.(?:ttf|otf|woff2?|png|jpe?g|svg))['"]?\)"""),
    re.compile(r"""href=["'](?P<path>[^"']+\.(?:png|jpe?g|svg))["']"""),
)

CHROME_CANDIDATES = (
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
)


def find_chrome() -> Path:
    """Locate a Chromium-family browser, or raise with something actionable."""
    for name in ("chrome", "google-chrome", "chromium", "msedge"):
        found = shutil.which(name)
        if found:
            return Path(found)
    for candidate in CHROME_CANDIDATES:
        path = Path(candidate)
        if path.is_file():
            return path
    raise FileNotFoundError(
        "no Chrome/Chromium/Edge binary found — install one, or put it on PATH"
    )


def to_data_uri(path: Path) -> str:
    mime, _ = mimetypes.guess_type(path.name)
    if mime is None:
        mime = "font/ttf" if path.suffix.lower() in {".ttf", ".otf"} else "application/octet-stream"
    payload = base64.b64encode(path.read_bytes()).decode("ascii")
    return f"data:{mime};base64,{payload}"


def inline_assets(html: str, base_dir: Path) -> tuple[str, int]:
    """Replace every local asset reference in `html` with a data URI."""
    inlined = 0

    def replace(match: re.Match[str]) -> str:
        nonlocal inlined
        ref = match.group("path")
        if ref.startswith(("data:", "http:", "https:")):
            return match.group(0)
        target = (base_dir / ref).resolve()
        if not target.is_file():
            raise FileNotFoundError(f"asset referenced but missing: {ref} -> {target}")
        inlined += 1
        log.info("  inlined %s (%.0f KB)", ref, target.stat().st_size / 1024)
        return match.group(0).replace(ref, to_data_uri(target))

    for pattern in ASSET_PATTERNS:
        html = pattern.sub(replace, html)
    return html, inlined


def render(source: Path, output: Path, *, dry_run: bool = False) -> None:
    if not source.is_file():
        raise FileNotFoundError(f"source page not found: {source}")

    # Chrome resolves --print-to-pdf against its own working directory, not ours, so a
    # relative --out would silently land somewhere unhelpful (or fail outright).
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    chrome = find_chrome()
    log.info("browser: %s", chrome)
    log.info("source:  %s", source)

    html, count = inline_assets(source.read_text(encoding="utf-8"), source.parent)
    log.info("inlined %d asset(s); page is now %.1f MB", count, len(html) / 1_048_576)

    if dry_run:
        log.info("dry run: not rendering %s", output)
        return

    with tempfile.TemporaryDirectory(prefix="office-guide-") as tmp:
        staged = Path(tmp) / "page.html"
        staged.write_text(html, encoding="utf-8")
        cmd = [
            str(chrome),
            "--headless=new",
            "--disable-gpu",
            "--no-sandbox",
            "--no-pdf-header-footer",
            "--virtual-time-budget=8000",
            f"--print-to-pdf={output}",
            staged.as_uri(),
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)

    if result.returncode != 0 or not output.is_file():
        log.error("chrome failed (exit %d)\n%s", result.returncode, result.stderr.strip())
        raise RuntimeError("PDF render failed")

    log.info("wrote %s (%.0f KB)", output, output.stat().st_size / 1024)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE, help="HTML page to render")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUTPUT, help="PDF to write")
    parser.add_argument(
        "--dry-run", action="store_true", help="resolve and report assets without rendering"
    )
    args = parser.parse_args(argv)

    try:
        render(args.source, args.out, dry_run=args.dry_run)
    except (FileNotFoundError, RuntimeError) as exc:
        log.error("%s", exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
