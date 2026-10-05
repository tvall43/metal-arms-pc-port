# Metal Arms Windows port — handoff

Written by the previous agent, which ran out of useful ideas rather than out of
builds. Read the whole thing before changing anything; most of it is a list of
things that have already been tried and disproved.

## The one-line status

The engine cross-compiles, boots, loads the retail `.mst`, completes bootup and
runs an unbounded, crash-free frame loop — but **the window is black**. Every
layer of the renderer has been verified working individually. The fault has not
been located.

## Objective

Get Metal Arms (Xbox retail) rendering under MinGW i686 / D3D8, reading the
retail assets directly.

## Environment

- Repo: `/workspaces/ma/orig` (102 commits, HEAD `172de53`), source in `orig/ma`.
- Build dir: `/workspaces/ma/build3`. Build with:
  ```
  cd /workspaces/ma/build3 && . /workspaces/ma/tools/env.sh && make -j4
  ```
- Test package: `/workspaces/ma/ma_win_test.zip` (`ma_win.exe` + `binkw32.dll`).
  Rebuild and re-zip it after every change.
- Retail master file: `/workspaces/ma/xbox/mettlearms_xb.mst` (558,755,840 bytes,
  `FANG` container).
- Target: PE32 / i686, C++11, `-m32 -msse -msse2 -fpermissive`, `_FANGDEF_WINGC`.
- Run logs: `ma_win_log.txt`, `ma_win_asset_log.txt`.

### Graphics environment — important

**Both available test environments are wined3d.** The Linux host runs Wine; the
XP VM runs wined3d-for-Windows. There is no native D3D8 anywhere.

Consequences the next agent should not have to rediscover:

- `0130:fixme:d3d:wined3d_device_apply_stateblock ... RS_PATCHEDGESTYLE/RS_PATCHSEGMENTS`
- `0134:fixme:d3d:wined3d_device_apply_stateblock state_linepattern_w ...`
- `0130:fixme:d3d:resource_check_usage Unhandled usage flags 0x40`
- **`0134:err:d3d:state_undefined Undefined state UNKNOWN_STATE(0) (0)`**

The first three are `fixme` and almost certainly wined3d limitations. The last is
`err`, not `fixme`. It appears on the render thread **exactly when glyphs are first
drawn**, in every single log. It has never been explained. Treat it as the single
most promising unexamined lead.

## Verified working — do not re-investigate

Each of these was established by instrumenting the running game, not by reading
code and assuming.

| Area | Evidence |
|---|---|
| Frame loop | Runs indefinitely, no crash, no assertion dialog spam |
| `Clear` **and** `Present` | Both reach the screen — overriding `fviewport_Clear` to magenta produced a solid magenta window |
| Text queueing | 123 chars/frame across 2 fonts, correctly attributed, re-queued each frame |
| Glyph coordinates | Correct: heading at `X=320.0 Y=36.0` for `fY=0.075` in a 640×480 target |
| `DrawPrimitive` | Issued and returns `D3D_OK` |
| Device viewport | `0,0 640x480 minZ=0.000 maxZ=1.000` |
| Vertex buffer | Created; `nDraw_D3DVBVertexCount` is valid (no warning logged) |

`Clear` being visible while draws are not is the central clue: `Clear` is the one
operation that requires **no vertex format**. That asymmetry should drive the next
investigation.

## Real bugs found and fixed (committed — keep these)

1. **`CFRect2D` aliased all four corner names to one value.** `fmath_geo.h` had
   `union { CFVec2 UpperLeft; CFVec2 LowerRight; CFVec2 m_UpperLeft; CFVec2 m_LowerRight; };`
   with the comment *"Two names for each corner"*. A union aliases all four to the
   **same** corner, so `fdx8viewport` building `UpperLeft=(0,0)` then
   `LowerRight=(640,480)` left `m_UpperLeft` reading `(640,480)`. `ftext` used that
   as the viewport origin and placed every glyph off-screen. Now two distinct
   members; `m_` spellings normalised (11 in `ftext.cpp`, 14 in `fmath_geo.inl`).

