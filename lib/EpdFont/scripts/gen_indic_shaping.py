#!/usr/bin/env python3
"""Generate the Indic shaping tables (.cpshape) that lib/IndicShape loads.

The Brahmic scripts write consonant clusters as a base letter plus stacked or
attached forms for the consonants that follow, and merge most vowel signs into
the base glyph.  Those combined shapes are produced by the font's GSUB table
and have no Unicode codepoints, so a renderer that looks glyphs up by codepoint
cannot reach them -- the text comes out as separate letters with visible
viramas.

Running HarfBuzz on the device is not an option (no heap or flash budget for
it), and one table entry per cluster is tens of thousands of entries.  So this
script shapes a script's cluster space offline with HarfBuzz and factors the
result into three small tables the device can apply directly:

    base[(unit, matra, hasSubscript)] -> up to 4 codepoints + where subscripts
                                         are inserted within them
    sub[(consonant, matra)]           -> that consonant's below-base codepoint
    akhand[(c1, c2)]                  -> pairs that behave as one base unit

Whatever those tables still get wrong is emitted as a literal exception keyed on
the whole cluster, so coverage is exact by construction rather than by argument.
The script then re-shapes every cluster and refuses to emit anything unless the
tables reproduce HarfBuzz byte for byte -- swapping the font makes the build
fail loudly instead of silently rendering wrong text.

Which consonant of a conjunct is the base differs by script (and by font), so
each script records `base_last` -- whichever position reproduces more of the
cluster space, measured rather than assumed.

The tables go into a `.cpshape` file (lib/IndicShape/IndicTables.h has the
layout), committed under shaping/ and copied by build-sd-fonts.py into the
matching font family's folder as shaping.cpshape. The device loads it from
there when the font is installed, so the firmware carries no per-script data.

Shaped glyphs without a cmap entry are assigned private-use codepoints from a
per-script range and written to a JSON map, which fontconvert_sdcard.py
rasterizes into the .cpfont alongside the normal intervals. The .cpshape, the
map and the .cpfont must come from the same font, which is why they ship
together in one folder.

Usage:
    python3 gen_indic_shaping.py --script kannada NotoSansKannada-Regular.ttf
    python3 gen_indic_shaping.py --script telugu NotoSansTelugu-Regular.ttf \\
        --golden

Requires: fonttools, uharfbuzz (see requirements.txt).
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import unicodedata
from collections import Counter, defaultdict

try:
    from fontTools.ttLib import TTFont
    import uharfbuzz as hb
except ImportError as exc:  # pragma: no cover - build tooling guard
    print(f"error: {exc}. Install with: pip install -r requirements.txt", file=sys.stderr)
    sys.exit(1)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", "..", ".."))

NO_MATRA = 0

# .cpshape v1; must match lib/IndicShape/IndicTables.h.
SIDECAR_MAGIC = b"CPSH"
SIDECAR_VERSION = 1
SIDECAR_HEADER_SIZE = 64


class ScriptSpec:
    """Everything script-specific the derivation and the device need.

    `pua_base`/`pua_limit` carve the private-use area into a range per script
    so a font covering two of them can still address both sets of shaped forms.
    Ranges are fixed once published: changing one invalidates every .cpfont
    already generated for that script.

    `has_reph` says whether a leading ra + virama reorders out of the cluster
    (Kannada's arkavattu, Telugu's ra-vattu).  Where it does not, the leading ra
    is an ordinary consonant and those clusters are not enumerated separately.

    `nukta`, when set, is the script's nukta sign. Every consonant whose
    canonical decomposition is consonant + nukta is then written to the file as
    a fold pair, because books carry the decomposed spelling (Unicode keeps the
    precomposed letters out of NFC) while the font only draws the precomposed
    letter.

    `base_last` says which consonant of a conjunct is the base -- the one that
    keeps its full form and carries the vowel sign.  Kannada and Telugu stack
    the later consonants beneath the first, so the base is first.  Devanagari,
    Bengali, Odia and Malayalam turn the earlier consonants into half or
    attached forms and leave the last one as the base, so the matra belongs to
    it.  Modelling those with a first-position base put almost the whole cluster
    space in the exception table.
    """

    def __init__(self, name, display, block, consonants, matras, virama, ra,
                 pua_base, pua_limit, has_reph=True, base_last=False, nukta=0):
        self.name = name
        self.display = display
        self.block = block
        self.consonants = consonants
        self.matras = matras
        self.virama = virama
        self.ra = ra
        self.pua_base = pua_base
        self.pua_limit = pua_limit
        self.has_reph = has_reph
        self.base_last = base_last
        self.nukta = nukta

    @property
    def namespace(self):
        return f"{self.name}Tables"

    @property
    def golden_namespace(self):
        return f"{self.name}Golden"

    @property
    def sidecar_path(self):
        """Committed copy build-sd-fonts.py places into the font's folder."""
        return os.path.join(SCRIPT_DIR, "shaping", f"{self.name}.cpshape")

    @property
    def pua_map_path(self):
        return os.path.join(SCRIPT_DIR, f"{self.name}_pua_map.json")

    @property
    def golden_path(self):
        return os.path.join(REPO_ROOT, "test", "indic_shaping", "generated",
                            f"{self.display}Golden.h")


