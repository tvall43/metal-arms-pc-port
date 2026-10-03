#!/usr/bin/env python3
"""Unwrap anonymous structs used as union members: 'struct { T n; };' -> 'T n;'

The engine's math types are type-punning unions, e.g. CFVec3A:

    union {
        struct { f32 x, y, z, w; };
        struct { CFVec2 v2; };
        struct { CFVec4A v4a; };
        f32 a[4];
    };

Scalar anonymous structs are harmless. But as soon as a member has a
non-trivial constructor (CFVec2/CFVec3/CFVec4A all declare constructors and
copy-assignment), GCC rejects the anonymous-struct wrapper even under
-std=gnu++11: C++11 unrestricted unions permit such members only when they
sit *directly* in the union, not nested in an anonymous struct.

The anonymous union still injects its members into the enclosing class scope,
so '.v4a' / '.m_Pos' resolve exactly as before and the layout is byte-for-byte
unchanged. Scalar-typed anonymous structs are left untouched.
"""
import re, sys, os

DECL = re.compile(rb'^\s*((?:const\s+)?(?:unsigned\s+|signed\s+)?[A-Za-z_]\w*)\s*((?:\w+\s*(?:,\s*\w+\s*)*)?);\s*(//[^\r\n]*)?$')

def is_scalar(t):
    t = t.replace(b'const', b'').strip()
    return t in (b'f32', b'f64', b'u8', b'u16', b'u32', b'u64',
                 b's8', b's16', b's32', b's64', b'int', b'float',
                 b'double', b'char', b'short', b'long', b'BOOL', b'bool')

def match_brace(b, i):
    """b[i] must be '{'; return index just past the matching '}'."""
    assert b[i:i+1] == b'{'
    depth, j = 0, i
    while j < len(b):
        c = b[j:j+1]
        if c == b'{': depth += 1
        elif c == b'}':
            depth -= 1
            if depth == 0: return j + 1
        j += 1
    raise ValueError('unbalanced')

def process(path):
    b = open(path, 'rb').read()
    changed = 0
    # Repeatedly find the first un-unwrapped `struct {` directly inside a union.
    while True:
        hit = None
        for um in re.finditer(rb'\bunion\s*\{', b):
            try: uend = match_brace(b, um.end() - 1)
            except ValueError: continue
            # Walk the union body looking for `struct {` sitting directly in
            # it. um.end() is already one past the union's '{', so we start
            # one level inside the union.
            i = um.end()
            depth = 1
            while i < uend - 1:
                c = b[i:i+1]
                if c == b'{':
                    depth += 1; i += 1; continue
                if c == b'}':
                    depth -= 1; i += 1; continue
                if depth == 1:
                    m = re.match(rb'struct\s*\{', b[i:i+20])
                    if m:
                        send = match_brace(b, i + m.end() - 1)
                        # must be followed by ';' (an anonymous member)
                        j = send
                        while j < len(b) and b[j:j+1] in b' \t': j += 1
                        if b[j:j+1] == b';':
                            body = b[i + m.end():send - 1]
                            decls = [d for d in body.split(b';') if d.strip()]
                            parsed = [DECL.match(d + b';') for d in decls]
                            ok = parsed and all(parsed)
                            if ok:
                                scalars_only = all(is_scalar(p.group(1)) for p in parsed)
                                if not scalars_only:
                                    hit = (i, j + 1, decls, parsed)
                                    break
                i += 1
            if hit: break
        if not hit: break
        s, e, decls, parsed = hit
        # rebuild: one declaration per original, keeping trailing comments
        parts = []
        for p in parsed:
            c = p.group(3) or b''
            parts.append(b'\t\t' + p.group(1) + b' ' + p.group(2) + b';' + c)
        b = b[:s] + b'\r\n'.join(parts) + b[e:]
        changed += 1
    if changed:
        open(path, 'wb').write(b)
        print(f"  {changed:3d}  {path}")
    return changed

total = 0
for root in sys.argv[1:]:
    for dp, _, fns in os.walk(root):
        for fn in sorted(fns):
            if fn.endswith(('.h', '.inl', '.cpp')):
                total += process(os.path.join(dp, fn))
print("total unwrapped:", total)
