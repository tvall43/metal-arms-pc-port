#!/usr/bin/env python3
"""Convert backslashes to forward slashes in quoted #include targets.

MSVC happily resolved "ai\\aicorrosive.h" and "dx\\fdx8.h"; GCC treats the
backslash as part of the filename and fails to find anything. Forward slashes
work on every toolchain, and the include paths in CMake are already
slash-separated.
"""
import re, sys, os

INC = re.compile(rb'(#\s*include\s+")([^"]*)(")')
changed = 0

def fix(p):
    global changed
    b = open(p, 'rb').read()
    def sub(m):
        global changed
        t = m.group(2)
        if b'\\' in t:
            changed_local = t.replace(b'\\', b'/')
            return m.group(1) + changed_local + m.group(3)
        return m.group(0)
    out = INC.sub(sub, b)
    if out != b:
        open(p, 'wb').write(out)
    return out != b

files = 0
for root in sys.argv[1:]:
    for dp, _, fns in os.walk(root):
        for fn in sorted(fns):
            if fn.endswith(('.c', '.cpp', '.h', '.inl')):
                if fix(os.path.join(dp, fn)):
                    files += 1
print(f"files updated: {files}")
