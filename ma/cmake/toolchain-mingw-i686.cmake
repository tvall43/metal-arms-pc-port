set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(MA_TOOLS /workspaces/ma/tools)

set(CMAKE_C_COMPILER   ${MA_TOOLS}/bin/i686-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER ${MA_TOOLS}/bin/i686-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  ${MA_TOOLS}/bin/i686-w64-mingw32-windres)
set(CMAKE_AR           ${MA_TOOLS}/bin/i686-w64-mingw32-ar)
set(CMAKE_RANLIB       ${MA_TOOLS}/bin/i686-w64-mingw32-ranlib)
set(CMAKE_LINKER       ${MA_TOOLS}/bin/i686-w64-mingw32-ld)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(EXISTS ${MA_TOOLS}/i686-w64-mingw32)
  set(CMAKE_FIND_ROOT_PATH ${MA_TOOLS}/i686-w64-mingw32)
elseif(EXISTS ${MA_TOOLS}/x86_64-w64-mingw32/i686-w64-mingw32)
  set(CMAKE_FIND_ROOT_PATH ${MA_TOOLS}/x86_64-w64-mingw32/i686-w64-mingw32)
else()
  set(CMAKE_FIND_ROOT_PATH ${MA_TOOLS})
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# SSE/SSE2 are baseline for any x86-64-era CPU and are already assumed by the
# original MSVC build (it used __m128 in CFVec4A and the XMM intrinsics). Enable
# them explicitly: GCC otherwise targets generic i686, where _mm_* intrinsics
# are always_inline and fail with "target specific option mismatch".
add_compile_options(-m32 -msse -msse2)
add_link_options(-m32)
