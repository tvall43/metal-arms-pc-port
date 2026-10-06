# Metal Arms Windows port — handoff

Written by the previous agent, which ran out of useful ideas rather than out of
builds. Read the whole thing before changing anything; most of it is a list of
things that have already been tried and disproved.

## The one-line status

The engine cross-compiles, boots, loads the retail `.mst`, completes bootup and
runs an unbounded, crash-free frame loop — and **it now renders**. The "Choose
Bootup Mode" menu draws with correct text, correct layout and a moving selection
highlight. The black-window bug is fixed (Measurement 8). What is left is that
**the game cannot be controlled** — see Measurement 9.

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
- **Input is dead — diagnosed, not yet fixed (Measurement 9).** `fpadio`
  enumerates **DirectInput game controllers only**; under Wine there are none, so
  `fpadio_Install` fails with `Error 418: Nothing has been enumerated` and
  `fpad_UpdateSamples()` returns 0 every frame. `Gamepad_aapSample[][]` therefore
  reads all zeros forever. There is **no keyboard path in the game at all** — it
  is a console title. The fix is a synthetic keyboard-fed device inside
  `fdx8padio.cpp` so `fpadio_Install` succeeds.

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

---

## Measurement 5 — `WINEDEBUG` trace (done) → **the transforms are the bug**

The game runs under Wine (`Z:\home\tim\games\...`), so wined3d's own trace is
available. The stderr redirect the earlier note asked for *was* the missing
piece: `WINEDEBUG` only writes to the console, never into `ma_win_log.txt`.

    cd ~/games/ma
    WINEDEBUG=+d3d wine ma_win.exe 2> log.txt     # ~29 MB, ~287k lines; kill after a few seconds

Result is in `outputs/log.txt` (`backtrace.txt` there is **stale**, ignore it).

### What the trace proves is *working*

| evidence | count | meaning |
|---|---|---|
| `wined3d_device_context_draw` / `draw_primitive … Draw completed.` | 1217 / 1216 | wined3d really executes the draws |
| `swapchain_gl_present` + `swapchain_blit (0,0)-(640,480)` | 226 | `Present` runs and blits the offscreen backbuffer to the window every frame |
| `wined3d_device_clear … color 2.74509806e-02` (=7/255) | per frame | the clear reaches texture `05A60240`, i.e. the same resource `GetBackBuffer` returns |
| `context_gl_check_fbo_status FBO complete.` | 1896 | the framebuffer the draws render into is complete |
| `device_context_set_viewports 0,0 640x480 z=0..1` | 3 | viewport is correct |
| `wined3d_texture_load_location … WINED3D_LOCATION_SYSMEM` | 42 | exactly our 42 probes — so each probe **downloads GL contents**; the probe is trustworthy (it saw 0x000000 → 0x000007 after the first clear) |

So: draws execute, into a complete FBO on the presented backbuffer, and our
readback of that backbuffer is genuine. Something upstream makes the draws
degenerate.

### THE FINDING — VIEW and WORLD arrive degenerate

`wined3d_stateblock_set_transform` prints the full 4x4. Across the whole run:

    VIEW    (492 samples, every one identical)
      1 0 0 0
      0 0 0 0      <-- row 2 zero
      0 0 0 0      <-- row 3 zero
      0 0 0 1
    WORLD   (488 samples) — identical structure
    PROJECTION — sane, three variants:
      ortho   diag(0.003125, -0.004167, 0.001, 1) t=(-1, 1, -0.0001)   [HUD 640x480]
      ortho   diag(0.003125, -0.004167, 0.526, 1) t=(-1, 1, -0.053)    [diag triangle]
      persp   diag(1, 1.3333, 1.001, 0) with m43=1, m44=-1.001

Row-vector maths: for `v' = v * VIEW` this matrix forces **y' = 0 and z' = 0 for
every vertex**. Feeding that into the sane ortho projection puts *every* vertex
at NDC y = +1 — all triangles collapse onto one horizontal line, area 0, nothing
rasterizes. With the perspective projection w' comes out 0 and the vertices are
fully clipped instead. Either way: `DrawPrimitive` returns `D3D_OK`, wined3d logs
`Draw completed.`, and not one pixel changes. That is the entire black screen.