2. **`_GameInit` always failed in the `SAS_USER_NONE` build.** The assignment
   `bInitReturnValue = TRUE;` is commented out in the original source in the
   `SAS_USER_NONE` branch of `_GameInit`, so the following
   `if( !bInitReturnValue ) goto _ExitGameInitWithError;` always fired. The Xbox
   build never compiles that branch (it runs with a named `SAS_ACTIVE_USER`), which
   is why this was invisible upstream. `SAS_ACTIVE_USER` is `SAS_USER_NONE` here
   (`compat/sas_user.h`).

3. **Input NULL-deref when no controller is present.** `fpadio_Install` fails with
   `NOTHING_ENUMERATED` on any machine without a pad. `fpad_UpdateSamples` and
   `fpadio_GetSamples` then dereferenced a NULL mutex/sample array, and
   `fpad_Map` left the global `Gamepad_aapSample` pointers NULL, which
   `PROTRACK_WORK` dereferences unconditionally on frame 1. Fixed with early
   returns, `fpad_Map` handing out `_oAlwaysZeroSample`, and `fpad_Install` no
   longer gated on `fpadio`.

4. **Text aspect ratio hardcoded to 4:3.** `_PHYSICAL_DEVICE_ASPECT_RATIO_WoH`
   was `(4.0f/3.0f)`, a CRT assumption. Now derived from
   `FVid_Mode.nPixelsAcross / nPixelsDown`, refreshed per frame, falling back to 1.0.

5. **`w_flamer.csv` schema drift.** `CWeaponFlamer::m_aUserPropVocab` describes a
   newer CSV than retail: it wants 10 numeric columns plus `fSoundRadius`, but the
   retail row is 21 all-string columns. Continuing with zeroed defaults instead of
   aborting, because the failure propagated to `_InitGameSystems()`, which tears
   down every system it already initialised.

## Hypotheses already tried and DISPROVED

Do not spend time on these again. Each was implemented, run, and observed not to
change the result.

1. Pixel shader 63 failing (`_apPShaderFunc[63]` duplicates `[62]`).
2. Back-face culling discarding glyph quads (`D3DCULL_CCW` default).
3. The hardcoded 4:3 aspect ratio. *(Real bug, fixed — but not the cause.)*
4. Transform matrices not being pushed. **Already called** unconditionally at
   `fdx8draw.cpp:1278`, before the XB block. A "fix" adding a second call was
   redundant and removed.
5. `_DrawPrim` being XB-only with no `#else`. **False** — there is a complete
   Windows implementation in the `#else` at `fdx8draw.cpp:1433`. A reimplementation
   was written and later found to be dead code after the `#endif`, then removed.
6. Unbinding the pixel shader in the text path.
7. `D3DRS_ALPHATESTENABLE` left `TRUE` by `fdx8draw_Renderer_Open` with zero
   diffuse alpha rejecting every fragment.
8. A degenerate device viewport. **Already correct.**
9. `DrawPrimitive` failing. **Returns `D3D_OK`.**
10. Missing FVF on the unindexed path. **A real gap, fixed in `172de53`, but the
    window stayed black — so it is not sufficient either.**

## Diagnostic harnesses that were themselves broken

This is the most important section for anyone continuing. The previous agent built
four test triangles, and **three of them were invalid**, then drew conclusions
from them about the engine:

1. Custom vertex buffer created with `FDX8VB_TYPE_C1`: reported **stride 66** and
   **FVF `0x0010` (`D3DFVF_NORMAL`)**, while writing 16-byte XYZ+colour vertices.
   `fdx8vb_Lock` then calls `fdx8sh_SetVertexType()` with the buffer's own shader,
   overriding the FVF it had been given. Result: draw discarded, `D3D_OK` returned.
2. NDC coordinates `[-1.0, 3.0]` submitted to `FViewport_pDefaultOrtho`, which is
   an **orthographic projection over `[0,640]×[0,480]`**, not NDC. Produced a
   sub-pixel speck off-screen.
3. Triangle drawn immediately after `fvid_Begin()`. `_BootupDraw` then calls
   `fviewport_Clear(FVIEWPORT_CLEARFLAG_ALL, ...)` **in the same frame**, erasing it
   before it could ever be presented. The test could not have passed regardless of
   the state it set.

