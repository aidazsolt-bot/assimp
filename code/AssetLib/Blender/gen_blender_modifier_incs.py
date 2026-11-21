#!/usr/bin/env python3
"""Regenerate BlenderModifierEnum.inc + BlenderModifierDnaNames.inc from blender DNA."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def find_default_dna() -> Path:
    here = Path(__file__).resolve()
    assimp_root = here.parents[3]
    candidates = [
        assimp_root.parent / "blender.git/source/blender/makesdna/DNA_modifier_types.h",
        assimp_root / "blender.git/source/blender/makesdna/DNA_modifier_types.h",
        Path("/usr/src/TestAdnroidNDKSDK/blender.git/source/blender/makesdna/DNA_modifier_types.h"),
    ]
    for c in candidates:
        if c.is_file():
            return c
    return candidates[0]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dna", type=Path, default=None, help="Path to DNA_modifier_types.h")
    ap.add_argument("--out-dir", type=Path, default=Path(__file__).resolve().parent)
    args = ap.parse_args()
    dna_path = args.dna if args.dna is not None else find_default_dna()
    if not dna_path.is_file():
        print(f"DNA file not found: {dna_path}", file=sys.stderr)
        return 1
    dna = dna_path.read_text(encoding="utf-8", errors="replace")
    enums = re.findall(r"eModifierType_([A-Za-z0-9_]+)\s*=\s*(\d+)", dna)
    if not enums:
        print(f"no eModifierType_* found in {dna_path}", file=sys.stderr)
        return 1
    skip = {
        "ModifierData",
        "MappingInfoModifierData",
        "SurfaceModifierData_Runtime",
        "NodesModifierDataBlock",
    }
    structs = re.findall(r"^struct ([A-Za-z0-9_]+ModifierData)\b", dna, flags=re.M)
    seen: set[str] = set()
    structs = [s for s in structs if s not in skip and not (s in seen or seen.add(s))]
    special = {"SubsurfModifierData", "MirrorModifierData", "FluidModifierData"}
    generic = [s for s in structs if s not in special]

    enum_lines = [
        "/* Generated from blender.git DNA_modifier_types.h - do not hand-edit. */",
        "/* Regen: python3 gen_blender_modifier_incs.py [--dna PATH] */",
    ]
    for i, (n, v) in enumerate(enums):
        comma = "," if i + 1 < len(enums) else ""
        enum_lines.append(f"    eModifierType_{n} = {v}{comma}")
    (args.out_dir / "BlenderModifierEnum.inc").write_text("\n".join(enum_lines) + "\n", encoding="utf-8")

    name_lines = [
        "/* Generated from blender.git DNA_modifier_types.h - GenericModifierData DNA names. */",
        "/* Regen: python3 gen_blender_modifier_incs.py [--dna PATH] */",
        "/* SubsurfModifierData / MirrorModifierData / FluidModifierData keep specialized converters. */",
    ]
    for i, s in enumerate(generic):
        comma = "," if i + 1 < len(generic) else ""
        name_lines.append(f'    "{s}"{comma}')
    (args.out_dir / "BlenderModifierDnaNames.inc").write_text("\n".join(name_lines) + "\n", encoding="utf-8")
    print(f"wrote {len(enums)} enum values, {len(generic)} generic DNA names from {dna_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
