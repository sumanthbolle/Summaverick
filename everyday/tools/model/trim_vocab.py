#!/usr/bin/env python3
"""Copy a Gemma 4 GGUF with a smaller vocabulary: only tokens the app's languages can use.

Usage: python3 -I tools/model/trim_vocab.py <in.gguf> <out.gguf> --keep-scripts latin,telugu,devanagari
                                            [--keep-observed tokens.txt] [--dry-run]   (needs the `gguf` package)

A token is kept when any of these holds:
  - its type is not "normal" (control, user-defined, unknown, byte-fallback <0xXX>);
  - every character is in a kept script or is common: ASCII, Unicode punctuation (P*), separators (Z*),
    symbols (S*: currency, arrows, emoji ...), General Punctuation incl. ZWJ/ZWNJ, the "▁" space marker;
  - its id is listed in tokens.txt (ids the full model used on the test set, see compare_outputs.sh);
  - it is a part of a BPE merge that produces a kept token (closure, so every kept token can still be built).
Kept tokens keep their order: new id = rank among kept ids (pad stays 0, llama.cpp's multimodal path
reads row 0 of per_layer_token_embd by number).

Rewritten: tokenizer.ggml.tokens / scores / token_type (sliced), tokenizer.ggml.merges (only merges whose
two parts and result are kept, original order), tokenizer.ggml.*_token_id (remapped), and the rows of
token_embd.weight (also the tied output) and per_layer_token_embd.weight. Every other tensor and key
is copied as is. The output is reopened and checked before the sizes are printed.
"""
import argparse
import os
import sys
import unicodedata

import numpy as np
from gguf import GGUFReader, GGUFValueType, GGUFWriter

SCRIPTS = {
    "latin": [(0x0000, 0x024F), (0x0300, 0x036F), (0x1E00, 0x1EFF)],
    "telugu": [(0x0C00, 0x0C7F)],
    "devanagari": [(0x0900, 0x097F), (0xA8E0, 0xA8FF), (0x1CD0, 0x1CFF)],
}
COMMON = [(0x0000, 0x007F), (0x2000, 0x206F), (0x2581, 0x2581), (0xFE0F, 0xFE0F)]
VOCAB_TENSORS = ("token_embd.weight", "per_layer_token_embd.weight")
ID_KEYS = ("bos", "eos", "unknown", "padding", "mask", "seperator", "cls", "eot", "eom")
NORMAL = 1
SKIP_KEYS = {"GGUF.version", "GGUF.tensor_count", "GGUF.kv_count", "general.architecture"}


def char_ok(c, ranges):
    o = ord(c)
    if any(a <= o <= b for a, b in ranges):
        return True
    return unicodedata.category(c)[0] in "PZS"


def merge_parts(m):
    # llama.cpp: split at the first space after position 0
    p = m.find(" ", 1)
    return (m[:p], m[p + 1:]) if p > 0 else (m, "")


def plan(r, scripts, observed):
    toks = r.fields["tokenizer.ggml.tokens"].contents()
    types = r.fields["tokenizer.ggml.token_type"].contents()
    merges = r.fields["tokenizer.ggml.merges"].contents()
    ranges = COMMON + [rg for s in scripts for rg in SCRIPTS[s]]
    keep = set()
    why = {"special/byte": 0, "script": 0, "observed only": 0, "merge closure": 0}
    for i, t in enumerate(toks):
        if types[i] != NORMAL:
            keep.add(i); why["special/byte"] += 1
        elif all(char_ok(c, ranges) for c in t):
            keep.add(i); why["script"] += 1
    bad = [i for i in observed if not 0 <= i < len(toks)]
    if bad:
        sys.exit(f"FAIL: observed ids out of range: {bad[:5]}")
    for i in observed:
        if i not in keep:
            keep.add(i); why["observed only"] += 1
    # closure: the parts of every merge that builds a kept token are kept too
    tid = {t: i for i, t in enumerate(toks)}
    makers = {}
    for m in merges:
        a, b = merge_parts(m)
        makers.setdefault(a + b, []).append((a, b))
    stack = list(keep)
    while stack:
        for a, b in makers.get(toks[stack.pop()], ()):
            for part in (a, b):
                j = tid.get(part)
                if j is not None and j not in keep:
                    keep.add(j); why["merge closure"] += 1; stack.append(j)
    kept = sorted(keep)
    kept_text = {toks[i] for i in kept}
    new_merges = [m for m in merges if all(x in kept_text for x in (*merge_parts(m), "".join(merge_parts(m))))]
    return kept, new_merges, why


def rows(t, kept):
    n_rows = int(t.shape[-1])
    data = np.asarray(t.data).reshape(n_rows, -1)
    return np.ascontiguousarray(data[np.asarray(kept)])