**Lesson:** verify the instrument before trusting it. Verify trace strings are in
the binary with `strings ma_win.exe` — an earlier trace was placed inside an
XB-only block, was compiled out entirely, and its absence was misread as a runtime
fact.

## The measurement that is missing

**Nobody has determined whether pixels are in the back buffer.** That single
question splits the problem cleanly:

- pixels present → renderer works, fault is in `Present`/window
- pixels absent → renderer is not writing, keep bisecting the draw path

D3D8 blocks the obvious route: `IDirect3DDevice8::GetRenderTarget` and
`IDirect3DSurface8::GetRenderTargetData` are **D3D9** APIs, and
`GetSurfaceLevelData` does not exist on `IDirect3DSurface8`. Both were attempted
and failed to compile.

**Suggested workaround:** use `D3DXSaveSurfaceToFileA()` (D3DX8, supported) on the
back buffer immediately after the test draw. The triangle is drawn last in the scene
inside `fvid_End()`, so nothing can clear it before the save. This has not been
tried.

## Unfinished leads

- **`err:d3d:state_undefined UNKNOWN_STATE(0)`** — see above. Never explained.
- **`Present` parameters.** `_SwapBuffers()` (`fdx8vid.cpp:2273`) calls
  `FDX8_pDev->Present( NULL, NULL, NULL, NULL )`. Never examined. Also
  `_PresentationParms`, and the fact that `fvid_CreateWindow` reports
  `adapter 'Default' not found (1 enumerated)`.
- **Shader 63** is a genuine duplicate table entry; fix it properly rather than
  working around it.
- **Asset-version drift:** `DebrisFlam1`/`DebrisSmok1` "not the correct version",
  missing `Debris` damage profile, missing `TFDMLaser02`/`TFDMLaser04`, and
  `Difficulty.csv` expecting 4 levels. These log and continue, but will matter in
  actual gameplay.
- **`w_flamer.csv`** vocab still wants 10 numeric columns retail lacks.
- **Input** is dead under wined3d (no gamepad); keyboard input does not exist.

## Housekeeping left behind

Temporary diagnostics are still in the tree and should be removed once rendering is
confirmed:

- `Lib/Fang2/dx/fdx8draw.cpp` — `_DrawPrim ENTER`, `fdraw_PrimList #`
- `Lib/Fang2/dx/fdx8text.cpp` — `ftext submit WIN`, `diag triangle`
- `Lib/Fang2/ftext.cpp` — `ftext XY`
- `App/ma/win/main_win.cpp` — `first frame:` handler pointers

The magenta clear override in `fviewport_Clear` **is** already removed; only the
`Clear` HRESULT logging remains there.

## A note on approach

The previous agent made ten wrong calls in a row on the black window, each stated
with confidence and each disproved by the next log. The pattern was proposing a
cause before measuring the thing being claimed. It also spent several rounds
reading source instead of instrumenting, and several rounds instrumenting code
that was not compiled on this platform.

If you find yourself about to assert a cause, check whether you have measured
that specific thing. Prefer one narrow measurement over a plausible story.
---

## Pass-2 review (read-only audit of HEAD `e8f844c`)

Written by the second agent after reading the renderer end-to-end and diffing
HEAD against the untouched baseline `8e4fc78`. Nothing below is speculative
unless marked as such; each item says how it was established.

### 1. BLOCKER: `fviewport_Clear()` no longer clears

`Lib/Fang2/dx/fdx8viewport.cpp:281-332` computes `nFlags`, `D3DColor`,
`fDepth`, `nStencil` and then **returns without ever calling
`FDX8_pDev->Clear()`**. The function body just ends after the stencil `if`.

Established by reading the baseline: `git show 8e4fc78:ma/Lib/Fang2/dx/fdx8viewport.cpp`
contains

```c
	if( _pCurrentViewport->pViewportIS->bCoversEntireRenderTarget) {
		FDX8_pDev->Clear( 0, NULL, nFlags, D3DColor, fDepth, nStencil );
	} else {
		... FDX8_pDev->Clear( 1, &Rect, nFlags, D3DColor, fDepth, nStencil );
	}
```