**We already had this data.** `_DiagBeforeDraw` printed
`view=1.000/0.000/0.000/1.000` on every draw — that is the **diagonal**
`(_11,_22,_33,_44)`, so `_22=0` and `_33=0` was on screen the whole time. It was
read as "identity"; identity would print `1/1/1/1`.

### Where the bad matrix comes from (next run answers this)

`FDX8_SetViewMatrix()` (`fdx8.h:225`) copies `FXfm_pView->m_MtxF` row by row into
the D3D matrix — rows are `Right / Up / Front / Pos`, since `CFMtx43A`'s union is
`f32 aa[4][4]` (`fmath_mtx.h:366`). The D3D matrix we measured implies the Fang
view matrix is `Right=(1,0,0)` with **`Up`, `Front`, `Pos` all zero**. WORLD comes
from `FXfm_pModel->m_MtxF` the same way (`_ConvertXfmToD3DMtx`).

So either the Fang transforms really are degenerate (ftransform/camera never
built — but `Identity()` would give full identity, not this), or the
conversion/struct layout is wrong. The build in the zip prints, on the first 24
draws:

* `viewXF r=… u=… f=… p=…` and `modelXF …` — the 12 floats of both Fang matrices
* `viewFULL=` — all 16 floats of the D3D VIEW straight from `GetTransform`
* `sizes CFVec3A=… CFMtx43A=… CFMtx44=… CFMtx44A=… D3DMATRIX=… CFXfm=…` — if
  `CFVec3A` is not 16 bytes or `CFMtx43A` not 64, the row mapping is misaligned

Also in this build: `Present hr=0x%08X frame N` plus `probe present`. Note the
previous `present` probe had been added **inside the Xbox `#else` branch of
`_SwapBuffers`**, so it never compiled on Windows and its absence proved
nothing — Windows calls `Present` at `fdx8vid.cpp:2622`.

### Other things the trace surfaced (not yet acted on)

* `wined3d_resource_check_box_dimensions Box (off,0,0)-(off+size,0,0) is invalid`
  + `Map box is invalid` — **1260 times**, on index-buffer locks. For a buffer
  wined3d wants `(off,0,0)-(off+size,1,1)` (compare the healthy
  `upload_bo … box (256,0,0)-(320,1,1)`). Engine index-buffer content may be
  garbage; worth checking once the transforms are fixed.
* `sanitise_map_flags DISCARD or NOOVERWRITE map not on dynamic GPU-accessible
  buffer, ignoring.` x5 — `fdx8vb_Lock` passes `D3DLOCK_DISCARD` on dynamic VBs.
* `wined3d_texture_load_location Operation requires 0x2 access, but texture only
  has 0xd.` x20.
* `state_undefined Undefined state UNKNOWN_STATE(0) (0).` x1.

**Zip exe crc32 `c7f9c9b6`.**

---

## Measurement 6 — struct layout is fine, the *Fang* transforms are the bad ones

New log (`sizes`/`viewXF`/`modelXF`/`viewFULL`/`Present hr=`) answers the
question Measurement 5 left open:

    sizes CFVec3A=16 CFMtx43A=64 CFMtx44=64 CFMtx44A=64 D3DMATRIX=64 CFXfm=136
    viewXF  r=1,0,0  u=0,0,0  f=0,0,0  p=0,0,0      <-- FXfm_pView->m_MtxF
    modelXF r=1,0,0  u=0,0,0  f=0,0,0  p=0,0,0      <-- FXfm_pModel->m_MtxF
    viewFULL 1/0/0/0 0/0/0/0 0/0/0/0 0/0/0/1        <-- GetTransform(D3DTS_VIEW)
    Present   hr=0x00000000

* **Layout is correct** — `CFVec3A` is 16, `CFMtx43A` is 64, `aa[4][4]` lines up
  with the four `CFVec3A` rows. So it is *not* a struct/alignment mismatch.