def verify(src, dst, kept, new_merges):
    s, d = GGUFReader(src), GGUFReader(dst)
    if d.alignment != s.alignment:
        sys.exit("FAIL: alignment differs")
    for k in ("tokens", "scores", "token_type"):
        a = s.fields[f"tokenizer.ggml.{k}"].contents()
        b = d.fields[f"tokenizer.ggml.{k}"].contents()
        if len(b) != len(kept) or any(b[j] != a[i] for j, i in enumerate(kept)):
            sys.exit(f"FAIL: tokenizer.ggml.{k} not the kept slice")
    if d.fields["tokenizer.ggml.merges"].contents() != new_merges:
        sys.exit("FAIL: merges differ")
    stoks, dtoks = s.fields["tokenizer.ggml.tokens"].contents(), d.fields["tokenizer.ggml.tokens"].contents()
    for k in ID_KEYS:
        key = f"tokenizer.ggml.{k}_token_id"
        if key in s.fields:
            if stoks[s.fields[key].contents()] != dtoks[d.fields[key].contents()]:
                sys.exit(f"FAIL: {key} points at another token")
    if dtoks[0] != stoks[0]:
        sys.exit("FAIL: token 0 moved")
    want = {t.name: t for t in s.tensors}
    got = {t.name: t for t in d.tensors}
    if set(got) != set(want):
        sys.exit("FAIL: tensor sets differ")
    for n, t in got.items():
        o = want[n]
        if t.tensor_type != o.tensor_type:
            sys.exit(f"FAIL: {n} type differs")
        if n in VOCAB_TENSORS:
            if int(t.shape[-1]) != len(kept) or list(t.shape[:-1]) != list(o.shape[:-1]):
                sys.exit(f"FAIL: {n} shape {list(t.shape)}")
            if not np.array_equal(np.asarray(t.data).reshape(len(kept), -1), rows(o, kept)):
                sys.exit(f"FAIL: {n} rows differ")
        elif list(t.shape) != list(o.shape) or bytes(t.data) != bytes(o.data):
            sys.exit(f"FAIL: tensor {n} differs")
    print(f"verify OK: {len(kept)} tokens, {len(new_merges)} merges, vocab tensors are the kept rows, "
          f"{len(got) - len(VOCAB_TENSORS)} other tensors identical")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--keep-scripts", required=True)
    ap.add_argument("--keep-observed")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    scripts = [s for s in a.keep_scripts.split(",") if s]
    unknown = [s for s in scripts if s not in SCRIPTS]
    if unknown:
        sys.exit(f"unknown script(s) {unknown}; known: {', '.join(SCRIPTS)}")
    observed = []
    if a.keep_observed:
        with open(a.keep_observed) as f:
            observed = sorted({int(x) for x in f.read().split()})

    r = GGUFReader(a.src)
    if r.fields["tokenizer.ggml.model"].contents() != "gemma4":
        sys.exit("only tokenizer.ggml.model = gemma4 (BPE, <0xXX> byte fallback) is handled")
    n_vocab = len(r.fields["tokenizer.ggml.tokens"].contents())
    kept, new_merges, why = plan(r, scripts, observed)
    old_id = {i: j for j, i in enumerate(kept)}
    n_merges = len(r.fields["tokenizer.ggml.merges"].contents())
    print(f"vocabulary: {n_vocab} -> {len(kept)} tokens kept ({', '.join(f'{k} {v}' for k, v in why.items())})")
    print(f"observed ids: {len(observed)}, outside the script rule: {why['observed only']}")
    print(f"merges: {n_merges} -> {len(new_merges)}")
    vocab_bytes = sum(t.n_bytes for t in r.tensors if t.name in VOCAB_TENSORS)
    print(f"vocab tensors: {vocab_bytes} bytes -> about {vocab_bytes * len(kept) // n_vocab} bytes")
    if a.dry_run:
        return

    arch = r.fields["general.architecture"].contents()
    w = GGUFWriter(a.dst, arch)
    w.data_alignment = r.alignment
    sliced = {"tokenizer.ggml.tokens", "tokenizer.ggml.scores", "tokenizer.ggml.token_type"}
    for key, f in r.fields.items():
        if key in SKIP_KEYS or key in ("alignment", "general.alignment"):
            continue
        v = f.contents()
        t = f.types[0]
        if key in sliced:
            v = [v[i] for i in kept]
        elif key == "tokenizer.ggml.merges":
            v = new_merges
        elif key.startswith("tokenizer.ggml.") and key.endswith("_token_id"):
            if v not in old_id:
                sys.exit(f"FAIL: {key} = {v} was not kept")
            v = old_id[v]
        if t == GGUFValueType.ARRAY:
            w.add_key_value(key, v, t, sub_type=f.types[-1])
        else:
            w.add_key_value(key, v, t)
    w.add_string("summaverick.vocab_trim", f"kept {len(kept)} of {n_vocab} tokens; scripts {','.join(scripts)}"
                 f"{' + observed test-set ids' if observed else ''}")
    for t in r.tensors:
        data = rows(t, kept) if t.name in VOCAB_TENSORS else t.data
        w.add_tensor(t.name, data, raw_dtype=t.tensor_type)
    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file(progress=False)
    w.close()
    verify(a.src, a.dst, kept, new_merges)
    sa, sb = os.path.getsize(a.src), os.path.getsize(a.dst)
    print(f"before: {sa} bytes ({sa / 1e9:.2f} GB)")
    print(f"after:  {sb} bytes ({sb / 1e9:.2f} GB), saved {(sa - sb) / 1e6:.0f} MB")


if __name__ == "__main__":
    main()
