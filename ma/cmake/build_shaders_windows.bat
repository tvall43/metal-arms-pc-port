@echo off
REM Delayed expansion is required: :one reads !ASM!, !LINE! etc. at execution
REM time, and the counters are updated inside a subroutine.
setlocal EnableDelayedExpansion
REM ===========================================================================
REM  build_all.bat -- assemble the Metal Arms Direct3D8 shaders.
REM
REM  The engine #includes 238 pre-assembled shader headers plus one point-sprite
REM  shader blob (win\fdxsh_psprite.cvs). None of them are in the source tree:
REM  they were produced at build time by Microsoft's DirectX 8 shader
REM  assemblers, vsa.exe (vertex) and psa.exe (pixel), both bundled here.
REM
REM  Run this on Windows. Everything lands in .\out. Copy the contents of
REM  .\out back over ma\Lib\Fang2\dx\ in the source tree.
REM
REM  Usage:  build_all.bat
REM
REM  manifest.txt lines are:
REM      <header to write>|<source file under src\>|<array name the engine uses>|<aliases>
REM
REM  The third field matters: the assembler always emits a generically named
REM  array ("dwVertexShader" / "dwPixelShader"), but the engine expects a
REM  specific name per shader, and the tree mixes two conventions
REM  (dwCompiledPShaderfxbColorPixelShader and dwFdx8ColorPixelShader both
REM  occur). The manifest records the exact name each header must define.
REM
REM  NOTE: :one deliberately does not use setlocal, because setlocal would
REM  discard the OK/BAD counters when it returns.
REM ===========================================================================

cd /d "%~dp0"

if not exist "out" md "out"
if exist "out\_REPORT.txt" del "out\_REPORT.txt"

REM Absolute paths: the assembler runs inside a staging directory.
set "ROOT=%CD%"
set "VSA=%ROOT%\tools\vsa.exe"
REM psa/vsa here are the DirectX 8.1 SDK assemblers (KERNEL32 + ADVAPI32 only).
REM The D3DX9 Shader Assembler 9.25 was tried instead: it handles the saturation
REM modifiers these shaders use, but it demands explicit dcl_position /
REM dcl_texcoord varying declarations and so rejects 134 of the 239. See the
REM comment in :one.
set "PSA=%ROOT%\tools\psa.exe"
set "SRCDIR=%ROOT%\flat"

if not exist "%VSA%" (
  echo ERROR: cannot find "%VSA%".
  goto :fail
)
if not exist "%PSA%" (
  echo ERROR: cannot find "%PSA%".
  goto :fail
)

REM Fail fast if delayed expansion is not actually working. Without it every
REM !VAR! below reaches the command line literally, which produced 239
REM identical-looking "ASSEMBLER PRODUCED NO .h" errors instead of one clear
REM message.
set "_DEXP_TEST=ok"
if "!_DEXP_TEST!"=="ok" (
  echo.
) else (
  echo ERROR: delayed expansion is not working in this cmd.exe, so this
  echo        script cannot run. That should not happen on Windows XP or later.
  goto :fail
)
echo Shader pack version: 10
echo.

set /a OK=0
set /a BAD=0
set /a N=0

for /f "usebackq tokens=1,2,3,4 delims=|" %%A in ("%ROOT%\manifest.txt") do call :one "%%A" "%%B" "%%C" "%%D"

echo.
echo ============================================================
echo   assembled : !OK!
echo   FAILED    : !BAD!
echo   report    : out\_REPORT.txt
echo ============================================================
echo.
echo Copy the contents of .\out over ma\Lib\Fang2\dx\ in the source tree.
if not "!BAD!"=="0" (
  echo.
  echo SOME SHADERS FAILED -- please send back out\_REPORT.txt.
)
exit /b 0

REM ---------------------------------------------------------------------------
REM  :one  <target header>  <source file>  <array name>  <aliases>
REM ---------------------------------------------------------------------------
:one
set /a N+=1
set "HEADER=%~1"
set "SRC=%~2"
set "WANT=%~3"
REM Aliases are ";"-separated. Three shaders are referenced by the engine under
REM both the fdx8 and fxb spellings but only ship one header, so the header
REM has to answer to both names.
set "ALIASES=%~4"

REM ---- derive the assembler's input basename from the required header name ----
REM   CompiledVShaderfdx8PassThru_1tc.h  ->  fdx8PassThru_1tc
REM   win/fdxsh_psprite.cvs             ->  fdxsh_psprite
set "BASE=%HEADER%"
set "BASE=%BASE:.cvs=%"
set "BASE=%BASE:.h=%"
set "BASE=%BASE:CompiledVShader=%"
set "BASE=%BASE:CompiledPShader=%"
set "BASE=%BASE:win/=%"

