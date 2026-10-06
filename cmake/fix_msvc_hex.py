#!/usr/bin/env python3
"""Rewrite MSVC hex literals ('00h', 'FFFFFFFFh') to C form ('0x00', '0xFFFFFFFF').

The engine's vector math is hand-written x86 inline assembly written for
MSVC, which uses an 'h' suffix on hex constants. GCC rejects that with
'invalid suffix "h" on integer constant', and the resulting parse failure
cascades (registers like xmm0/mov then get parsed as C++ identifiers).

Only tokens inside __asm { ... } blocks are rewritten, and only when they
are delimited like real literals, so identifiers such as "length" or
"Push" are never touched.
"""
import re, sys, os

# A literal: 1+ hex digits then 'h', delimited by whitespace/punctuation.
LIT = re.compile(
    rb'(?<=[ \t,\(\[\{=+\-*/%&|^~!<>])([0-9A-Fa-f]+)h(?=[ \t,)\]\};\r\n+\-*/%&|])')

def rewrite(data: bytes) -> tuple[bytes, int]:
    out, changed, pos = bytearray(), 0, 0
    # Walk __asm { ... } regions. Blocks do not nest in this codebase.
    for m in re.finditer(rb'__asm\s*\{', data):
        # flush untouched text before this block
        out += data[pos:m.end()]
        # find matching close brace
        depth, i = 1, m.end()
        while i < len(data) and depth:
            if data[i:i+1] == b'{': depth += 1
            elif data[i:i+1] == b'}': depth -= 1
            i += 1
        body = data[m.end():i]
        def sub(mm):
            nonlocal changed
            tok = mm.group(1)
            # Require >= 2 hex digits: MSVC asm literals are byte/word sized.
            if len(tok) < 2:
                return mm.group(0)
            changed += 1
            return b'0x' + tok
        out += LIT.sub(sub, body)
        pos = i
    out += data[pos:]
    return bytes(out), changed

def main(paths):
    total = 0
    for p in paths:
        b = open(p, 'rb').read()
        nb, n = rewrite(b)
        if n:
            open(p, 'wb').write(nb)
            print(f"  {n:4d}  {p}")
            total += n
    print(f"total literals rewritten: {total}")

if __name__ == '__main__':
    files = []
    for root in sys.argv[1:]:
        if os.path.isfile(root):
            files.append(root)
            continue
        for dp, _, fns in os.walk(root):
            for fn in fns:
                if fn.endswith(('.c', '.cpp', '.h', '.inl', '.hpp')):
                    files.append(os.path.join(dp, fn))
    main(files)