* **The conversion is faithful** — `viewFULL` is exactly `_ConvertXfmToD3DMtx`'s
  output of `m_MtxF` (it forces `_44=1`), so the corruption is already present in
  the Fang matrix.
* **`Present` succeeds** (`hr=0x00000000`) — finally confirmed, and `probe present`
  now runs (the previous probe had been inside the Xbox `#else` branch).

### Why this must come from `CFMtx43A::Identity()`

`_ViewXfm` is written in exactly two places (`fxfm.cpp:111` and `:114`), and
`CFXfm::InitStack()` is only ever called **with the default NULL** (`fxfm.cpp:93`,
`fdx8xfm.cpp:428`). `InitStackWithView()` — the only caller that passes a view —
is declared inline at `fxfm.h:143` but is called solely from `gc/*.cpp`, and
**zero `gc/*.obj` are in the link** (confirmed against `link.txt`). So `_ViewXfm`
can only ever be `Identity()`-ed.

Yet it reads back as `r=(1,0,0)` with Up/Front/Pos all zero. `_pModelStack[0]`
is likewise `Identity()`-ed (`fxfm.cpp:104`) and reads back identically.
`fxfm_SetViewAndWorldSpaceModelMatrices()` on Windows (`fdx8xfm.cpp:242`) only
sets the *world* matrix to `m_IdentityMtx` and never touches VIEW.

Every candidate on paper is correct:

| source | result |
|---|---|
| `CFMtx43A::m_IdentityMtx` init (`fdx8math_mtx.cpp:444`) | real identity, and **outside** both `#if !FANG_WINGC` blocks (247→430, 613→852), so it *is* compiled |
| `Identity()` → `Set(m_IdentityMtx)` (`fdx8gcmath_mtx.inl:343`, `:375`) | copies `m_vX/m_vY/m_vZ/m_vP` row-wise |
| `Identity33()` (`fdx8gcmath_mtx.inl:348`) | assigns `m_UnitAxisX/Y/Z` |
| `CFVec3A` 3-float ctor / `Set(x,y,z)` (`fdx8gcmath_vec.inl:452`, `:470`) | stores all three |
| `CFVec3A::m_UnitAxisX/Y/Z` (`fdx8math_vec.cpp:62-64`) | defined, no preprocessor guards |

Note the live config: **`-D_FANGDEF_WINGC` → `FANG_WINGC=1`**, so the whole
SSE/MSVC-`__asm` body of `fdx8math_mtx.inl` (lines 526→2526) is compiled *out*
and `fdx8gcmath_mtx.inl` supplies the math instead — that is the implementation
actually running.

So either the static reads wrong at runtime or `Set()` mis-copies — both
undecidable from reading. **Next build measures directly** (`_DiagBeforeDraw`,
first draw only):

* `mtxDefault` / `mtxIdentity` — a fresh `CFMtx43A`, then `Identity()`-ed
* `mtxStatic` — `CFMtx43A::m_IdentityMtx` itself
* `unitAxes` — `CFVec3A::m_UnitAxisX/Y/Z`
* `viewFull16` / `modelFull16` — all 16 floats of both live matrices (including
  the `[3][3]` we could not see before)
* `viewR` / `modelR` + `scaleF`/`scaleR` — the inverse halves and scales

If `mtxIdentity` is a proper identity → the static and `Set()` are fine and the
live objects are overwritten by a writer we have not found. If `mtxIdentity`
comes back degenerate → `Identity()`/`Set()`/the static is broken and we fix it
there.

**Zip exe crc32 `a4f03465`.**

---

## Measurement 7 — `Identity()` proven broken; the port is the suspect

Log (`mtxStatic`/`mtxIdentity`/`unitAxes`):

    mtxStatic   r=1,0,0 u=0,1,0 f=0,0,1 p=0,0,0   <-- source is a perfect identity
    mtxIdentity r=1,0,0 u=0,0,0 f=0,0,0 p=0,0,0   <-- T.Identity() mangles it
    unitAxes    x=1,0,0 y=0,1,0 z=0,0,1           <-- the statics are fine too

