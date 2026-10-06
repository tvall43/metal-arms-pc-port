#!/bin/sh
# Build a rootless Wine capable of running the DirectX 8 vsa.exe/psa.exe.
#
# Why this exists: the compiled Compiled*Shader*.h files were produced by
# Microsoft's DirectX 8 vsa/pssa assemblers, which are 32-bit Windows console
# EXEs. They import only KERNEL32.dll and ADVAPI32.dll, so they run fine under
# Wine -- but Debian's Wine needs an i386 runtime, and this container has no
# root and no user namespaces, so it is installed into a private prefix and run
# under PRoot.
#
# Status: Wine 10.0 launches and reports its version. Executing a PE still
# fails with "cannot get path to ntdll.so" -- Wine resolves its own path via
# /proc/self/exe, which PRoot does not present the way Wine expects. The one
# piece of the puzzle left is that path; see the notes at the bottom.
set -e
WORK=${WORK:-/tmp/opencode}
mkdir -p "$WORK"/{debs,root}

# 1. DirectX 8.1 SDK (contains vsa.exe / psa.exe under DXF/DXSDK/bin/DXUtils).
if [ ! -f "$WORK/dx81sdk_full.exe" ]; then
  curl -sSL -o "$WORK/dx81sdk_full.exe" \
    https://archive.org/download/dx81sdk_full/dx81sdk_full.exe
fi
7z x -y -o"$WORK/sdk" "$WORK/dx81sdk_full.exe" \
  'DXF/DXSDK/bin/DXUtils/vsa.exe' 'DXF/DXSDK/bin/DXUtils/psa.exe' >/dev/null

# 2. i386 package index and the wine dependency closure.
curl -sS -o "$WORK/Packages-i386.xz" \
  http://deb.debian.org/debian/dists/trixie/main/binary-i386/Packages.xz
# (generate the closure list with the python snippet in the commit message;
#  it is 189 packages: libwine, wine32, libwine's X11/ffmpeg deps, libc6, ...)

# 3. Unpack everything into the prefix. dpkg-deb -x needs no root.
for d in "$WORK"/debs/*.deb; do dpkg-deb -x "$d" "$WORK/root/"; done

# 4. Debian is merged-/usr; PRoot is unhappy with symlinks at the chroot root,
#    so give it a real /lib holding a copy of /usr/lib.
R="$WORK/root"
[ -L "$R/lib" ] && mv "$R/lib" "$R/lib.usrlink"
mkdir -p "$R/lib" && cp -a "$R/usr/lib/." "$R/lib/"
[ -L "$R/lib/ld-linux.so.2" ] && cp "$R/lib/i386-linux-gnu/ld-linux.so.2" "$R/lib/ld-linux.so.2"

# 5. Run. The loader must be the MULTIARCH one: Wine derives its libdir from
#    dirname(/proc/self/exe) and expects /usr/lib/i386-linux-gnu, so invoking
#    /lib/ld-linux.so.2 instead makes it look for /i386-linux-gnu/...
export LD_LIBRARY_PATH="$R/usr/lib/x86_64-linux-gnu:$R/lib/x86_64-linux-gnu"
exec "$WORK/proot" -r "$R" -b /proc -b /dev \
  -b "$WORK/wineprefix:/tmp/wineprefix" -b /workspaces:/workspaces -w / \
  /usr/lib/i386-linux-gnu/ld-linux.so.2 /usr/lib/wine/wine "$@"