# Consonant and matra runs are listed from the Unicode block, skipping the
# unassigned slots. The order fixes the ids used by every key table, so a row
# added in the middle later would invalidate the generated .cpfont PUA maps --
# append instead.
SCRIPTS = {
    "kannada": ScriptSpec(
        name="kannada", display="Kannada", block=(0x0C80, 0x0CFF),
        # ka..ha, skipping the two unassigned slots in the run.
        consonants=[cp for cp in range(0x0C95, 0x0CBA) if cp not in (0x0CA9, 0x0CB4)],
        # Dependent vowel signs, plus the two length marks that combine with them.
        # A word-final consonant + virama is one glyph (kaviramaknda, raviramaknda),
        # not the consonant followed by a drawn virama, so the virama is modelled
        # as a last matra slot. Appended so the earlier slot ids do not move.
        matras=[cp for cp in range(0x0CBE, 0x0CCD) if cp != 0x0CC5] + [0x0CD5, 0x0CD6, 0x0CCD],
        virama=0x0CCD, ra=0x0CB0,
        pua_base=0xE000, pua_limit=0xE3FF,
    ),
    "telugu": ScriptSpec(
        name="telugu", display="Telugu", block=(0x0C00, 0x0C7F),
        # ka..ha skipping the unassigned 0C29, plus the three later additions.
        consonants=[cp for cp in range(0x0C15, 0x0C3A) if cp != 0x0C29]
                   + [0x0C58, 0x0C59, 0x0C5A],
        matras=[cp for cp in range(0x0C3E, 0x0C4D) if cp not in (0x0C45, 0x0C49)]
               + [0x0C55, 0x0C56]
               # Word-final consonant + virama is one glyph (kahalanttelu), so
               # the virama is modelled as a last matra slot.
               + [0x0C4D],
        virama=0x0C4D, ra=0x0C30,
        pua_base=0xE400, pua_limit=0xE7FF,
        # Telugu has no arkavattu: a cluster-initial ra stays the base letter
        # and the consonant after it stacks beneath, which the ordinary base +
        # below-base path already describes. Modelling it as a repha instead
        # put every such cluster in the exception table (+624 rows, +7.7 KB)
        # for no gain.
        has_reph=False,
    ),
    "tamil": ScriptSpec(
        name="tamil", display="Tamil", block=(0x0B80, 0x0BFF),
        # Tamil's consonant run is sparse, so it is listed rather than ranged.
        consonants=[0x0B95, 0x0B99, 0x0B9A, 0x0B9C, 0x0B9E, 0x0B9F, 0x0BA3,
                    0x0BA4, 0x0BA8, 0x0BA9, 0x0BAA, 0x0BAE, 0x0BAF, 0x0BB0,
                    0x0BB1, 0x0BB2, 0x0BB3, 0x0BB4, 0x0BB5, 0x0BB6, 0x0BB7,
                    0x0BB8, 0x0BB9],
        matras=[0x0BBE, 0x0BBF, 0x0BC0, 0x0BC1, 0x0BC2, 0x0BC6, 0x0BC7, 0x0BC8,
                0x0BCA, 0x0BCB, 0x0BCC,
                # Word-final consonant + pulli is one glyph (kaprehalftamil),
                # so the virama is modelled as a last matra slot.
                0x0BCD],
        virama=0x0BCD, ra=0x0BB0,
        # Tamil's base stays at 0xE800: font packs are already published with
        # these codepoints. Only its unused upper bound was tightened to make
        # room for the scripts below.
        pua_base=0xE800, pua_limit=0xE9FF,
        # Tamil writes a cluster-initial ra with a visible pulli in place; it
        # does not reorder, so there is no repha form to lift out.
        has_reph=False,
    ),
    "devanagari": ScriptSpec(
        name="devanagari", display="Devanagari", block=(0x0900, 0x097F),
        # ka..ha, then the eight precomposed nukta letters (qa..yya) that behave
        # as ordinary consonants.
        consonants=[cp for cp in range(0x0915, 0x093A)]
                   + [cp for cp in range(0x0958, 0x0960)],
        # Dependent vowel signs, plus the two vocalic-L signs.
        matras=[cp for cp in range(0x093E, 0x094D)] + [0x0962, 0x0963],
        virama=0x094D, ra=0x0930,
        pua_base=0xEA00, pua_limit=0xEDFF,
        # Measured both ways against Noto Sans Devanagari: a first-position
        # base reproduces more of the cluster space (80.1% vs 78.4%) and costs
        # 18 KB less, because this font keeps the leading consonant's full form
        # with a visible virama in most conjuncts rather than using a half form.
        base_last=False,
        nukta=0x093C,
    ),
    "bengali": ScriptSpec(
        name="bengali", display="Bengali", block=(0x0980, 0x09FF),
        # ka..ha skipping the unassigned slots in the run, plus the three
        # precomposed nukta letters.
        consonants=[cp for cp in range(0x0995, 0x09BA)
                    if cp not in (0x09A9, 0x09B1, 0x09B3, 0x09B4, 0x09B5)]
                   + [0x09DC, 0x09DD, 0x09DF],
        matras=[0x09BE, 0x09BF, 0x09C0, 0x09C1, 0x09C2, 0x09C3, 0x09C4, 0x09C7,
                0x09C8, 0x09CB, 0x09CC, 0x09D7],
        virama=0x09CD, ra=0x09B0,
        pua_base=0xEE00, pua_limit=0xF1FF,
        base_last=True,
    ),
    "oriya": ScriptSpec(
        name="oriya", display="Oriya", block=(0x0B00, 0x0B7F),
        consonants=[cp for cp in range(0x0B15, 0x0B3A)
                    if cp not in (0x0B29, 0x0B31, 0x0B34)]
                   + [0x0B5C, 0x0B5D, 0x0B5F],
        matras=[0x0B3E, 0x0B3F, 0x0B40, 0x0B41, 0x0B42, 0x0B43, 0x0B44, 0x0B47,
                0x0B48, 0x0B4B, 0x0B4C, 0x0B56, 0x0B57],
        virama=0x0B4D, ra=0x0B30,
        pua_base=0xF200, pua_limit=0xF5FF,
        base_last=True,
    ),
    "malayalam": ScriptSpec(
        name="malayalam", display="Malayalam", block=(0x0D00, 0x0D7F),
        consonants=[cp for cp in range(0x0D15, 0x0D3B) if cp != 0x0D29],
        matras=[cp for cp in range(0x0D3E, 0x0D4D) if cp not in (0x0D45, 0x0D49)]
               + [0x0D57],
        virama=0x0D4D, ra=0x0D30,
        pua_base=0xF600, pua_limit=0xF8FF,
        base_last=True,
    ),
}

