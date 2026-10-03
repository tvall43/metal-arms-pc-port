@echo off
REM Regenerate the Compiled*Shader*.h files from the .vsh/.psh sources.
REM
REM These are Direct3D 8 vertex/pixel shader ASSEMBLY sources. The token
REM streams the engine #includes were produced at build time by Microsoft's
REM DirectX 8 SDK tools:
REM
REM     vsa.exe -h  <file>.vsh      ->  CompiledVShader<name>.h
REM     psa.exe -h  <file>.psh      ->  CompiledPShader<name>.h
REM
REM (-h means "emit a .h file" rather than a .vso/.pso blob.) They are 32-bit
REM Windows console executables that import only KERNEL32.dll and ADVAPI32.dll,
REM so they also run under Wine on Linux/macOS:
REM
REM     wine vsa.exe -h foo.vsh
REM
REM Run this from the directory holding the .vsh/.psh sources. The naming
REM convention in the tree is inconsistent -- the same source is referenced
REM under both "fdx8<name>" and "fxb<name>" output headers -- so emit both.
REM See cmake/shader_map.tsv for the authoritative source -> header mapping.

setlocal
set VSA=%~1
set PSA=%~2
if "%VSA%"=="" set VSA=vsa.exe
if "%PSA%"=="" set PSA=psa.exe

for %%F in (*.vsh) do call :one "%VSA%" "%%F"
for %%F in (*.psh) do call :one "%PSA%" "%%F"
echo Done.
goto :eof

:one
REM %1 = assembler, %2 = source file
setlocal
for %%F in (%~2) do set BASE=%%~nF
REM strip a leading "fxb" to get the logical shader name
set STRPFX=%BASE:fxb=%
"%~1" -h "%~2"
REM emit the alternate spellings the engine also #includes
if /I "%BASE:~0,3%"=="fxb" (
  "%~1" -h -o"CompiledVShaderfdx8%STRPFX%.h" "%~2"
  "%~1" -h -o"CompiledVShaderfxb%BASE%.h"    "%~2"
) else (
  "%~1" -h -o"CompiledVShaderfdx8%BASE%.h"    "%~2"
  "%~1" -h -o"CompiledVShaderfxb%BASE%.h"    "%~2"
)
endlocal
