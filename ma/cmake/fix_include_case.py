#!/usr/bin/env python3
"""Rewrite #include "..." targets to the on-disk spelling.

Visual Studio and the CodeWarrior-era build were case-insensitive; GCC is
not, so 'fcamshake.h' does not resolve when the file is FCamShake.h. This
walks each include path and rewrites the quoted target to the real filename,
preserving the original relative form.
"""
import os, re, sys

INC = re.compile(rb'#\s*include\s+"([^"]+)"')

def resolve(name, incdirs):
    """Try name, then name.lower(), then case-insensitive search."""
    if os.path.isfile(name):
        return name
    for d in incdirs:
        cand = os.path.join(d, name)
        if os.path.isfile(cand):
            return cand
    return None

def find_ci(target, searchdirs):
    """Case-insensitive lookup of `target`'s basename in each search dir.

    Returns (newtarget, resolvedpath) or None. Keeps any relative directory
    prefix from the original include so nested includes like "../foo.h" and
    "gc/fGCdata.h" keep pointing at the same place.
    """
    d, base = os.path.split(target)
    tl = base.lower()
    for sd in searchdirs:
        if not os.path.isdir(sd):
            continue
        for fn in os.listdir(sd):
            if fn.lower() == tl and fn != base:
                return os.path.join(d, fn) if d else fn, os.path.join(sd, fn)
    return None

def process(path, incdirs):
    b = open(path, 'rb').read()
    hits = []

    def sub(m):
        target = m.group(1).decode()
        if resolve(target, incdirs):
            return m.group(0)                     # already correct
        r = find_ci(target, [os.path.dirname(path)] + list(incdirs))
        if r:
            newtarget, realpath = r
            hits.append((target, realpath))
            return b'#include "' + newtarget.encode() + b'"'
        hits.append((target, None))               # genuinely absent
        return m.group(0)

    out = INC.sub(sub, b)
    if out != b:
        open(path, 'wb').write(out)
    return hits

def main(root, incdirs):
    fixed = missing = 0
    for dp, _, fns in os.walk(root):
        for fn in sorted(fns):
            if not fn.endswith(('.c', '.cpp', '.h', '.inl')):
                continue
            p = os.path.join(dp, fn)
            for target, real in process(p, incdirs):
                if real:
                    print(f"  {target}  ->  {real}   ({p})")
                    fixed += 1
                else:
                    print(f"  MISSING (not a case issue): {target}   ({p})")
                    missing += 1
    print(f"case-fixed: {fixed}   missing: {missing}")

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2:])