So the corruption is inside `CFMtx43A::Identity()` -> `Set(m_IdentityMtx)` ->
`m_vX/Y/Z/P.Set(rM.m_vX...)` -> `v3 = rV.v3` -> `CFVec3::operator=`, because a
fresh temporary comes back wrong while the object it copies *from* comes back
right. That also settles why **both** the view and the model matrix are wrong:
both are `Identity()`-ed (`fxfm.cpp:114` and `:104`).

### Context: this is very likely a porting bug

The GCC/MinGW port is one of the earliest things done here, and two of its
commits touch exactly this code path:

* **`6e7f3a9` "Port remaining Fang2 math to GCC: C++11 unions"** —
  - switched the build to `CMAKE_CXX_STANDARD 11` *specifically because*
    "the vector/quaternion types are type-punning unions whose members have
    non-trivial constructors... GCC only accepts those from -std=c++11
    ('unrestricted unions')";
  - `cmake/fix_union_anon_struct.py` unwrapped `struct { T n; };` to `T n;`
    inside those unions (layout verified unchanged — offsets are identical).

  Under MSVC (the compiler `win/` was written for) reading a union member other
  than the active one just works. In C++11 that read is undefined unless that
  member is active, and `-O2`/`-fstrict-aliasing` is free to conclude the
  `CFVec3A`/`CFVec3` subobject was never written — which is precisely a copy
  that silently keeps only some of its values.

* **`05c0b7f` "Complete the _FANGDEF_WINGC configuration"** — this is what made
  `FANG_WINGC=1` buildable at all (15 members lived inside `#if !FANG_WINGC`
  and could not link). It added `dx/fdx8wingc_missing.inl`, included last from
  `fmath.h`. Checked: it does **not** shadow `CFMtx43A::Set`/`Identity`
  (it only adds `RevMul`, `CFVec3A::CrossVecWithY`-family and `CFQuatA::Set`).

`git log` shows `fdx8gcmath_*.inl`, `fdx8math_*.inl`, `fmath_vec.h`,
`fmath_mtx.h` are otherwise untouched since baseline `8e4fc78` apart from the
MSVC-hex-literal rewrites (`52dd5b2`, `4032872`, asm-only) — so the original
source of those files is intact.

### What this build measures

`_DiagBeforeDraw`, first draw only, all one-time:

| line | question it answers |
|---|---|
| `mtxStatic` | re-confirm the source object (via `aa[][]`) |
| `srcPun` | **read the same static through `m_vX/m_vY/m_vZ/m_vP.v3`** — if this disagrees with `mtxStatic`, the union type-punning is the bug and it is a porting bug |
| `sIdent` | `Identity()` into a `static` object (rules out stack/varargs clobber of the earlier `T`) |
| `oneRow` | one `m_vY.Set()` on a zeroed matrix, everything else observed |
| `rawCopy` | `a[]` -> `a[]` element-wise, bypassing every operator |
| `unitAxes` | re-confirm the vector statics |

Reading: `srcPun` wrong => union/aliasing => fix the port (e.g. copy through
`a[]`, or give the unions a plain `float[16]` path). `srcPun` right but
`oneRow` wrong => `CFVec3A::Set`/`CFVec3::operator=` miscompiled.
`rawCopy` right and everything else right => the earlier `T` was clobbered.

**Zip exe crc32 `80086479`.**

---

## Measurement 8 — **ROOT CAUSE FOUND AND FIXED**: the port flattened multi-member anonymous structs

### The measurement

First draw, one-time lines (from the log this pass shipped):

    mtxStatic  r=1,0,0 u=0,1,0 f=0,0,1 p=0,0,0     <-- read via aa[][]: perfect identity
    srcPun     x=1,0,0 y=1,0,0 z=1,0,0 p=1,0,0     <-- same object via m_vX/m_vY/m_vZ/m_vP.v3
    oneRow     r0=1,0,0 r1=0,0,0 ...               <-- one sU.m_vY.Set() landed in r0
    rawCopy    correct                              <-- a[] -> a[] element-wise is fine
    unitAxes   x=1,0,0 y=0,1,0 z=0,0,1             <-- the vector statics are fine

