set -e
cd "$(dirname "$0")"
mkdir -p ../out
i686-w64-mingw32-gcc -O2 -Wall -Wno-unused-function -Wno-misleading-indentation -Wno-unused-variable -std=gnu11 -fno-strict-aliasing \
  -shared -o ../out/version.dll pirates_coop.c version_proxy.c stubs.S version.def \
  -static-libgcc -Wl,--kill-at -Wl,--enable-stdcall-fixup -ld3d9 -lshlwapi -s
ls -la ../out/version.dll
