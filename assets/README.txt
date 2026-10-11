BeamMP Manager icon set
-----------------------
icon.svg          Main logo (256x256 viewBox): a soft-body lattice car. Use at 48px and up.
icon-small.svg    Bolder, simplified version for 16-32px (also used as the browser favicon).
favicon.svg       Same as icon-small.svg.
app.ico           Windows icon, 7 sizes (16, 24, 32, 48, 64, 128, 256). Embedded in the exe.
png/              icon-16 ... icon-512 as PNG (16/24/32 use the small version).

Colors: plate #242e49 -> #0b0f17, beams #f6f8fc -> #9eabc9, wheels #ffc933.
All artwork is original to this project.

To change the logo: edit the SVGs, export the PNGs at the same sizes into png/, run
  python tools/make_ico.py      (rebuilds app.ico)
  python tools/make_ui.py       (re-embeds the logo + favicon in the app's UI)
then rebuild the exe.
