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