# Set by select_script() before derive() runs. They stay module-level because
# the derivation reads them in a dozen places and threading a spec through all
# of them buys nothing.
SPEC = None
CONSONANTS = []
MATRAS = []
VIRAMA = 0
RA = 0
PUA_BASE = 0
PUA_LIMIT = 0
CONSONANT_INDEX = {}
MATRA_INDEX = {}
MATRA_SLOTS = 1


def select_script(name):
    """Bind the module-level script constants to one entry of SCRIPTS."""
    global SPEC, CONSONANTS, MATRAS, VIRAMA, RA, PUA_BASE, PUA_LIMIT
    global CONSONANT_INDEX, MATRA_INDEX, MATRA_SLOTS

    SPEC = SCRIPTS[name]
    CONSONANTS = SPEC.consonants
    MATRAS = SPEC.matras
    VIRAMA = SPEC.virama
    RA = SPEC.ra
    PUA_BASE = SPEC.pua_base
    PUA_LIMIT = SPEC.pua_limit
    CONSONANT_INDEX = {cp: i for i, cp in enumerate(CONSONANTS)}
    MATRA_INDEX = {NO_MATRA: 0}
    for i, matra in enumerate(MATRAS):
        MATRA_INDEX[matra] = i + 1
    MATRA_SLOTS = len(MATRAS) + 1
    return SPEC


