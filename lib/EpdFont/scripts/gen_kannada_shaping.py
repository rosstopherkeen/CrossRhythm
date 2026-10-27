#!/usr/bin/env python3
"""Generate the Kannada shaping tables used by lib/KannadaShape.

Kannada writes consonant clusters as a base letter with the following
consonants stacked beneath it, and merges most vowel signs into the base
glyph.  Those combined shapes are produced by the font's GSUB table and have
no Unicode codepoints, so a renderer that looks glyphs up by codepoint cannot
reach them -- Kannada comes out as separate letters with visible viramas.

Running HarfBuzz on the device is not an option (no heap or flash budget for
it), and one table entry per cluster is ~20k entries.  So this script shapes
the cluster space offline with HarfBuzz and factors the result into three small
tables the device can apply directly:

    base[(unit, matra, hasSubscript)] -> up to 2 codepoints + where subscripts
                                         are inserted within them
    sub[(consonant, matra)]           -> that consonant's below-base codepoint
    akhand[(c1, c2)]                  -> pairs that behave as one base unit

Whatever those tables still get wrong is emitted as a literal exception keyed on
the whole cluster, so coverage is exact by construction rather than by argument.
The script then re-shapes every cluster and refuses to emit anything unless the
tables reproduce HarfBuzz byte for byte -- swapping the font makes the build
fail loudly instead of silently rendering wrong Kannada.

Shaped glyphs without a cmap entry are assigned private-use codepoints from
U+E000 and written to a sidecar JSON, which fontconvert_sdcard.py rasterizes
into the .cpfont alongside the normal intervals.

Usage:
    python3 gen_kannada_shaping.py NotoSansKannada-Regular.ttf \\
        --header ../../KannadaShape/generated/KannadaTables.h \\
        --pua-map kannada_pua_map.json

Requires: fonttools, uharfbuzz (see requirements.txt).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter, defaultdict

try:
    from fontTools.ttLib import TTFont
    import uharfbuzz as hb
except ImportError as exc:  # pragma: no cover - build tooling guard
    print(f"error: {exc}. Install with: pip install -r requirements.txt", file=sys.stderr)
    sys.exit(1)

# --- Kannada block structure -------------------------------------------------

# Consonants ka..ha, skipping the two unassigned slots in the run.
CONSONANTS = [cp for cp in range(0x0C95, 0x0CBA) if cp not in (0x0CA9, 0x0CB4)]
# Dependent vowel signs, plus the two length marks that combine with them.
MATRAS = [cp for cp in range(0x0CBE, 0x0CCD) if cp != 0x0CC5] + [0x0CD5, 0x0CD6]
VIRAMA = 0x0CCD
RA = 0x0CB0
NO_MATRA = 0

# Private-use base for shaped forms the font leaves unencoded.
PUA_BASE = 0xE000
PUA_LIMIT = 0xF8FF

CONSONANT_INDEX = {cp: i for i, cp in enumerate(CONSONANTS)}
MATRA_INDEX = {NO_MATRA: 0}
for _i, _m in enumerate(MATRAS):
    MATRA_INDEX[_m] = _i + 1
MATRA_SLOTS = len(MATRAS) + 1


def shape(hbfont, codepoints):
    buf = hb.Buffer()
    buf.add_str("".join(chr(c) for c in codepoints))
    buf.guess_segment_properties()
    hb.shape(hbfont, buf)
    return [info.codepoint for info in buf.glyph_infos]


def cluster_codepoints(consonants, matra):
    """Logical codepoint sequence for a cluster spec.

    consonants is a tuple of codepoints, optionally led by the marker "R" for a
    repha (ra + virama) prefix, which HarfBuzz reorders to the end of the
    cluster as arkavattu.
    """
    out = []
    rest = consonants
    if consonants and consonants[0] == "R":
        out += [RA, VIRAMA]
        rest = consonants[1:]
    for i, consonant in enumerate(rest):
        out.append(consonant)
        if i < len(rest) - 1:
            out.append(VIRAMA)
    if matra:
        out.append(matra)
    return out


def enumerate_specs():
    """The cluster space the tables are derived from and verified against."""
    specs = [((c,), NO_MATRA) for c in CONSONANTS]
    specs += [((c,), m) for c in CONSONANTS for m in MATRAS]
    specs += [((a, b), NO_MATRA) for a in CONSONANTS for b in CONSONANTS]
    specs += [((a, b), m) for a in CONSONANTS for b in CONSONANTS for m in MATRAS]
    specs += [(("R", c), NO_MATRA) for c in CONSONANTS]
    specs += [(("R", c), m) for c in CONSONANTS for m in MATRAS]
    return specs


def derive(font_path):
    tt = TTFont(font_path, lazy=True)
    glyph_order = tt.getGlyphOrder()
    cmap = tt.getBestCmap()
    name_to_gid = {name: gid for gid, name in enumerate(glyph_order)}
    gid_to_codepoint = {}
    for cp, name in cmap.items():
        gid = name_to_gid.get(name)
        # Lowest codepoint wins so the mapping is deterministic across runs.
        if gid is not None and (gid not in gid_to_codepoint or cp < gid_to_codepoint[gid]):
            gid_to_codepoint[gid] = cp

    hbfont = hb.Font(hb.Face(hb.Blob.from_file_path(font_path)))

    def rejected(gids):
        for gid in gids:
            name = glyph_order[gid] if gid < len(glyph_order) else ""
            if name == ".notdef" or "dotted" in name.lower():
                return True
        return False

    truth = {}
    for spec in enumerate_specs():
        gids = shape(hbfont, cluster_codepoints(*spec))
        if gids and not rejected(gids):
            truth[spec] = gids

    if not truth:
        print("error: font produced no usable Kannada clusters", file=sys.stderr)
        sys.exit(1)

    # reph: the glyph a repha cluster adds over the same cluster without it.
    reph_votes = Counter()
    for (consonants, matra), gids in truth.items():
        if consonants[0] != "R":
            continue
        inner = truth.get(((consonants[1],), matra))
        if inner:
            reph_votes.update(g for g in gids if g not in inner)
    reph = reph_votes.most_common(1)[0][0] if reph_votes else None

    # Plain below-base form per consonant, from matra-free conjuncts.
    sub_votes = defaultdict(Counter)
    for first in CONSONANTS:
        solo = truth.get(((first,), NO_MATRA), [])
        for second in CONSONANTS:
            gids = truth.get(((first, second), NO_MATRA))
            if gids:
                sub_votes[second].update(g for g in gids if g not in solo)
    subscript = {c: v.most_common(1)[0][0] for c, v in sub_votes.items() if v}

    # A conjunct whose output contains no below-base form is an akhand ligature
    # (ksha, jnya): the pair renders as its own base unit.
    akhand = set()
    for first in CONSONANTS:
        for second in CONSONANTS:
            gids = truth.get(((first, second), NO_MATRA))
            if not gids:
                continue
            form = subscript.get(second)
            if form is None or form not in gids:
                akhand.add((first, second))

    def split_unit(consonants):
        """Separate the base unit from the consonants stacked beneath it."""
        if len(consonants) >= 2 and (consonants[0], consonants[1]) in akhand:
            return (consonants[0], consonants[1]), consonants[2:]
        return consonants[0], consonants[1:]

    base = {}
    for (consonants, matra), gids in truth.items():
        if consonants[0] == "R":
            continue
        unit, tail = split_unit(consonants)
        if tail:
            continue
        base[(unit, matra, False)] = (tuple(gids), len(gids))

    # Below-base form keyed by (consonant, matra): the form has plain and
    # right-side variants, and for some matras it absorbs the matra entirely.
    # Which one appears is decided by the matra, so keying on the pair captures
    # both without modelling either rule.
    sub_pair_votes = defaultdict(Counter)
    for (consonants, matra), gids in truth.items():
        if consonants[0] == "R":
            continue
        unit, tail = split_unit(consonants)
        if len(tail) != 1:
            continue
        solo = base.get((unit, matra, False))
        if solo:
            sub_pair_votes[(tail[0], matra)].update(g for g in gids if g not in solo[0])
    sub_pair = {k: v.most_common(1)[0][0] for k, v in sub_pair_votes.items() if v}

    # The base usually changes form when something attaches beneath it, so a
    # cluster carrying a subscript needs its own entry. Removing the subscript
    # glyph leaves the base rendering, and the slot it vacated is the insertion
    # point: for matras that split into a merged part and a trailing part, the
    # subscript belongs between them rather than after.
    for (consonants, matra), gids in truth.items():
        if consonants[0] == "R":
            continue
        unit, tail = split_unit(consonants)
        if len(tail) != 1:
            continue
        form = sub_pair.get((tail[0], matra))
        if form is None or form not in gids:
            continue
        index = gids.index(form)
        base[(unit, matra, True)] = (tuple(g for g in gids if g != form), index)

    def model(consonants, matra):
        if consonants[0] == "R":
            inner = model(consonants[1:], matra)
            if inner is None or reph is None:
                return None
            return inner + [reph]
        unit, tail = split_unit(consonants)
        entry = base.get((unit, matra, bool(tail))) or base.get((unit, matra, False))
        if entry is None:
            return None
        sequence, insert_at = entry
        forms = []
        for consonant in tail:
            form = sub_pair.get((consonant, matra), subscript.get(consonant))
            if form is None:
                return None
            forms.append(form)
        return list(sequence[:insert_at]) + forms + list(sequence[insert_at:])

    exceptions = {spec: gids for spec, gids in truth.items() if model(*spec) != gids}

    # --- assign codepoints to every glyph the tables can emit ---
    reached = set()
    for gids in truth.values():
        reached.update(gids)
    pua = {}
    next_pua = PUA_BASE
    for gid in sorted(reached):
        if gid in gid_to_codepoint:
            continue
        if next_pua > PUA_LIMIT:
            print("error: shaped forms exceed the private-use range", file=sys.stderr)
            sys.exit(1)
        pua[gid] = next_pua
        next_pua += 1

    def codepoint_of(gid):
        return gid_to_codepoint.get(gid, pua.get(gid))

    tt.close()
    return {
        "truth": truth,
        "base": base,
        "subscript": subscript,
        "sub_pair": sub_pair,
        "akhand": akhand,
        "reph": reph,
        "exceptions": exceptions,
        "pua": pua,
        "codepoint_of": codepoint_of,
        "glyph_order": glyph_order,
        "model": model,
        "split_unit": split_unit,
    }


def verify(derived):
    """Re-shape every cluster and confirm tables + exceptions are exact."""
    truth = derived["truth"]
    model = derived["model"]
    exceptions = derived["exceptions"]
    codepoint_of = derived["codepoint_of"]

    failures = []
    for spec, gids in truth.items():
        produced = exceptions.get(spec) or model(*spec)
        if produced != gids:
            failures.append(spec)
            continue
        if any(codepoint_of(g) is None for g in produced):
            failures.append(spec)
    return failures


def unit_id(unit, akhand_order):
    if isinstance(unit, tuple):
        return len(CONSONANTS) + akhand_order[unit]
    return CONSONANT_INDEX[unit]


def emit_header(derived, path, font_path):
    base = derived["base"]
    sub_pair = derived["sub_pair"]
    subscript = derived["subscript"]
    akhand = derived["akhand"]
    reph = derived["reph"]
    exceptions = derived["exceptions"]
    codepoint_of = derived["codepoint_of"]

    akhand_order = {pair: i for i, pair in enumerate(sorted(akhand))}

    # Every row below must survive into the header unchanged: the verify pass
    # proved the tables reproduce HarfBuzz, so silently dropping a row here
    # would ship something that was never checked. Anything that does not fit
    # the on-device entry shape is a hard error, not a skip.
    # A base unit can render as anywhere from one to four glyphs (a matra such
    # as 0CCB splits into a merged vowel, a post-base sign and a length mark),
    # so entries point into a shared pool rather than carrying a fixed array.
    base_pool = []
    base_rows = []
    for (unit, matra, has_sub), (sequence, insert_at) in base.items():
        codepoints = [codepoint_of(g) for g in sequence]
        if any(c is None for c in codepoints):
            raise ValueError(f"base unit {unit!r} matra {matra:#06x} reaches an unmapped glyph")
        if len(codepoints) > 0xFF or insert_at > 0xFF:
            raise ValueError(f"base unit {unit!r} matra {matra:#06x} exceeds the uint8 count fields")
        key = unit_id(unit, akhand_order) * (MATRA_SLOTS * 2) + MATRA_INDEX[matra] * 2 + int(has_sub)
        base_rows.append((key, len(base_pool), len(codepoints), min(insert_at, len(codepoints))))
        base_pool.extend(codepoints)
    base_rows.sort()

    sub_rows = sorted((c_index * MATRA_SLOTS + MATRA_INDEX[matra], codepoint_of(form))
                      for (consonant, matra), form in sub_pair.items()
                      if (c_index := CONSONANT_INDEX.get(consonant)) is not None)

    plain_rows = sorted((CONSONANT_INDEX[c], codepoint_of(form))
                        for c, form in subscript.items() if c in CONSONANT_INDEX)

    akhand_rows = sorted((CONSONANT_INDEX[a] * len(CONSONANTS) + CONSONANT_INDEX[b],
                          len(CONSONANTS) + akhand_order[(a, b)])
                         for (a, b) in akhand)

    # Exceptions: key on the whole cluster, values in a shared codepoint pool.
    pool = []
    exc_rows = []
    for (consonants, matra), gids in sorted(
            exceptions.items(), key=lambda kv: (len(kv[0][0]), str(kv[0][0]), kv[0][1])):
        has_reph = consonants[0] == "R"
        rest = consonants[1:] if has_reph else consonants
        if len(rest) > 2 or any(c not in CONSONANT_INDEX for c in rest):
            raise ValueError(
                f"exception cluster {consonants!r} has more consonants than the "
                "ExceptionEntry key encodes")
        codepoints = [codepoint_of(g) for g in gids]
        if any(c is None for c in codepoints):
            raise ValueError(f"exception cluster {consonants!r} reaches an unmapped glyph")
        if len(codepoints) > 0xFF:
            raise ValueError(f"exception cluster {consonants!r} exceeds the uint8 count field")
        first = CONSONANT_INDEX[rest[0]]
        second = CONSONANT_INDEX[rest[1]] if len(rest) > 1 else 0xFF
        key = (int(has_reph) << 24) | (first << 16) | (second << 8) | MATRA_INDEX[matra]
        exc_rows.append((key, len(pool), len(codepoints)))
        pool.extend(codepoints)
    exc_rows.sort()

    def hex_list(values, per_line=12, formatter="0x{:04X}"):
        lines = []
        for i in range(0, len(values), per_line):
            lines.append("    " + ", ".join(formatter.format(v) for v in values[i:i + per_line]) + ",")
        return "\n".join(lines) if lines else "    0,"

    out = []
    out.append("// GENERATED FILE -- DO NOT EDIT.")
    out.append("//")
    out.append(f"// Source font: {os.path.basename(font_path)}")
    out.append("// Regenerate with lib/EpdFont/scripts/gen_kannada_shaping.py, which verifies")
    out.append("// these tables reproduce HarfBuzz for every cluster before writing them.")
    out.append("")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace kannadaTables {")
    out.append("")
    out.append(f"constexpr uint32_t PUA_BASE = 0x{PUA_BASE:04X};")
    out.append(f"constexpr uint8_t CONSONANT_COUNT = {len(CONSONANTS)};")
    out.append(f"constexpr uint8_t MATRA_SLOTS = {MATRA_SLOTS};")
    out.append(f"constexpr uint32_t REPH_CODEPOINT = 0x{(codepoint_of(reph) or 0):04X};")
    out.append("")
    out.append("/// Consonants ka..ha in block order; index is the id used by the key tables.")
    out.append(f"inline constexpr uint16_t CONSONANTS[{len(CONSONANTS)}] = {{")
    out.append(hex_list(CONSONANTS))
    out.append("};")
    out.append("")
    out.append("/// Dependent vowel signs; matra slot 0 means \"no matra\".")
    out.append(f"inline constexpr uint16_t MATRAS[{len(MATRAS)}] = {{")
    out.append(hex_list(MATRAS))
    out.append("};")
    out.append("")
    out.append("/// Base unit + matra rendering. `insertAt` is where below-base forms go.")
    out.append("struct BaseEntry {")
    out.append("  uint16_t key;     ///< unitId * (MATRA_SLOTS*2) + matraSlot*2 + hasSubscript")
    out.append("  uint16_t offset;  ///< into BASE_CODEPOINTS")
    out.append("  uint8_t count;")
    out.append("  uint8_t insertAt;  ///< index within this run where subscripts go")
    out.append("};")
    out.append(f"inline constexpr BaseEntry BASE[{len(base_rows)}] = {{")
    for key, offset, count, insert_at in base_rows:
        out.append(f"    {{0x{key:04X}, {offset}, {count}, {insert_at}}},")
    out.append("};")
    out.append(f"inline constexpr uint16_t BASE_CODEPOINTS[{max(len(base_pool), 1)}] = {{")
    out.append(hex_list(base_pool))
    out.append("};")
    out.append("")
    out.append("/// Below-base form for (consonant, matra); absorbs the matra for some pairs.")
    out.append("struct SubEntry {")
    out.append("  uint16_t key;  ///< consonantId * MATRA_SLOTS + matraSlot")
    out.append("  uint16_t codepoint;")
    out.append("};")
    out.append(f"inline constexpr SubEntry SUBSCRIPT[{len(sub_rows)}] = {{")
    for key, cp in sub_rows:
        out.append(f"    {{0x{key:04X}, 0x{cp:04X}}},")
    out.append("};")
    out.append("")
    out.append("/// Matra-independent below-base form, used when no pair entry exists.")
    out.append(f"inline constexpr SubEntry SUBSCRIPT_PLAIN[{len(plain_rows)}] = {{")
    for key, cp in plain_rows:
        out.append(f"    {{0x{key:04X}, 0x{cp:04X}}},")
    out.append("};")
    out.append("")
    out.append("/// Consonant pairs that render as a single base unit (ksha, jnya).")
    out.append("struct AkhandEntry {")
    out.append("  uint16_t key;     ///< firstId * CONSONANT_COUNT + secondId")
    out.append("  uint16_t unitId;")
    out.append("};")
    out.append(f"inline constexpr AkhandEntry AKHAND[{len(akhand_rows)}] = {{")
    for key, unit in akhand_rows:
        out.append(f"    {{0x{key:04X}, {unit}}},")
    out.append("};")
    out.append("")
    out.append("/// Clusters the tables above do not reproduce, stored literally.")
    out.append("struct ExceptionEntry {")
    out.append("  uint32_t key;  ///< reph<<24 | firstId<<16 | secondId<<8 | matraSlot (secondId 0xFF = none)")
    out.append("  uint16_t offset;  ///< into EXCEPTION_CODEPOINTS")
    out.append("  uint8_t count;")
    out.append("};")
    out.append(f"inline constexpr ExceptionEntry EXCEPTIONS[{len(exc_rows)}] = {{")
    for key, offset, count in exc_rows:
        out.append(f"    {{0x{key:08X}, {offset}, {count}}},")
    out.append("};")
    out.append(f"inline constexpr uint16_t EXCEPTION_CODEPOINTS[{max(len(pool), 1)}] = {{")
    out.append(hex_list(pool))
    out.append("};")
    out.append("")
    out.append("}  // namespace kannadaTables")
    out.append("")

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(out))

    return {
        "base": len(base_rows),
        "subscript": len(sub_rows),
        "plain": len(plain_rows),
        "akhand": len(akhand_rows),
        "exceptions": len(exc_rows),
        "pool": len(pool),
        "base_pool": len(base_pool),
        "bytes": len(base_rows) * 6 + len(base_pool) * 2 + (len(sub_rows) + len(plain_rows)) * 4
                 + len(akhand_rows) * 4 + len(exc_rows) * 8 + len(pool) * 2,
    }


def emit_pua_map(derived, path, font_path):
    glyph_order = derived["glyph_order"]
    forms = [{"codepoint": cp, "glyph": glyph_order[gid]}
             for gid, cp in sorted(derived["pua"].items(), key=lambda kv: kv[1])]
    payload = {
        "comment": ("Shaped Kannada forms with no cmap entry, mapped to private-use "
                    "codepoints. fontconvert_sdcard.py rasterizes these by glyph name."),
        "source_font": os.path.basename(font_path),
        "pua_base": PUA_BASE,
        "forms": forms,
    }
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, ensure_ascii=True)
        handle.write("\n")
    return len(forms)


def emit_golden(derived, path, font_path, stride):
    """Write HarfBuzz ground truth for the native shaper test.

    Every cluster the tables failed to reproduce is included, since those are
    exactly the cases a regression would hit first; the rest are sampled to keep
    the generated file reviewable.
    """
    truth = derived["truth"]
    exceptions = derived["exceptions"]
    codepoint_of = derived["codepoint_of"]

    selected = []
    for index, (spec, gids) in enumerate(sorted(truth.items(), key=lambda kv: str(kv[0]))):
        if spec not in exceptions and index % stride != 0:
            continue
        expected = [codepoint_of(g) for g in gids]
        if any(c is None for c in expected):
            continue
        selected.append((cluster_codepoints(*spec), expected))

    records = []
    for logical, expected in selected:
        records.append(len(logical))
        records.extend(logical)
        records.append(len(expected))
        records.extend(expected)

    lines = []
    lines.append("// GENERATED FILE -- DO NOT EDIT.")
    lines.append("//")
    lines.append(f"// HarfBuzz ground truth for {os.path.basename(font_path)}, used by")
    lines.append("// KannadaShapeTest to prove the on-device shaper agrees with the shaper the")
    lines.append("// tables were derived from. Regenerate with gen_kannada_shaping.py --golden.")
    lines.append("//")
    lines.append("// Flat records: [logicalCount, logical..., expectedCount, expected...]")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("namespace kannadaGolden {")
    lines.append("")
    lines.append(f"constexpr int CASE_COUNT = {len(selected)};")
    lines.append(f"inline constexpr uint16_t RECORDS[{max(len(records), 1)}] = {{")
    for i in range(0, len(records), 12):
        lines.append("    " + ", ".join(f"0x{v:04X}" for v in records[i:i + 12]) + ",")
    if not records:
        lines.append("    0,")
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace kannadaGolden")
    lines.append("")

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))
    return len(selected)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("font", help="Kannada TTF/OTF to derive tables from")
    parser.add_argument("--header", required=True, help="generated C++ header path")
    parser.add_argument("--pua-map", required=True, help="generated private-use map JSON path")
    parser.add_argument("--golden", help="optional generated golden-set header for the native test")
    parser.add_argument("--golden-stride", type=int, default=6,
                        help="sample every Nth non-exception cluster into the golden set")
    args = parser.parse_args()

    derived = derive(args.font)
    failures = verify(derived)
    total = len(derived["truth"])
    exact = total - len(derived["exceptions"])

    print(f"clusters shaped:        {total}", file=sys.stderr)
    print(f"reproduced by tables:   {exact} ({100.0 * exact / total:.2f}%)", file=sys.stderr)
    print(f"literal exceptions:     {len(derived['exceptions'])}", file=sys.stderr)

    if failures:
        print(f"error: {len(failures)} clusters do not round-trip; refusing to emit tables.",
              file=sys.stderr)
        for spec in failures[:10]:
            print(f"  {spec}", file=sys.stderr)
        sys.exit(1)

    stats = emit_header(derived, args.header, args.font)
    forms = emit_pua_map(derived, args.pua_map, args.font)

    print(f"private-use forms:      {forms}", file=sys.stderr)
    print(f"table rows:             base={stats['base']} sub={stats['subscript']} "
          f"plain={stats['plain']} akhand={stats['akhand']} exc={stats['exceptions']}",
          file=sys.stderr)
    print(f"flash cost:             {stats['bytes']} bytes "
          f"({stats['bytes'] / 1024.0:.1f} KB)", file=sys.stderr)
    print(f"wrote {args.header}", file=sys.stderr)
    print(f"wrote {args.pua_map}", file=sys.stderr)

    if args.golden:
        cases = emit_golden(derived, args.golden, args.font, max(args.golden_stride, 1))
        print(f"golden cases:           {cases}", file=sys.stderr)
        print(f"wrote {args.golden}", file=sys.stderr)


if __name__ == "__main__":
    main()