Commit **`c1a62dd`** ("Fix text layout off-screen: derive aspect ratio...", whose
message says *"Also remove all the temporary instrumentation"*) deleted that block
along with the HRESULT logging that had been wrapped around it. `git diff 8e4fc78 HEAD -- .../fdx8viewport.cpp`
is deletions only, no insertions.

Consequences:

- At HEAD nothing clears the colour buffer or the depth buffer.
- The **"Clear and Present | Both reach the screen"** row of *Verified working*
  above is no longer true of the binary you would build today. It was true at
  `cb9e005`/`599225d`, before `c1a62dd`.
- Any test that assumes a cleared frame is measuring leftovers.

This is a definite bug and must be restored first. It is **not** the cause of
the black window: the black window predates `c1a62dd`.

### 2. Windows `_DrawIndexedPrim` never sets a vertex format

`fdx8draw.cpp`: `_DrawIndexedPrim` opens `#if FANG_PLATFORM_XB` at line 985,
where the `FDX8_SetVertexShader(...)` calls live, and its `#else` at line 1112
declares locals and goes straight into the fill loop with **no** FVF call.

This is the same defect class as disproved-hypothesis #10 above, but for the
*indexed* path, which `172de53` did not touch. It will silently discard every
mesh draw once meshes render. Not yet exercised (nothing has drawn a mesh).

### 3. The Windows immediate path never rebinds its stream source

`_FillVertexBufferAndDrawPrim` (fdx8draw.cpp:743) and
`_FillVertexBufferAndDrawIndexedPrim` (:849) lock `_pVB` and call
`FDX8_pDev->DrawPrimitive(...)`, but **neither calls `fdx8vb_Select(_pVB)`**.
They depend on `fdx8draw_Renderer_Open` (:1621) having selected it.

Meanwhile `fdx8psprite.cpp` selects its own VBs at :371, :377, :399, :549, and
`fdx8mesh.cpp` at :568, :835, :1071, :1328, :1723, :1999. `fdx8vb_Select`
(:492) is the only thing that calls `SetStreamSource(0, ...)`, and it early-outs
when `pVB == _pSelectedVB`.

So if any sprite or mesh draw has run since the last `fdraw_Renderer_Open`,
the device reads vertices from the *wrong buffer* while the engine writes into
`_pVB`. On Xbox this path used the push buffer and never needed a stream
source, which is why it was never written.

**This link has never been instrumented.** `SetStreamSource` / `GetStreamSource`
appear nowhere in the diagnostics.

### 4. QUADLIST overruns the vertex lock

`_FillVertexBufferAndDrawPrim` locks `nVtxCount` vertices (:763) but the
QUADLIST branch writes `nVtxCount * 1.5` of them (:766-819, six D3D vertices per
four source vertices), then issues
`DrawPrimitive(D3DPT_TRIANGLELIST, nStartIndex, nVtxCount * 0.5f)`.
Writing past the locked range is undefined. Text uses TRILIST, so this is not
the current symptom, but it is broken.

### 5. The Windows-side `_DrawPrim` diagnostics are dead code

`fdx8draw.cpp:1325-1341` (the `maxPerArray` / `BeginPush failures` counters) and
the `BeginPush FAILED` report at :1354 sit **inside `#if FANG_PLATFORM_XB`**
(opened at :1293, `#else` at :1433). Verified with
`strings build3/ma_win.exe | grep maxPerArray` -> 0 hits, while
`_DrawPrim ENTER` (outside the block) is present.

So: nobody has ever counted BeginPush failures on Windows, and the handoff's
implication to the contrary is wrong. `_DrawPrim ENTER` and `fdraw_PrimList #`
are the only live traces.

### 6. `ma_win_log.txt` is stale

It contains `[ma] diag triangle submitted via fdraw_PrimList on frame %u`, a
string that exists in neither the source nor the shipped exe (which prints
`[ma] diag triangle drawn last in scene, frame %u`). The log predates the
current build. Do not draw conclusions from it; re-run and re-capture.

### 7. `state_undefined UNKNOWN_STATE(0)` - probably a red herring

