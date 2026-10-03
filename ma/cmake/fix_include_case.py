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

def resolve_ci(target, searchdirs):
    """Case-insensitively resolve a quoted include target.

    Every path component is matched case-insensitively, not just the basename,
    because the tree also gets *directory* names wrong: the real path is
    App/ma/Ai/AIMain.h but sources write "AI/aimain.h".
    """
    parts = [p for p in target.replace('\\', '/').split('/') if p and p != '.']
    if not parts:
        return None
    for sd in searchdirs:
        cur = sd
        ok = True
        for part in parts:
            cand = os.path.join(cur, part)
            if os.path.isdir(cand) or os.path.isfile(cand):
                cur = cand
                continue
            low = part.lower()
            match = None
            if os.path.isdir(cur):
                for fn in os.listdir(cur):
                    if fn.lower() == low:
                        match = fn
                        break
            if match is None:
                ok = False
                break
            cur = os.path.join(cur, match)
        if ok and (os.path.isfile(cur) or os.path.isdir(cur)):
            # Report the corrected target with the same relative shape.
            fixed = os.path.relpath(cur, sd)
            return fixed.replace(os.sep, '/'), cur
    return None

def process(path, incdirs):
    b = open(path, 'rb').read()
    hits = []

    def sub(m):
        target = m.group(1).decode()
        if resolve(target, incdirs):
            return m.group(0)                     # already correct
        r = resolve_ci(target, [os.path.dirname(path)] + list(incdirs))
        if r and r[0] != target:
            newtarget, realpath = r
            hits.append((target, realpath))
            return b'#include "' + newtarget.encode() + b'"'
        hits.append((target, None))               # genuinely absent
        return m.group(0)

    out = INC.sub(sub, b)
    if out != b:
        open(path, 'wb').write(out)
    return hits

def main(roots):
    # Every argument is scanned AND searched, so one invocation covers the whole
    # project. An earlier version walked only argv[1] and treated the rest as
    # include dirs, which silently skipped every other tree.
    incdirs = [r for r in roots if os.path.isdir(r)]
    fixed = missing = 0
    for root in roots:
        if not os.path.isdir(root):
            continue
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
    main(sys.argv[1:])