`mtxStatic` (through `aa[][]`) is a **perfect identity** while `srcPun` (through
`m_vX/m_vY/m_vZ/m_vP`) reads `x == y == z == p` — i.e. **all four row members
resolve to the same address**. `oneRow` confirms it in the other direction: a
write to `m_vY` shows up in row 0.

That is not type-punning or aliasing. Measurement 7's "strict aliasing" guess
was wrong. It is **member layout**.

### The cause

Commit **`6e7f3a9` "Port remaining Fang2 math to GCC: C++11 unions"** ran
`cmake/fix_union_anon_struct.py` over `fmath_mtx.h` and friends. Its job was to
unwrap `struct { T n; };` to `T n;` — a single-member unwrap, which **is**
layout-neutral. But it also unwrapped the **multi-member** groups:

    -  struct { CFVec3A m_vX, m_vY, m_vZ, m_vP; };
    -  struct { CFVec3A m_vRight, m_vUp, m_vFront, m_vPos; };
    +  CFVec3A m_vX, m_vY, m_vZ, m_vP;
    +  CFVec3A m_vRight, m_vUp, m_vFront, m_vPos;

Those become four *independent union members*, all at **offset 0**. The struct
was the only thing giving them sequential addresses. With every row aliasing
row 0:

* `Identity()` / `Set()` copy row 0 into all four rows, and the source's rows
  1-3 are zeroes, so the destination collapses to `[1,0,0,0 / 0,0,0,0 /
  0,0,0,0 / 0,0,0,1]` — exactly the `WINED3D_TS_VIEW` /
  `WINED3D_TS_WORLD_MATRIX(0)` value seen 980 times in the `WINEDEBUG=+d3d`
  trace;
* `y' = z' = 0` for every vertex, every triangle has zero area, and nothing
  rasterises. Everything else (clear, present, FBO, viewport, scissor, 1217
  draws) was already proven correct — which is why the window was black with
  no error anywhere.

Sites broken (all in `fmath_mtx.h`): `CFMtx33`, `CFMtx43`, `CFMtx44`,
`CFMtx44A`, `CFMtx43A` — 10 structs. Plus `CFTQuatA`
(`struct { CFVec3 m_Pos; f32 m_fScale; };` → `m_fScale` moved from +12 to +0,
breaking `CFAnimFrame`) and `fmath_vec.h`'s two `struct { __vec2x32float__ _A,
_B; };` (GC-only).

`CFMtx44`'s *single*-member groups (`m33`, `m44`, `m44a`) were **not** broken by
the flatten — a lone member genuinely sits at offset 0 — so they stay unwrapped.

### Why the obvious repairs do not work

Verified against the real toolchain (`i686-w64-mingw32-g++ -std=gnu++11`):

| approach | result |
|---|---|
| restore `struct { CFVec3 m_vX, m_vY, m_vZ; };` | **rejected**: `member 'CFVec3' with constructor not allowed in anonymous aggregate` |
| same + `-fms-extensions` | **rejected** — this is not an MSVC-compat gap |
| same + `-fpermissive` | **rejected** |
| member with only *implicit* special members in an anon struct | **accepted** |
| anon struct holding an **array** of ctor-typed elements | **accepted** |
| a *direct* union member with a ctor | accepted (this is what the flatten does) |
| rename ~10 000 `m_vX`/`m_vPos` call sites to `m_vRows[i]` | rejected — `m_vPos` alone appears 3518 times, and non-matrix classes own fields with those names |

GCC's restriction is fundamental here: `CFVec3`/`CFVec3A`/`CFVec4`/`CFVec4A`
have user-declared constructors and copy assignment, so they may not appear in
an anonymous struct — but they *may* appear as an array inside one.

### The fix

`Lib/Fang2/fmath_mtx.h` — each matrix stores its rows as **one array member**
inside the anonymous struct, which GCC accepts and which lays out at
0/16/32/48 exactly as the original four-member struct did:

    union {
        struct { CFVec3A m_vRows[4]; };   // was: m_vX, m_vY, m_vZ, m_vP
        CFMtx44 m44;                      // single members stay unwrapped
        CFMtx44A m44a;
        f32 a[16];
        f32 aa[4][4];
    };

and eight `#define`s near the top of the same header restore the original
names verbatim, so no call site changes:

    #define m_vX m_vRows[0]   ...   #define m_vPos m_vRows[3]

