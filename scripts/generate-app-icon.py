#!/usr/bin/env python3
"""Render the Todoist application icon on the BlackBerry 10 Passport squircle footprint.

Unofficial Community Port Edition:
- Visual Design: Authentic Todoist crimson gradient with 3 crisp white checkmark stripes
  entering flush from the left edge and an authentic white BlackBerry 7-dot fruit emblem
  in the bottom-right corner (< 1/4 badge area).
- Canvas: 144x144 RGBA PNG.
- Base Footprint: 116x116 squircle with 14px transparent inset and 28px corner radius.
  Bounding box of non-transparent pixels is exactly (13, 13, 131, 131).
- Rendering: 4x supersampling (576x576) downsampled with Lanczos filtering for pristine
  453 PPI display fidelity on the BlackBerry Passport.
- Source: Composited directly from high-resolution reference design (ref.png) when present,
  with pure vector fallback.
"""

from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

# Authentic BlackBerry 7-dot fruit emblem path (in 24x24 normalized space)
BB_EMBLEM_PATH = (
    "M2.05 3.54L1.17 7.7H4.45C6.1 7.7 7.42 6.38 7.42 4.73V3.54H2.05"
    "M2.13 11.23l-.88 4.16h3.42c1.65 0 2.97-1.32 2.97-2.97v-1.19H2.13"
    "M12.95 11.23l-.88 4.16h3.42c1.65 0 2.97-1.32 2.97-2.97v-1.19h-5.51"
    "M13.04 3.54l-.88 4.16h3.42c1.65 0 2.97-1.32 2.97-2.97V3.54h-5.51"
    "M11.58 17.65l-.88 4.16h3.42c1.65 0 2.97-1.32 2.97-2.97v-1.19h-5.51"
    "M19.39 14.19l-.88 4.16h3.42c.8 0 1.5-.32 2.02-.85a2.85 2.85 0 0 0 .85-2.02v-1.29h-5.41"
    "M20.24 7.23h-5.41v1.19c0 1.65 1.32 2.97 2.97 2.97h3.42l-.88-4.16z"
)

# Vector fallback: 464x464 red squircle with 3 check stripes and corner BlackBerry emblem
VECTOR_FALLBACK_SVG = f"""<svg xmlns="http://www.w3.org/2000/svg" width="576" height="576" viewBox="0 0 576 576">
<defs>
  <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0%" stop-color="#E84435"/>
    <stop offset="100%" stop-color="#CA2A1E"/>
  </linearGradient>
</defs>
<rect x="56" y="56" width="464" height="464" fill="url(#bg)"/>
<g transform="translate(56, 56) scale(19.333)">
  <!-- Stripe 1 (Top Check, starts flush at x=0) -->
  <path fill="#FFFFFF" d="
    M 0 5.2
    L 5.3 8.3 C 5.8 8.6 6.4 8.6 6.9 8.3
    L 15.2 3.5 C 15.8 3.2 16.5 3.4 16.8 4.0 C 17.1 4.6 16.9 5.3 16.3 5.6
    L 7.2 11.0 C 6.5 11.4 5.7 11.3 5.1 11.0
    L 0 8.0 Z
  "/>
  <!-- Stripe 2 (Middle Check, starts flush at x=0) -->
  <path fill="#FFFFFF" d="
    M 0 10.4
    L 5.3 13.5 C 5.8 13.8 6.4 13.8 6.9 13.5
    L 15.2 8.7 C 15.8 8.4 16.5 8.6 16.8 9.2 C 17.1 9.8 16.9 10.5 16.3 10.8
    L 7.2 16.2 C 6.5 16.6 5.7 16.5 5.1 16.2
    L 0 13.2 Z
  "/>
  <!-- Stripe 3 (Bottom Check, starts flush at x=0) -->
  <path fill="#FFFFFF" d="
    M 0 15.6
    L 5.3 18.7 C 5.8 19.0 6.4 19.0 6.9 18.7
    L 15.2 13.9 C 15.8 13.6 16.5 13.8 16.8 14.4 C 17.1 15.0 16.9 15.7 16.3 16.0
    L 7.2 21.4 C 6.5 21.8 5.7 21.7 5.1 21.4
    L 0 18.4 Z
  "/>
  <!-- BlackBerry 7-Dot Emblem in Bottom-Right (< 1/4 area) -->
  <g transform="translate(15.2, 14.8) scale(0.32)">
    <path fill="#FFFFFF" d="{BB_EMBLEM_PATH}"/>
  </g>
</g>
</svg>"""


def generate_app_icon(output_path=None, ref_path=None):
    if output_path is None:
        output_path = ROOT / "assets/icon.png"
    else:
        output_path = Path(output_path)

    if ref_path is None:
        if (ROOT / "ref.png").is_file():
            ref_path = ROOT / "ref.png"
        elif (ROOT / "assets/ref.png").is_file():
            ref_path = ROOT / "assets/ref.png"
    else:
        ref_path = Path(ref_path)

    with tempfile.TemporaryDirectory() as temp_dir:
        dir_path = Path(temp_dir)
        mask = dir_path / "mask.png"
        raw_source = dir_path / "source.png"

        # 1. Standard Passport squircle mask:
        # 144x144 canvas, 116x116 base footprint, 14px transparent inset, 28px corner radius.
        # At 4x supersampling (576x576):
        # Base rectangle from 56,56 to 519,519 (size 464x464) with corner radius 112.
        subprocess.run(
            [
                "convert",
                "-size",
                "576x576",
                "xc:none",
                "-fill",
                "white",
                "-draw",
                "roundrectangle 56,56 519,519 112,112",
                str(mask),
            ],
            check=True,
        )

        # 2. Source image: use ref_path if present, otherwise render vector fallback SVG
        if ref_path is not None and ref_path.is_file():
            # Resize ref.png to 464x464 base footprint centered in 576x576 canvas
            subprocess.run(
                [
                    "convert",
                    str(ref_path),
                    "-resize",
                    "464x464!",
                    "-background",
                    "none",
                    "-gravity",
                    "center",
                    "-extent",
                    "576x576",
                    str(raw_source),
                ],
                check=True,
            )
        else:
            subprocess.run(
                [
                    "convert",
                    "-background",
                    "none",
                    "svg:-",
                    str(raw_source),
                ],
                input=VECTOR_FALLBACK_SVG.encode("utf-8"),
                check=True,
            )

        # 3. Composite source through the squircle mask, downsample to 144x144, save as 32-bit RGBA PNG
        subprocess.run(
            [
                "convert",
                str(raw_source),
                str(mask),
                "-compose",
                "DstIn",
                "-composite",
                "-resize",
                "144x144",
                "-strip",
                f"PNG32:{output_path}",
            ],
            check=True,
        )

    print(f"OK: Generated standard Passport squircle app icon at {output_path}")


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else None
    generate_app_icon(output_path=out)