Searching the exact message finds an unrelated game under Wine logging the
*same* trio, including the sibling fixme:

```
0124:err:d3d:state_undefined Undefined state UNKNOWN_STATE(0) (0).
0124:fixme:d3d:state_linepattern_w Setting line patterns is not supported...
```

That is the same `state_linepattern_w` fixme listed in *Graphics environment*
above, for software that never touches this engine. It reads like wined3d
applying an internal state block and hitting table slot 0.

Cheap to dispose of one way or the other (run once with `WINEDEBUG=+d3d`, or
check whether it fires before the first draw), but it should not be worked on
before the backbuffer measurement in §9.

### 8. The diagnostic triangle's colour path is actually correct

Checked in case the instrument was invalid again:
`frenderer_Push(FRENDERER_DRAW, NULL)` -> `frenderer_SetDefaultState()` ->
`FDRAW_DEFAULT_COLORFUNC == FDRAW_COLORFUNC_DECAL_AI` (fdraw.h:338) ->
`_aMap_ColorFunc_COLOROP[DECAL_AI] = D3DTOP_SELECTARG1` and
`_aMap_ColorFunc_COLORARG1[DECAL_AI] = D3DTA_DIFFUSE` (fdx8draw.cpp:113-142),
texture bound to NULL. So the triangle selects diffuse = magenta. Colour-op is
not the flaw.

Side effect worth knowing: under `DECAL_AI`, `_DrawPrim:1448` sets
`FVF = D3DFVF_XYZ | D3DFVF_DIFFUSE` (16 bytes) while the stream stride is 24
(`FDX8VB_TYPE_C1T1`). Benign - wined3d takes the stride from `SetStreamSource`
and pos (0..11) / diffuse (12..15) still line up - but it means the triangle and
the text draw are *not* exercising the same vertex format.

### 9. Measurement plan for this pass

Run these one at a time; do not batch them, and do not propose a cause ahead of
the result.

1. **Restore the `Clear()` call** in `fviewport_Clear` (§1). Verify by
   re-running the magenta-override test that used to prove Clear works.
2. **Backbuffer dump.** `D3DXSaveSurfaceToFileA()` on the back buffer
   immediately after the diagnostic triangle in `fvid_End()` (after
   `ftext_Draw()`, before `EndScene()`), so nothing can clear it first. This is
   the measurement the previous pass identified as missing and never tried.
   - pixels present -> renderer writes, fault is in Present/window
   - pixels absent -> keep bisecting the draw path
3. **If absent:** immediately before `DrawPrimitive`, log `GetVertexShader()`,
   `GetStreamSource(&pVB, &offset, &stride)`, and read back the first 24 bytes
   of the locked region after `Unlock()`. This settles FVF-vs-stream-source
   (§2, §3) in one run.
4. **Same run, capture Wine stderr** with `WINEDEBUG=+d3d` so the engine trace
   and the wined3d call stream correlate line by line. Also answers §7.

Rebuild and re-zip `ma_win_test.zip` after every change, and confirm new trace
strings are actually in the binary (`strings ma_win.exe | grep ...`) before
trusting a silence.

---

## Pass-2 work log (updates as work proceeds)

### Done in this pass

**§1 fixed — the `Clear()` call is restored.** `fviewport_Clear`
(fdx8viewport.cpp) now issues `FDX8_pDev->Clear(...)` again, with the
full-viewport vs. per-rect split exactly as in baseline `8e4fc78`. Builds clean.

**§9.2 implemented — backbuffer snapshot.** New static
`_DiagDumpBackBuffer(const char *pTag, u32 nFrame)` in
`Lib/Fang2/dx/fdx8vid.cpp`, just above `fvid_End`:

- `GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO)` — the *presentation* buffer, not
  the current render target, so a render-to-texture viewport cannot make it
  pass while `Present` shows nothing.
- `LockRect(D3DLOCK_READONLY)`, scans every pixel, writes a binary PPM.
- Logs one line: format, pitch, `nonblack=n/total`, `magenta=n`, probe pixel at
  (8,8) and at the centre, and the full path written.