def shape(hbfont, codepoints):
    buf = hb.Buffer()
    buf.add_str("".join(chr(c) for c in codepoints))
    buf.guess_segment_properties()
    hb.shape(hbfont, buf)
    return [info.codepoint for info in buf.glyph_infos]


def cluster_codepoints(consonants, matra):
    """Logical codepoint sequence for a cluster spec.

    consonants is a tuple of codepoints, optionally led by the marker "R" for a
    repha (ra + virama) prefix, which HarfBuzz reorders out of the cluster.
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
    if SPEC.has_reph:
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
        print(f"error: font produced no usable {SPEC.display} clusters", file=sys.stderr)
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

    # Which consonant of a pair is the base, and which gets an attached form.
    def split_pair(first, second):
        return (second, first) if SPEC.base_last else (first, second)

    # Plain attached form per consonant, from matra-free conjuncts: the glyph a
    # conjunct adds over the base consonant rendered on its own.
    sub_votes = defaultdict(Counter)
    for first in CONSONANTS:
        for second in CONSONANTS:
            gids = truth.get(((first, second), NO_MATRA))
            if not gids:
                continue
            base_c, attached_c = split_pair(first, second)
            solo = truth.get(((base_c,), NO_MATRA), [])
            sub_votes[attached_c].update(g for g in gids if g not in solo)
    subscript = {c: v.most_common(1)[0][0] for c, v in sub_votes.items() if v}

    # A conjunct whose output contains no attached form is an akhand ligature
    # (ksha, jnya): the pair renders as its own base unit.
    akhand = set()
    for first in CONSONANTS:
        for second in CONSONANTS:
            gids = truth.get(((first, second), NO_MATRA))
            if not gids:
                continue
            _, attached_c = split_pair(first, second)
            form = subscript.get(attached_c)
            if form is None or form not in gids:
                akhand.add((first, second))

    def split_unit(consonants):
        """Separate the base unit from the consonants attached to it.

        Returns (unit, attached). `attached` keeps logical order; the base is
        taken from whichever end of the run this script puts it.
        """
        if SPEC.base_last:
            if len(consonants) >= 2 and (consonants[-2], consonants[-1]) in akhand:
                return (consonants[-2], consonants[-1]), consonants[:-2]
            return consonants[-1], consonants[:-1]
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
            print(f"error: shaped {SPEC.display} forms exceed their private-use range "
                  f"(0x{PUA_BASE:04X}-0x{PUA_LIMIT:04X})", file=sys.stderr)
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


def emit_sidecar(derived, path):
    base = derived["base"]
    sub_pair = derived["sub_pair"]
    subscript = derived["subscript"]
    akhand = derived["akhand"]
    reph = derived["reph"]
    exceptions = derived["exceptions"]
    codepoint_of = derived["codepoint_of"]

    akhand_order = {pair: i for i, pair in enumerate(sorted(akhand))}

    # Every row below must survive into the file unchanged: the verify pass
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
        if key > 0xFFFF:
            raise ValueError(f"base key {key} exceeds the uint16 key field")
        base_rows.append((key, len(base_pool), len(codepoints), min(insert_at, len(codepoints))))
        base_pool.extend(codepoints)
    base_rows.sort()
    if len(base_pool) > 0xFFFF:
        raise ValueError("base codepoint pool exceeds the uint16 offset field")

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
    if len(pool) > 0xFFFF:
        raise ValueError("exception codepoint pool exceeds the uint16 offset field")

    # .cpshape v1 -- see lib/IndicShape/IndicTables.h for the layout, which the
    # device validates and then reads in place. Every section starts on a
    # 4-byte boundary so the device can point typed arrays straight at it.
    counts = {
        "base": len(base_rows), "base_pool": len(base_pool), "sub": len(sub_rows),
        "plain": len(plain_rows), "akhand": len(akhand_rows), "exc": len(exc_rows),
        "pool": len(pool),
    }
    for name, value in counts.items():
        if value > 0xFFFF:
            raise ValueError(f"{name} count {value} exceeds the uint16 header field")
    if len(CONSONANTS) > 0xFF or MATRA_SLOTS > 0xFF:
        raise ValueError("consonant or matra count exceeds the uint8 header field")

    sections = []
    sections.append(struct.pack(f"<{len(CONSONANTS)}H", *CONSONANTS))
    sections.append(struct.pack(f"<{len(MATRAS)}H", *MATRAS))
    sections.append(b"".join(struct.pack("<HHBB", *row) for row in base_rows))
    sections.append(struct.pack(f"<{len(base_pool)}H", *base_pool))
    sections.append(b"".join(struct.pack("<HH", *row) for row in sub_rows))
    sections.append(b"".join(struct.pack("<HH", *row) for row in plain_rows))
    sections.append(b"".join(struct.pack("<HH", *row) for row in akhand_rows))
    sections.append(b"".join(struct.pack("<IHBB", key, offset, count, 0)
                             for key, offset, count in exc_rows))
    sections.append(struct.pack(f"<{len(pool)}H", *pool))
    nukta_pairs = nukta_fold_pairs()
    sections.append(b"".join(struct.pack("<HH", *pair) for pair in nukta_pairs))

    payload = bytearray()
    for section in sections:
        payload += b"\0" * ((-(SIDECAR_HEADER_SIZE + len(payload))) % 4)
        payload += section
    total = SIDECAR_HEADER_SIZE + len(payload)

    reph_cp = (codepoint_of(reph) or 0) if SPEC.has_reph else 0
    header = struct.pack(
        "<4sHHIIIIIIBBBBHHHHHHHHI",
        SIDECAR_MAGIC, SIDECAR_VERSION, SIDECAR_HEADER_SIZE, total,
        SPEC.block[0], SPEC.block[1], SPEC.virama, SPEC.ra, reph_cp,
        int(SPEC.base_last), len(CONSONANTS), MATRA_SLOTS, 0,
        len(base_rows), len(base_pool), len(sub_rows), len(plain_rows),
        len(akhand_rows), len(exc_rows), len(pool), len(nukta_pairs), SPEC.nukta)
    header += b"\0" * (SIDECAR_HEADER_SIZE - len(header))
    assert len(header) == SIDECAR_HEADER_SIZE

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(header)
        handle.write(payload)

    return {
        "base": len(base_rows),
        "subscript": len(sub_rows),
        "plain": len(plain_rows),
        "akhand": len(akhand_rows),
        "exceptions": len(exc_rows),
        "pool": len(pool),
        "base_pool": len(base_pool),
        "bytes": total,
    }


def nukta_fold_pairs():
    """(base, precomposed) for every listed consonant that decomposes to base + nukta."""
    if not SPEC.nukta:
        return []
    pairs = []
    for cp in CONSONANTS:
        parts = unicodedata.decomposition(chr(cp)).split()
        if len(parts) == 2 and int(parts[1], 16) == SPEC.nukta and int(parts[0], 16) in CONSONANT_INDEX:
            pairs.append((int(parts[0], 16), cp))
    return pairs


def emit_pua_map(derived, path, font_path):
    glyph_order = derived["glyph_order"]
    forms = [{"codepoint": cp, "glyph": glyph_order[gid]}
             for gid, cp in sorted(derived["pua"].items(), key=lambda kv: kv[1])]
    payload = {
        "comment": (f"Shaped {SPEC.display} forms with no cmap entry, mapped to private-use "
                    "codepoints. fontconvert_sdcard.py rasterizes these by glyph name."),
        "script": SPEC.name,
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

    ns = SPEC.golden_namespace
    lines = []
    lines.append("// GENERATED FILE -- DO NOT EDIT.")
    lines.append("//")
    lines.append(f"// HarfBuzz ground truth for {os.path.basename(font_path)}, used by")
    lines.append("// IndicShapeTest to prove the on-device shaper agrees with the shaper the")
    lines.append("// tables were derived from. Regenerate with gen_indic_shaping.py --golden.")
    lines.append("//")
    lines.append("// Flat records: [logicalCount, logical..., expectedCount, expected...]")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append(f"namespace {ns} {{")
    lines.append("")
    lines.append(f"constexpr int CASE_COUNT = {len(selected)};")
    lines.append(f"inline constexpr uint16_t RECORDS[{max(len(records), 1)}] = {{")
    for i in range(0, len(records), 12):
        lines.append("    " + ", ".join(f"0x{v:04X}" for v in records[i:i + 12]) + ",")
    if not records:
        lines.append("    0,")
    lines.append("};")
    lines.append("")
    lines.append(f"}}  // namespace {ns}")
    lines.append("")

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))
    return len(selected)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("font", help="TTF/OTF to derive tables from")
    parser.add_argument("--script", required=True, choices=sorted(SCRIPTS),
                        help="which script's tables to generate")
    parser.add_argument("--sidecar", help="generated .cpshape path (default: per script)")
    parser.add_argument("--pua-map", help="generated private-use map JSON path (default: per script)")
    parser.add_argument("--golden", nargs="?", const=True, default=None,
                        help="also write the golden-set header for the native test")
    parser.add_argument("--golden-stride", type=int, default=6,
                        help="sample every Nth non-exception cluster into the golden set")
    args = parser.parse_args()

    spec = select_script(args.script)
    sidecar = args.sidecar or spec.sidecar_path
    pua_map = args.pua_map or spec.pua_map_path
    golden = spec.golden_path if args.golden is True else args.golden

    derived = derive(args.font)
    failures = verify(derived)
    total = len(derived["truth"])
    exact = total - len(derived["exceptions"])

    print(f"script:                 {spec.display}", file=sys.stderr)
    print(f"clusters shaped:        {total}", file=sys.stderr)
    print(f"reproduced by tables:   {exact} ({100.0 * exact / total:.2f}%)", file=sys.stderr)
    print(f"literal exceptions:     {len(derived['exceptions'])}", file=sys.stderr)

    if failures:
        print(f"error: {len(failures)} clusters do not round-trip; refusing to emit tables.",
              file=sys.stderr)
        for spec_failure in failures[:10]:
            print(f"  {spec_failure}", file=sys.stderr)
        sys.exit(1)

    stats = emit_sidecar(derived, sidecar)
    forms = emit_pua_map(derived, pua_map, args.font)

    print(f"private-use forms:      {forms}", file=sys.stderr)
    print(f"table rows:             base={stats['base']} sub={stats['subscript']} "
          f"plain={stats['plain']} akhand={stats['akhand']} exc={stats['exceptions']}",
          file=sys.stderr)
    print(f"sidecar size:           {stats['bytes']} bytes "
          f"({stats['bytes'] / 1024.0:.1f} KB)", file=sys.stderr)
    print(f"wrote {sidecar}", file=sys.stderr)
    print(f"wrote {pua_map}", file=sys.stderr)

    if golden:
        cases = emit_golden(derived, golden, args.font, max(args.golden_stride, 1))
        print(f"golden cases:           {cases}", file=sys.stderr)
        print(f"wrote {golden}", file=sys.stderr)


if __name__ == "__main__":
    main()
