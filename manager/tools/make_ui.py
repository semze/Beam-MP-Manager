#!/usr/bin/env python3
"""Builds ui/index.html from ui/index.src.html + assets/*.svg.

The UI is embedded in the exe, so the favicon and logo are inlined (no extra files to serve).
Run:  python tools/make_ui.py      (only needed if you edit index.src.html or the SVG icons)
"""
import re, sys, urllib.parse
from pathlib import Path

root = Path(__file__).resolve().parent.parent
src = (root / "ui" / "index.src.html").read_text(encoding="utf-8")
icon = (root / "assets" / "icon.svg").read_text(encoding="utf-8")
small = (root / "assets" / "icon-small.svg").read_text(encoding="utf-8")

# favicon: the simplified icon as a data URI
fav = "data:image/svg+xml," + urllib.parse.quote(re.sub(r">\s+<", "><", small).strip(), safe="/:=\" '")
fav = fav.replace('"', "%22").replace("'", "%27")

# logo + hero from the main icon: prefix ids so they cannot clash with anything else in the page
body = re.search(r"<svg[^>]*>(.*)</svg>", icon, re.S).group(1)
body = re.sub(r"\bid=\"(\w+)\"", r'id="lg-\1"', body)
body = re.sub(r"url\(#(\w+)\)", r"url(#lg-\1)", body)
defs = re.search(r"<defs>.*?</defs>", body, re.S).group(0)
shapes = body.replace(defs, "").strip()

# the animated hero: beams draw in, then the joints and wheels fade in
hero = shapes.replace('<g stroke="url(#lg-beam)"', '<g class="beam" stroke="url(#lg-beam)"', 1)
hero = re.sub(r'<g>\s*<circle cx="84"', '<g class="nodes"><circle cx="84"', hero, count=1)
hero = hero.replace('<g fill="#0b0f17" stroke="#f6f8fc" stroke-width="5">', '<g class="nodes" fill="#0b0f17" stroke="#f6f8fc" stroke-width="5">', 1)

src = src.replace("%%LOGO%%", shapes)
src = src.replace("%%HERO%%", hero)
src = src.replace("<defs>\n    <symbol", "<defs>" + defs[len("<defs>"):-len("</defs>")] + "\n    <symbol", 1)
src = src.replace("%%FAVICON%%", fav)
(root / "ui" / "index.html").write_text(src, encoding="utf-8")
print("ui/index.html written,", len(src), "bytes")