- Files go **next to the executable** (`GetModuleFileName`), same convention as
  `ma_win_log.txt`, not the working directory.
- Called from `fvid_End()` **after `EndScene()`**, before `fvid_Swap()`/
  `_SwapBuffers()`, on frames `{1,2,3,8,16,32,64,128}` of `FVid_nFrameCounter`.
  (Counter starts at 1 and is incremented at the top of `fvid_Begin`, so the
  first sample is frame 2.) Expected: 7 `ma_bb_end_XXXX.ppm` files.

How to read the result:

| observation | conclusion |
|---|---|
| `magenta > 0` | draw path works end-to-end; fault is in `Present`/window |
| `nonblack > 0` but `magenta == 0` | something draws, but not the last thing in the scene |
| `nonblack == 0` everywhere | `DrawPrimitive` is a no-op; bisect FVF / stream source / VB contents (§2, §3) |

### Trap caught before shipping

`FANG_PLATFORM_PC` **does not exist in this codebase** — the platform macros are
`FANG_PLATFORM_WIN`, `FANG_PLATFORM_XB`, `FANG_PLATFORM_DX`, etc. (fang.h:123).
Guarding new Windows-only code with `FANG_PLATFORM_PC` compiles it out silently
and *without* an unused-function warning, because both the definition and the
call site vanish together. It was caught only because
`strings ma_win.exe | grep bbdump` returned 0.

**Check every new diagnostic with `strings` after every build.** This is exactly
the failure mode §5 documents for the `_DrawPrim` counters.

### Packaging

`ma_win_test.zip` is rebuilt in place with Python's `zipfile` — there is no
`zip` binary in this container. The container has **no Wine and cannot run the
exe** (tools/README.md:58); runs happen on the user's laptop, then results come
back as `ma_win_log.txt` plus the `ma_bb_*.ppm` files.

### Measurement 1 result — Clear lands, draws do not

Run of the package built at 16:55-equivalent (zip crc `24c7d3e3`), user-reported
"a few seconds on a black window, then closed". Seven dumps written:
`ma_bb_end_{0002,0003,0008,0016,0032,0064,0128}.ppm`.

Every dump: **640x480, X8R8G8B8, pitch 2560, exactly one distinct colour across
all 307200 pixels — `0x000007` (BGR bytes `00 00 07`)**, and `magenta=0`.

Two conclusions, both hard:

1. **The restored `Clear` is live and working.** `0x000007` is precisely
   `App/ma/launcher.cpp:530/808/852` calling
   `fviewport_Clear(FVIEWPORT_CLEARFLAG_ALL, 0.0f, 0.0f, 0.03f, 1.0f, 0)`:
   `(u32)(0.03 * 255) = 7`. Nothing else produces that number. The window looks
   black because the back buffer genuinely *is* this uniform fill.
2. **Zero pixels from any draw ever reach the buffer** — not the magenta
   triangle, not the 123 glyphs/frame of text, nothing. Since the fill is
   uniform, even a draw writing plain black would have shown up as a second
   distinct colour. So this is **not** a "draws black" failure: the primitive is
   being discarded or clipped *before* it writes, or is written somewhere that
   is not this buffer.

Note on the earlier metric: `nonblack=307200/307200` (100%) is meaningless here
because the clear colour is not black. **Count distinct colours, not non-black
pixels.**

This closes §9 item 2. Item 3 (FVF / stream source / transforms / render states /
`DrawPrimitive` HRESULT) is what `_DiagBeforeDraw` ships next; the zip crc after
that build is `2f74a0cf`.

Remaining explanations the shipped readback discriminates, in order of how cheap
they are to falsify: bad FVF or stream source (`stream0=NONE`, `FVF=0`), an
all-zero view/projection (everything clipped), a write-killing render state
(`ZVISIBLE=1`, `COLORWRITEENABLE=0`, `CLIPPING=0`), alpha test with `ALPHAREF`
above the vertex alpha, the fill not landing in the VB (`vtx0` empty/garbage),
or a failing `DrawPrimitive`. If all of those come back sane, the last
possibility is that the draw targets something other than the presented buffer
(`GetRenderTarget` vs `GetBackBuffer`).