REM ---- which assembler, and which input extension it accepts ----
set "ASM=%VSA%"
set "EXT=.vsh"
if /I "%HEADER:~0,15%"=="CompiledPShader" (
  set "ASM=%PSA%"
  set "EXT=.psh"
)

if not exist "%SRCDIR%\%SRC%" (
  set /a BAD+=1
  echo [!N!] MISSING SOURCE  %SRC%
  >>"%ROOT%\out\_REPORT.txt" echo [!HEADER!] MISSING SOURCE %SRC%
  goto :eof
)

REM ---- assemble in a clean directory ----
REM These are the DirectX 8.1 assemblers, not the D3DX9 ones. The D3DX9 9.25
REM assembler understands the saturation modifiers some of these shaders use,
REM but it rejects 134 of them because it requires explicit "dcl_position" /
REM "dcl_texcoord" varying declarations that this tree does not write -- it
REM reports "Read of uninitialized components in v" over a thousand times. The
REM DirectX 8.1 assembler infers varyings from usage and accepts them, so it is
REM the right tool here; src\ has the few genuinely malformed shaders corrected.
REM
REM The 8.1 assembler takes "-h" to emit a header, but leaves both the output
REM filename and the array name up to the caller, hence the glob and the rename
REM below. (The D3DX9 assembler could do both with /Fh and /Vn, but could not
REM assemble this source set at all.)
set "STAGE=%ROOT%\work"
if exist "%STAGE%" rd /s /q "%STAGE%"
md "%STAGE%"
REM Sources come from flat\, which are preprocessed: the 8.1 assembler has no
REM preprocessor at all ("preprocessor directives are not supported"), so
REM #include and #define are expanded ahead of time. Nothing else is staged, so
REM the only .h in STAGE is the one the assembler writes.
copy /y "%SRCDIR%\%SRC%" "%STAGE%\%BASE%%EXT%" >nul

pushd "%STAGE%"
"!ASM!" -h "%BASE%%EXT%" >"%ROOT%\work\assembler.log" 2>&1
popd

REM The only .h in STAGE is the one the assembler wrote.
set "PROD="
for %%F in ("%STAGE%\*.h") do set "PROD=%%F"

if not defined PROD (
  set /a BAD+=1
  echo [!N!] NO OUTPUT FROM ASSEMBLER  %BASE%
  >>"%ROOT%\out\_REPORT.txt" echo [!HEADER!] ASSEMBLER PRODUCED NO .h ^(src %SRC%^)
  type "%ROOT%\work\assembler.log" >>"%ROOT%\out\_REPORT.txt"
  goto :eof
)

REM ---- rewrite the generic array name, and copy the whole file ----
REM vsa emits "DWORD dwVertexShader[]", psa "DWORD dwPixelShader[]". Every line
REM has to be written: stopping after the declaration leaves an unterminated
REM array that will not compile.
set "OUTFILE=%ROOT%\out\%HEADER%"
for %%D in ("%OUTFILE%") do if not exist "%%~dpD" md "%%~dpD"
if exist "%OUTFILE%" del "%OUTFILE%"

for /f "usebackq delims=" %%L in ("%PROD%") do (
  set "LINE=%%L"
  set "LINE=!LINE:dwVertexShader[]=%WANT%[]!"
  set "LINE=!LINE:dwPixelShader[]=%WANT%[]!"
  >>"%OUTFILE%" echo !LINE!
)

REM ---- verify: right array name, and a complete declaration ----
find "%WANT%" "%OUTFILE%" >nul
if errorlevel 1 (
  set /a BAD+=1
  echo [!N!] SYMBOL MISMATCH  %HEADER!  wanted %WANT%
  >>"%ROOT%\out\_REPORT.txt" echo [!HEADER!] SYMBOL %WANT% NOT FOUND IN OUTPUT
  goto :eof
)
find /c "};" "%OUTFILE%" >nul
if errorlevel 1 (
  set /a BAD+=1
  echo [!N!] TRUNCATED OUTPUT  %HEADER!
  >>"%ROOT%\out\_REPORT.txt" echo [!HEADER!] OUTPUT TRUNCATED - no closing brace
  goto :eof
)

REM Three shaders are referenced by the engine under both the fdx8 and fxb
REM spellings but only ship one header, so it answers to both names.
if not "%ALIASES%"=="" >>"%OUTFILE%" echo #define %ALIASES% %WANT%

set /a OK+=1
goto :eof

:fail
echo.
echo BUILD ABORTED -- the package looks incomplete.
exit /b 1
