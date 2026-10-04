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
REM psa/vsa here are the D3DX9 Shader Assembler 9.25 (from the November 2008
REM DirectX SDK), not the DirectX 8.1 pair. 27 of the shaders are written in
REM D3D9 assembly -- saturation modifiers (mov r0, r0_sat) and ps.1.1 tex with
REM an implicit coordinate -- which the 8.1 assembler rejects outright. 9.25
REM accepts both those and ps_1_1, so it assembles the whole set. It needs
REM d3dx9_40.dll, which is bundled alongside.
set "PSA=%ROOT%\tools\psa.exe"
set "SRCDIR=%ROOT%\src"

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
echo Shader pack version: 6
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

REM ---- assemble ----
REM Stage every header in src\ as well as the source itself: 106 of the sources
REM do "#include fdx8vshader_const.h", which defines the CV_* constants they
REM then use as c[CV_WORLDVIEWPROJ_0].
set "STAGE=%ROOT%\work"
if exist "%STAGE%" rd /s /q "%STAGE%"
md "%STAGE%"
for %%H in ("%SRCDIR%\*.h") do copy /y "%%H" "%STAGE%\" >nul
copy /y "%SRCDIR%\%SRC%" "%STAGE%\%BASE%%EXT%" >nul

REM The assembler writes the header itself, named and with its array named, so
REM there is no globbing and no rewriting here:
REM   /Fh<file>  output header file containing the object code
REM   /Vn<name>  use <name> as the variable name in the header file
REM   /I<dir>    where to resolve #include from
REM (The DirectX 8.1 assemblers used "-h" and left both the filename and the
REM variable name to be guessed afterwards. This one does not.)
set "OUTFILE=%ROOT%\out\%HEADER%"
for %%D in ("%OUTFILE%") do if not exist "%%~dpD" md "%%~dpD"
if exist "%OUTFILE%" del "%OUTFILE%"

"!ASM!" /nologo /I"%STAGE%" /Fo"%STAGE%\%BASE%.obj" /Fh"%OUTFILE%" /Vn%WANT% "%STAGE%\%BASE%%EXT%" >"%ROOT%\work\assembler.log" 2>&1

if not exist "%OUTFILE%" (
  set /a BAD+=1
  echo [!N!] NO OUTPUT FROM ASSEMBLER  %BASE%
  >>"%ROOT%\out\_REPORT.txt" echo [!HEADER!] ASSEMBLER PRODUCED NO .h ^(src %SRC%^)
  type "%ROOT%\work\assembler.log" >>"%ROOT%\out\_REPORT.txt"
  goto :eof
)

REM ---- verify: the right array name, and a complete declaration ----
REM A truncated header ("DWORD dw...[ ] = {" with no closing brace) will not
REM compile, so check for both.
findstr /C:"DWORD %WANT%[]" "%OUTFILE%" >nul
if errorlevel 1 (
  set /a BAD+=1
  echo [!N!] SYMBOL MISMATCH  %HEADER!  wanted %WANT%
  >>"%ROOT%\out\_REPORT.txt" echo [!HEADER!] SYMBOL %WANT% NOT FOUND IN OUTPUT
  goto :eof
)
findstr /C:"};" "%OUTFILE%" >nul
if errorlevel 1 (
  set /a BAD+=1
  echo [!N!] TRUNCATED OUTPUT  %HEADER%
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