They must be defined **before** the class bodies because `ChangeEndian()` uses
those names, and they are visible to `fmath_quat.h`, `fmath_geo.h` and every
`.inl` because of `fmath.h`'s include order (`fmath_vec` 297 → `fmath_mtx` 298
→ `fmath_quat` 299 → `fmath_geo` 300 → `.inl` 330+).

`Lib/Fang2/fmath_quat.h` — `CFTQuatA` keeps `m_Pos` as a direct union member
(offset 0, legal) and moves `m_fScale` to +12 with a POD tail:

    union {
        CFVec4A m_PosScale;
        CFVec3  m_Pos;
        struct { f32 _posPad[3]; f32 m_fScale; };
    };

Because `m_vPos` is now a macro, the two **non-matrix** fields that shared the
name had to be renamed, or they would silently have become `CFVec3 m_vRows[3]`
and still compiled:

* `App/ma/CamCutscene.h` — `CFVec3 m_vPos` → `m_vCutPos`
* `App/ma/Splat.h` — `CFVec3 m_vPos` → `m_vSplatPos`

### Verification (all passed)

* **Full build: 0 errors**, `Built target ma_win`.
* **Compile-time layout test** (`static_assert` / `__builtin_offsetof` with the
  project's own flags) — exit 0:
  `CFMtx33=36 CFMtx43=48 CFMtx44=64 CFMtx44A=64 CFMtx43A=64`,
  `m_vRows@0` in all five, `a`/`aa`/`m44`/`m44a`/`m33` all `@0`,
  `CFTQuatA::m_fScale == m_PosScale + 12`, `m_Pos == m_PosScale`.
* `grep m_vRows` outside `fmath_mtx.h` → **empty**, so the macro silently
  converted no other field.
* No multi-member anonymous struct with a ctor-bearing member remains anywhere
  under `ma/`.
* `strings -a ma_win.exe` confirms the new probe:
  `[ma] mtxLayout sizeof=%u m_vX@%u m_vY@%u m_vZ@%u m_vP@%u m44@%u aa@%u (want 64 / 0,16,32,48 / 0,0)`

### What the next run should show

`mtxLayout` must print `m_vX@0 m_vY@16 m_vZ@32 m_vP@48`. `srcPun` must equal
`mtxStatic` (`y=0,1,0 z=0,0,1`), `oneRow` must show the write in row 1, and
`viewXF`/`viewFULL`/`modelFull16` must become real identities instead of
`[1,0,0,0 / 0,0,0,0 / 0,0,0,0 / 0,0,0,1]`. With a non-degenerate world
transform the 1217 draws should finally produce non-clear pixels — check the
backbuffer dumps on frames `{1,2,3,8,16,32,64,128}`.

No `WINEDEBUG` needed for this run; the plain log is enough.

**Zip exe crc32 `67d66a6a`, archive crc32 `3bcc7979`.**

## Measurement 9 — **it renders**; the magenta was our own probe, input is the remaining fault

Build after Measurement 8. Two facts, both from evidence:

### 1. The matrix fix worked

From `ma_win_log.txt`:

```
[ma] mtxLayout sizeof=64 m_vX@0 m_vY@16 m_vZ@32 m_vP@48 m44@0 aa@0
```

exactly the wanted `64 / 0,16,32,48 / 0,0`. `srcPun` equals `mtxStatic`
(`y=0,1,0 z=0,0,1`), `oneRow` shows the write landing in row 1, and
`viewFull16` / `modelFull16` are identities. The window is no longer black: the
"Choose Bootup Mode" menu from `App/ma/launcher.cpp` draws with correct text,
the orange selection highlight on "Development Mode", the separator rule and
`Ver 1.19.1`.

### 2. The magenta triangle was ours, not a fault

The half-screen magenta came from the TEMPORARY DIAGNOSTIC block in
`Lib/Fang2/dx/fdx8vid.cpp` (~line 1117): a fixed `(0,0) (0,480) (640,0)` triangle
in `FFFF00FF`, logged as `[ma] diag triangle drawn last in scene`. It is drawn
*after* `ftext_Draw()` and before `EndScene()`, which is why only the right half
of the menu was visible. Measured: `probe tri magenta=2460` and
`bbdump magenta=154080` of 307200 pixels = **50.2%** of the frame — exactly the
lower-left half of the 640x480 screen.

Removed: the block plus its `_DiagProbe( "tri" )` call — **46 lines, pure
deletion, zero bytes altered elsewhere**. `if( _pFcnDrawOverlay )` was briefly
lost during editing and is restored (verified at `fdx8vid.cpp:1119`).

> **Careful with `fdx8vid.cpp`:** it is **mixed line ending** at HEAD — 429 CRLF
> and 3023 bare-LF lines. A blind "normalize to CRLF" pass rewrites 3023 lines.
> Edit it byte-precisely, or check `git diff` for a wall of whitespace changes.

Verified on the built exe with `strings -a ma_win.exe`:
`diag triangle` **0**, `_nDiagFrames` **0**, `mtxLayout` **1**, `postovl` **1**,
`endscene` **1**, `nonblack=` **1**.

### 3. Input: diagnosed, NOT yet fixed

The log says it outright:

```
[ FPADIO ] Error 418: Nothing has been enumerated !!!
Could not install input driver (fpadio); continuing without a controller.
[ma] input driver (fpad) installed.
```

Only the **fpad** line has the `[ma]` prefix, so a grep for `[ma]` misses the
failure — grep for `input` instead.

The input stack is `fdx8padio.cpp` (DirectInput) -> `fpad.cpp` -> `gamepad.cpp`
-> the global `Gamepad_aapSample[][]` the game reads:

* `fdx8padio.cpp:418` — `_pDInput->EnumDevices( DI8DEVCLASS_GAMECTRL, ...,
  DIEDFL_ATTACHEDONLY )` returns `uCount == 0` under Wine. The function bails
  out with `FPADIO_ERROR`, so `fpadio_IsInstalled()` stays FALSE.
* `fpad.cpp:191` — `fpad_UpdateSamples()` starts with
  `if( !_bModuleInstalled || !fpadio_IsInstalled() ) return 0;` — so **nothing is
  ever sampled**.
* `fpad.cpp:~140` `fpad_Map()` still runs (that was fixed earlier) and points
  every action at `_oAlwaysZeroSample`, so reads succeed and report "nothing
  pressed" rather than crashing. Correct, but therefore permanently inert.

This is a console title — **there is no keyboard code path anywhere in the
game**. `launcher.cpp:506-520` needs `GAMEPAD_MENU_LEFT_ANALOG_Y` or
`GAMEPAD_MENU_DPAD_Y` carrying
`FPAD_LATCH_TURNED_ON_WITH_REPEAT_AND_WITH_INITIAL_DELAY`, and
`launcher.cpp:431` needs `GAMEPAD_MENU_ACCEPT` with
`GAMEPAD_BUTTON_1ST_PRESS_MASK`. All of those come from `_aoSamples`, which come
from `fpadio_GetSamples()`.

**The fix:** make `fpadio_Install` succeed when no controller enumerates, by
registering a synthetic keyboard-driven device whose samples are filled from
key state each poll. Then `fpad_UpdateSamples()` runs normally and
`gamepad_SetMapping( i, GAMEPAD_MAP_MENU )` does the rest. Minimum useful
mapping: arrows/WASD -> stick or dpad Y, Enter/Space -> `GAMEPAD_MENU_ACCEPT`,
Esc -> `GAMEPAD_MENU_BACK`.

**Zip rebuilt:** archive crc32 `f1612b75`, `ma_win.exe` crc32 `f30bc0ca`,
`binkw32.dll` crc32 `55da02b7` (23,800,643 bytes, members `ma_win.exe` +
`binkw32.dll`). Previous build was exe `67d66a6a` / archive `3bcc7979`.
