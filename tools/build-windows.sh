#!/bin/sh
# Infinity-Link-Direct.exe for Windows x64, cross-compiled with llvm-mingw in Docker, linked statically.
#   ./tools/build-windows.sh   → dist/Infinity-Link-Direct-<version>-windows-x64.zip (Infinity-Link-Direct.exe, LICENSE.txt, SOURCE.txt)
# On the Mac it runs itself on the build server (ssh DX340) and brings dist/ back; LOCAL=1 builds here.
# x64 only: an ASIO driver is a DLL loaded into this process, and the makers ship x64 (and old 32-bit) drivers.
set -e
cd "$(dirname "$0")/.."
VERSION=${VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}
if [ "$(uname -s)" = Darwin ] && [ "${LOCAL:-}" != 1 ]; then
    R=DX340 RD=inf-link-direct
    ssh $R "mkdir -p $RD"
    rsync -az --delete --exclude '/build*' --exclude /dist --exclude .git ./ "$R:$RD/"
    st=0; ssh $R "cd $RD && rm -rf dist && VERSION='$VERSION' sh tools/build-windows.sh" || st=$?
    mkdir -p dist && rsync -az "$R:$RD/dist/" dist/
    exit $st
fi
mkdir -p dist
out="Infinity-Link-Direct-$VERSION-windows-x64"
if docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/dist mstorsjo/llvm-mingw:latest sh -ec "
    cmake -S /src -B /tmp/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Windows \
          -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ >/dev/null
    cmake --build /tmp/b --target Infinity-Link-Direct
    mkdir -p /tmp/$out
    x86_64-w64-mingw32-strip -o /tmp/$out/Infinity-Link-Direct.exe /tmp/b/Infinity-Link-Direct.exe
    cp /src/LICENSE /tmp/$out/LICENSE.txt
    printf 'Infinity-Link-Direct %s — source: https://github.com/tekchansin/Infinity-Link-Direct (GPLv3, LICENSE.txt)\nASIO is a registered trademark of Steinberg Media Technologies GmbH.\n' '$VERSION' > /tmp/$out/SOURCE.txt
    sed -i 's/\$/\r/' /tmp/$out/LICENSE.txt /tmp/$out/SOURCE.txt
    cd /tmp && rm -f /dist/$out.zip && zip -qr /dist/$out.zip $out" >dist/build-windows.log 2>&1; then
    echo "✅ dist/$out.zip"
else
    echo "❌ dist/build-windows.log"
    tail -30 dist/build-windows.log
    exit 1
fi
