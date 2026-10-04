cmake_minimum_required(VERSION 3.20)
project(ma C CXX)

# ---------------------------------------------------------------------------
# This rebuilds the original Metal Arms Windows target (App/ma/win/ma_win.vcproj)
# with mingw-w64 instead of Visual Studio .NET 2003.
#
#   . /workspaces/ma/tools/env.sh
#   cmake -S . -B ../build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-i686.cmake
#   cmake --build ../build -j8
# ---------------------------------------------------------------------------

# The game is /MACHINE:I386 and assumes 32-bit long, 4-byte pointers and
# PowerPC-flavoured alignment. A 64-bit target would introduce silent type bugs,
# so refuse it outright rather than let it look like it works.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "ma must be built 32-bit (i686-w64-mingw32); got ${CMAKE_SIZEOF_VOID_P}-byte pointers. "
    "Configure with -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-i686.cmake")
endif()

# _FANGDEF_WINGC selects GameCube alignment and disables the SSE paths in
# fmath_vec/fangalign. The original vcproj files were inconsistent: the
# fang2win Debug library was built WITH it while ma_win was built WITHOUT it,
# which is an ABI hazard since those macros change struct layout. Keep one
# setting across every target here; default off matches retail semantics.
option(MA_WINGC "GameCube alignment, no SSE (must match across all targets)" OFF)

# The MFC debug shell is excluded (see cmake/gen_sources.py). Without this the
# build still tries to link the asset browser and vidmode picker.
option(MA_BUILD_SHELL "Build the MFC debug shell (requires MSVC; mingw cannot)" OFF)

if(MA_BUILD_SHELL)
  message(FATAL_ERROR "MA_BUILD_SHELL requires MFC and is not supported by this toolchain")
endif()

# C++11 is required, not cosmetic: the engine's vector/quaternion types are
# type-punning unions whose members have non-trivial constructors (e.g.
# CFVec4A inside CFVec3A). GCC only accepts those from -std=c++11 onward
# ("unrestricted unions"); in C++98 mode every use is a hard error. The
# SmallAMX interpreter also relies on computed goto, hence the GNU extensions.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)          # gnu++11: computed gotos, anonymous structs
set(CMAKE_C_STANDARD 99)

add_compile_options(
  -fpermissive                        # 2003 MSVC code; downgrade what we can
  -Wno-deprecated-declarations
  -Wno-unused-variable
  -Wno-unused-but-set-variable
  -Wno-unused-function
  -Wno-unknown-pragmas
  -Wno-write-strings
  -Wno-invalid-offsetof
  -Wno-pointer-to-int-cast
  -Wno-int-to-pointer-cast
  -Wno-long-long
)

include(cmake/sources_smallamx.cmake)
include(cmake/sources_fang2win.cmake)
include(cmake/sources_ma_win.cmake)

# Definitions shared by every target. Kept uniform so the game and the engine
# library cannot disagree about struct layout.
add_compile_definitions(
  WIN32
  _WINDOWS
  _FANGDEF_PLATFORM_WIN
  $<$<CONFIG:Debug>:_DEBUG>
  $<$<CONFIG:Release>:NDEBUG>
  _FANGDEF_ENABLE_DEV_FEATURES
)
if(MA_WINGC)
  add_compile_definitions(_FANGDEF_WINGC)
endif()

# ---------------------------------------------------------------------------
# SmallAMX -- the scripting VM (Pawn-like) the game logic is scripted against.
# Third-party C, needs <stdint.h> types that used to arrive transitively.
# ---------------------------------------------------------------------------
add_library(smallamx STATIC ${SMALLAMX_SOURCES})
target_include_directories(smallamx PUBLIC Lib/SmallAMX)
target_compile_definitions(smallamx PRIVATE AMX_NODYNAL)
target_compile_options(smallamx PRIVATE -include stdint.h)

# ---------------------------------------------------------------------------
# Fang2 -- engine core plus the D3D8 renderer.
# ---------------------------------------------------------------------------
# compat/mfc.cpp implements the CWinThread subset fdx8loop.cpp uses. It is
# listed here rather than in cmake/sources_fang2win.cmake because that file is
# generated from the .vcproj by cmake/gen_sources.py and must stay in sync.
# compat/d3dx8_compat.cpp supplies the D3DX8 entry points mingw does not have
# (D3DXLoadSurfaceFromMemory, D3DXCreateCubeTexture).
add_library(fang2win STATIC ${FANG2WIN_SOURCES}
  compat/mfc.cpp
  compat/d3dx8_compat.cpp
)
target_include_directories(fang2win PUBLIC
  Lib/Fang2
  Lib/Fang2/dx
  Lib/SmallAMX
  Lib/Bink
  compat
)
target_include_directories(fang2win PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fang2win PUBLIC
  smallamx
  d3d8                # renderer device
  dinput8             # pad
  dsound              # sound
  winmm               # timers
  msacm32             # adpcm decode (fdx8audio.cpp)
  dxguid
  uuid
  gdi32
  ole32
)

# ---------------------------------------------------------------------------
# ma_win -- the game.
# ---------------------------------------------------------------------------
# App/ma/win/main_win.cpp is not in the original .vcproj: the retail PC build
# got its entry point from MFC (CMa_winApp in App/ma/win/ma_win.cpp, excluded
# below as it needs real MFC). main_win.cpp supplies a plain WinMain that runs
# the same fang_Init/Fang_ConfigDefs/fang_Startup/gameloop_Start sequence.
add_executable(ma_win
  ${MA_WIN_SOURCES}
  App/ma/win/main_win.cpp
)
target_include_directories(ma_win PRIVATE
  App/ma
  App/ma/win
  App/ma/Ai
  Lib/Fang2
  Lib/Fang2/dx
  Lib/SmallAMX
  Lib/Bink
  compat
)
target_link_libraries(ma_win PRIVATE
  fang2win
  ${CMAKE_CURRENT_SOURCE_DIR}/Lib/Bink/binkw32.lib   # Bink video (App/ma/win/binkw32.dll)
)
