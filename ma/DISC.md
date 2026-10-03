# Disc assets

The original Xbox disc is an XDVDFS image (7-Zip cannot read it; that is why
it looks like an opaque blob). Use the standard tool:

    tools/extract-xiso -l "<image>.xiso.iso"              # list
    tools/extract-xiso -x -d <outdir> "<image>.xiso.iso"   # extract

`tools/extract-xiso` is the official Linux binary from
https://github.com/XboxDev/extract-xiso (v2.7.1). Do not hand-roll a parser:
a directory is a binary search tree, so entries must be walked via their
left/right links -- reading them sequentially yields garbage.

There is also a retail DVD-Video ISO (ISO 9660 + UDF, label `SEP13011042`) whose
only session is `VIDEO_TS`; 7-Zip reads that one directly. It holds the
attract-mode MPEG-2 streams, not game data.

## Extracted layout (1,512,587,852 bytes, 207 files)

    default.xbe            4,116,480   Xbox executable (ENCRYPTED - see below)
    mettlearms_xb.mst    558,755,840   Fang master file; magic "FANG".
                                     App/ma/xb/main.cpp:40 opens exactly this
                                     name, so it is the file the game expects.
    *.wvs                          166  sound banks
    Movies/*.bik                    39  Bink videos

## default.xbe

The header parses once you use the correct layout (`HeaderSignature[256]`
occupies 0x004-0x104, so fields only begin at 0x104):

    BaseAddress   0x00010000      ImageSize  4,431,616
    EntryPoint    0xa8dfe8ea  ^ XOR_EP_RETAIL (0xA8FC57AB) = 0x0023bf41
    KernelThunk   0x5b5975d6  ^ XOR_KT_RETAIL (0x5B6D40B6) = 0x00343560

Both derived values are plausible, so the header itself is fine. The section
table does not resolve to sane values, which indicates the retail image body is
encrypted (beyond the two published XOR'd header fields) and needs the
per-title key. Not pursued further -- it is a reverse-engineering task, not a
porting one, and it is not on the critical path for the assets above.

Consequence for shaders: the compiled D3D8 token streams we want are not
obtainable from this file here. The 120 `.vsh`/`.psh` assembly sources in the
tree are intact, but assembling them needs Microsoft's DirectX 8 `vsa.exe` /
`psa.exe`, which are Windows-only and long obsolete. D3D9's `fxc` cannot target
`vs_1_1`/`ps_1_1`, so it is not a substitute.
