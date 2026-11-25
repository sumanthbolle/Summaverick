#!/usr/bin/env python3
"""Compare the string keys the compiler extracted with Localizable.xcstrings.

Usage: check_strings.py <derived-data-dir>
Run after an Xcode build. Exits 1 when a compiled key is missing from the
catalog, a catalog key is no longer compiled, or a translatable key has no
Telugu ("te") value. Also fails when a Telugu value uses other format specifiers than its key.
"""
import glob
import json
import os
import re
import sys

SPEC = re.compile(r"%(?:\d+\$)?(lld|ld|d|@|%)")

HERE = os.path.dirname(os.path.abspath(__file__))
CATALOG = os.path.join(HERE, "..", "SummaverickEveryday", "Localizable.xcstrings")


def compiled_keys(dd):
    files = glob.glob(os.path.join(dd, "Build", "Intermediates.noindex", "**", "*.stringsdata"), recursive=True)
    if not files:
        sys.exit(f"no .stringsdata files under {dd}; build first")
    keys = {}
    for path in files:
        with open(path) as f:
            data = json.load(f)
        for entry in data.get("tables", {}).get("Localizable", []):
            keys.setdefault(entry["key"], set()).add(os.path.basename(data.get("source", path)))
    return keys


def te_values(entry):
    te = entry.get("localizations", {}).get("te", {})
    if "stringUnit" in te:
        yield te["stringUnit"]["value"]
    for forms in te.get("variations", {}).values():
        for form in forms.values():
            yield form["stringUnit"]["value"]


def specs(text):
    return sorted(SPEC.findall(text))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    compiled = compiled_keys(sys.argv[1])
    with open(CATALOG) as f:
        strings = json.load(f)["strings"]

    not_in_catalog = sorted(set(compiled) - set(strings))
    stale = sorted(set(strings) - set(compiled))
    no_te = sorted(
        k for k, v in strings.items()
        if v.get("shouldTranslate", True) and "te" not in v.get("localizations", {})
    )
    bad_specs = sorted(
        k for k, v in strings.items()
        if any(specs(t) != specs(k) for t in te_values(v)) and "variations" not in v.get("localizations", {}).get("te", {})
    )
    untranslated = sum(1 for v in strings.values() if not v.get("shouldTranslate", True))

    print(f"compiled keys: {len(compiled)}  catalog keys: {len(strings)}  shouldTranslate=false: {untranslated}")
    for title, items in (("compiled but not in catalog", not_in_catalog),
                         ("in catalog but not compiled", stale),
                         ("translatable without te", no_te),
                         ("te value with different format specifiers", bad_specs)):
        print(f"{title}: {len(items)}")
        for k in items:
            where = ", ".join(sorted(compiled.get(k, []))) if k in compiled else ""
            print(f"  {k!r} {where}")
    ok = not (not_in_catalog or stale or no_te or bad_specs)
    print("STRINGS OK" if ok else "STRINGS MISMATCH")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
