Metal Arms -- shader assembly package
=====================================

What this is
------------
The Metal Arms engine #includes 238 pre-assembled Direct3D 8 shader headers,
plus one point-sprite shader blob, that are not in the source tree. They were
generated at build time by Microsoft's DirectX 8 shader assemblers:

    vsa.exe -h  <file>.vsh    vertex shaders
    psa.exe -h  <file>.psh    pixel shaders

Both assemblers are bundled in .\tools (they are 32-bit console programs from
the DirectX 8.1 SDK that need only KERNEL32.dll and ADVAPI32.dll, so they run
fine on Windows XP).

This is the last thing blocking the Linux build of the game. Everything else
already compiles.


How to use it
-------------
1. Unzip this anywhere on the Windows machine (the XP VM is fine).
2. Double-click  build_all.bat
3. It assembles all 239 shaders into the .\out folder and writes a summary.
   It should end with:

       assembled : 239
       FAILED    : 0

4. Copy the *contents* of .\out back into the source tree, overwriting:

       ma\Lib\Fang2\dx\

   so that you end up with

       ma\Lib\Fang2\dx\CompiledPShaderfdx8Color.h      (and 237 more)
       ma\Lib\Fang2\dx\win\fdxsh_psprite.cvs

5. Copy the .\out folder back here, or just zip it and send it over. If you zip
   it, include out\_REPORT.txt -- if anything failed, that file says which and
   why.


If it reports failures
----------------------
Send back out\_REPORT.txt. It names each shader that failed and, where the
assembler had something to say, includes its output. The most likely causes
would be an unusual assembler invocation or a source that does not assemble;
both are fixable from here without another round trip.


How it works
------------
manifest.txt has one line per output:

    <header to write>|<source file under src>|<array name the engine uses>
    CompiledPShaderfdx8Color.h|fxbColor.psh|dwFdx8ColorPixelShader

For each line the script:

  1. derives the assembler's input basename from the required header name
     (CompiledVShaderfdx8PassThru_1tc.h -> fdx8PassThru_1tc),
  2. stages the source under that basename in a clean directory,
  3. runs vsa.exe or psa.exe with -h on it,
  4. rewrites the array name in the generated header, and
  5. verifies the expected array name is present, failing loudly if not.

Two details that are easy to get wrong, and are handled here:

  * The assembler writes its output next to the input, but the filename it
    chooses is not documented and the tools have no -o option. So the script
    takes whatever single .h the assembler leaves in its staging directory
    rather than assuming a name.

  * The assembler always emits a generically named array -- "dwVertexShader[]"
    for vsa, "dwPixelShader[]" for psa -- but the engine expects a specific
    name per shader. The tree mixes two conventions, and both occur in the same
    files:

        dwCompiledPShaderfxbColorPixelShader     (full header basename)
        dwFdx8ColorPixelShader                   (short, capitalised)

    No shader uses both spellings, so the correct name cannot be derived from
    the filename. manifest.txt carries the exact name each header must define,
    taken from the engine sources. _REPORT.txt would flag any mismatch.


Contents
--------
    build_all.bat   the script; this is all you need to run
    manifest.txt    239 output/source/array-name mappings
    src\            the 125 .vsh/.psh/.nvp/.nvv assembly sources
    tools\vsa.exe   DirectX 8.1 vertex shader assembler
    tools\psa.exe   DirectX 8.1 pixel shader assembler
    out\            (created by the script; send this back)

Some shaders exist only as .nvp/.nvv files rather than .psh/.vsh. Those are
byte-identical to the .psh/.vsh files of the same name -- the tree just uses
the older naming for shaders that were never split into separate vertex and
pixel variants -- so the script stages them under a .psh/.vsh name, which is
what the assembler expects.
