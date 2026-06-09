#!/usr/bin/env python3
"""Migrate old HPL2 .map and .ent files to the new authored light-reach format.

The Vulkan renderer used to derive a point light's grid/cull reach from the
radiance floor (the commented-out block in HybridRenderer.cpp's point-light
upload); it now reads an authored CullingRadius (bin reach) and SourceRadius
(soft-shadow penumbra) per light. Old maps and entity definitions only carry
Radius, so this tool computes the two new attributes with the legacy
auto-derive formula and adds them to each <PointLight/> element:

    lin(c)        = c/12.92 if c <= 0.04045 else ((c+0.055)/1.055)^2.4
    maxC          = max(lin(DiffuseColor.rgb))
    reachSq       = maxC * Radius / kLightRadianceFloor - kPointLightSourceRadiusSq
    CullingRadius = sqrt(max(reachSq, 0))
    SourceRadius  = Radius * kPointLightDefaultSourceRadiusFrac

Spot lights still compute their reach in-engine (cone subset of the radius
sphere) and have an in-code SourceRadius fallback; box lights have no reach —
neither is touched.

Files are parsed and re-serialized with xml.etree.ElementTree (attribute order
and indentation are preserved; new attributes slot into alphabetical position,
matching the editor's serializer). The original is saved as <file>.bak before
the first in-place write.

Point the tool at any directory (searched recursively for *.map and *.ent) or
at individual files.
"""

import argparse
import math
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# Constants mirrored from amnesia/slang/Constants.h — keep in sync.
K_LIGHT_RADIANCE_FLOOR = 0.005            # kLightRadianceFloor
K_POINT_LIGHT_SOURCE_RADIUS_SQ = 0.25     # kPointLightSourceRadiusSq
K_DEFAULT_SOURCE_RADIUS_FRAC = 0.05       # kPointLightDefaultSourceRadiusFrac


def srgb_to_linear(c: float) -> float:
    """sRGB -> linear transfer (IEC 61966-2-1); mirrors detail::sRGBToLinear."""
    if c <= 0.04045:
        return c / 12.92
    return ((c + 0.055) / 1.055) ** 2.4


def fmt(v: float) -> str:
    """Trim trailing zeros the way the editor writes floats (9, 0.45, 23.9377)."""
    return f"{v:.6g}"


def compute_new_attrs(radius: float, diffuse: str):
    """Return (culling_radius, source_radius) from the legacy derive formula."""
    channels = [float(x) for x in diffuse.split()[:3]]
    max_c = max(srgb_to_linear(c) for c in channels) if channels else 0.0
    reach_sq = (max_c * radius) / K_LIGHT_RADIANCE_FLOOR - K_POINT_LIGHT_SOURCE_RADIUS_SQ
    culling = math.sqrt(reach_sq) if reach_sq > 0.0 else 0.0
    source = radius * K_DEFAULT_SOURCE_RADIUS_FRAC
    return culling, source


def set_attr_sorted(elem: ET.Element, name: str, value: str) -> None:
    """Set name=value, inserting at its alphabetical position among the
    existing attributes (the editor serializes alphabetically; ElementTree
    preserves dict order, so a plain .set() would append at the end)."""
    if name in elem.attrib:
        elem.set(name, value)  # keep existing position
        return
    rebuilt, placed = {}, False
    for k, v in elem.attrib.items():
        if not placed and k > name:
            rebuilt[name] = value
            placed = True
        rebuilt[k] = v
    if not placed:
        rebuilt[name] = value
    elem.attrib.clear()
    elem.attrib.update(rebuilt)


def patch_light(elem: ET.Element, force: bool, dry_run: bool, stats: dict,
                map_name: str) -> bool:
    """Compute and set the new attributes on one PointLight. Returns True if
    the element was (or would be) modified."""
    light_name = elem.get("Name", "?")

    if "CullingRadius" in elem.attrib and not force:
        stats["skipped"] += 1
        return False
    radius_str = elem.get("Radius")
    diffuse = elem.get("DiffuseColor")
    if radius_str is None or diffuse is None:
        print(f"  [WARN] {map_name}: {light_name}: missing Radius/DiffuseColor, skipped")
        stats["malformed"] += 1
        return False

    try:
        radius = float(radius_str)
        culling, source = compute_new_attrs(radius, diffuse)
    except ValueError:
        print(f"  [WARN] {map_name}: {light_name}: unparsable Radius/DiffuseColor, skipped")
        stats["malformed"] += 1
        return False

    if dry_run:
        max_c = max(srgb_to_linear(float(x)) for x in diffuse.split()[:3])
        print(f"  {light_name}: Radius={fmt(radius)} maxC={fmt(max_c)}"
              f" -> CullingRadius={fmt(culling)} SourceRadius={fmt(source)}")
    else:
        set_attr_sorted(elem, "CullingRadius", fmt(culling))
        set_attr_sorted(elem, "SourceRadius", fmt(source))
    stats["patched"] += 1
    return True


def migrate_file(path: Path, force: bool, dry_run: bool) -> dict:
    stats = {"patched": 0, "skipped": 0, "malformed": 0}
    original = path.read_text(encoding="utf-8")
    try:
        tree = ET.parse(path)
    except ET.ParseError as e:
        print(f"  [WARN] {path}: not valid XML ({e}), skipped")
        stats["malformed"] += 1
        return stats

    if dry_run:
        print(f"{path}:")
    modified = False
    for elem in tree.getroot().iter("PointLight"):
        modified |= patch_light(elem, force, dry_run, stats, path.name)

    if not dry_run and modified:
        bak = path.with_suffix(path.suffix + ".bak")
        if not bak.exists():  # never clobber the true original
            bak.write_text(original, encoding="utf-8")

        text = ET.tostring(tree.getroot(), encoding="unicode")
        if original.endswith("\n") and not text.endswith("\n"):
            text += "\n"
        path.write_text(text, encoding="utf-8")

        cache = path.with_suffix(path.suffix + "_cache")
        if cache.exists():
            print(f"  [NOTE] stale binary cache may shadow this edit: {cache}")

    return stats


GLOB_PATTERNS = ("*.map", "*.ent")


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("paths", nargs="*", type=Path,
                        default=[repo_root / "build/amnesia"],
                        help=".map/.ent files or directories (searched"
                             " recursively). Default: build/amnesia")
    parser.add_argument("--dry-run", action="store_true",
                        help="print computed values, write nothing")
    parser.add_argument("--force", action="store_true",
                        help="recompute lights that already have CullingRadius")
    args = parser.parse_args()

    files = []
    for p in args.paths:
        if p.is_dir():
            for pattern in GLOB_PATTERNS:
                files.extend(p.rglob(pattern))
        elif p.is_file():
            files.append(p)
        else:
            print(f"[ERROR] no such file or directory: {p}", file=sys.stderr)
            return 1
    files = sorted(set(files))
    if not files:
        print("[ERROR] no .map/.ent files found", file=sys.stderr)
        return 1

    totals = {"patched": 0, "skipped": 0, "malformed": 0}
    for path in files:
        stats = migrate_file(path, args.force, args.dry_run)
        for k in totals:
            totals[k] += stats[k]
        if stats["patched"] or stats["skipped"] or stats["malformed"]:
            print(f"{path}: {stats['patched']} patched,"
                  f" {stats['skipped']} skipped (already migrated),"
                  f" {stats['malformed']} malformed")

    print(f"\n{len(files)} files: {totals['patched']} lights patched,"
          f" {totals['skipped']} skipped, {totals['malformed']} malformed"
          f"{' (dry run — nothing written)' if args.dry_run else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
