#!/usr/bin/env bash
#
# Build only the B2S plugin Windows DLL, for quick testing.
#
# Cross-compiles the plugin (and nothing else, BUILD_APP=OFF) with mingw-w64:
#   ./tools/build_b2s_plugin_windows.sh            # x64, Release
#   ./tools/build_b2s_plugin_windows.sh x64 Debug
#   ./tools/build_b2s_plugin_windows.sh x86        # 32 bit plugin-b2spro.dll
#
# Output: build-b2s-win-<arch>/plugins/b2s-pro/  (plugin DLL + plugin.cfg + the mingw
# runtime DLLs, ready to be dropped in the Visual Pinball plugins/b2s-pro folder).
#
# Dependencies: cmake, ninja or make, and the mingw-w64 toolchain:
#   Debian/Ubuntu:  sudo apt install mingw-w64
#   MSYS2 UCRT64:   pacman -S mingw-w64-ucrt-x86_64-gcc   (see the x64 line below)
#
# No root on a Debian/Ubuntu box: the toolchain can be extracted in the home directory, the
# gcc/g++ binaries find their install directory relative to themselves so a plain 'dpkg -x'
# tree works (the -posix variants are used, the win32 ones have no POSIX threads):
#   mkdir -p ~/.local/mingw/dl ~/.local/mingw/root; cd ~/.local/mingw/dl
#   apt-get download binutils-mingw-w64-base binutils-mingw-w64-x86-64 mingw-w64-common \
#      mingw-w64-x86-64-dev gcc-mingw-w64-base gcc-mingw-w64-x86-64-posix-runtime \
#      gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix
#   apt-get download binutils-mingw-w64-i686 mingw-w64-i686-dev gcc-mingw-w64-i686-posix-runtime \
#      gcc-mingw-w64-i686-posix g++-mingw-w64-i686-posix      # for the x86 plugin
#   for d in *.deb; do dpkg -x "$d" ~/.local/mingw/root; done
#   cd ~/.local/mingw/root/usr/bin
#   ln -sf x86_64-w64-mingw32-gcc-posix  x86_64-w64-mingw32-gcc
#   ln -sf x86_64-w64-mingw32-g++-posix  x86_64-w64-mingw32-g++
#   ln -sf i686-w64-mingw32-gcc-posix    i686-w64-mingw32-gcc
#   ln -sf i686-w64-mingw32-g++-posix    i686-w64-mingw32-g++
#   export PATH=~/.local/mingw/root/usr/bin:$PATH
#
#   Windows/MSVC:   use the 'b2s-plugin' GitHub Actions workflow, or
#                   cmake -DPLATFORM=windows -DARCH=x64 -DBUILD_APP=OFF -B build &&
#                   cmake --build build --config Release --target B2SPlugin --parallel

set -euo pipefail

ARCH="${1:-x64}"
CONFIG="${2:-Release}"

cd "$(dirname "${BASH_SOURCE[0]}")/.."

case "${ARCH}" in
   x64)
      CC=x86_64-w64-mingw32-gcc
      CXX=x86_64-w64-mingw32-g++
      SYSTEM_PROCESSOR=AMD64
      pacman_pkg=mingw-w64-ucrt-x86_64-gcc
      ;;
   x86)
      CC=i686-w64-mingw32-gcc
      CXX=i686-w64-mingw32-g++
      SYSTEM_PROCESSOR=x86
      pacman_pkg=mingw-w64-i686-gcc
      ;;
   *)
      echo "usage: $0 [x64|x86] [Release|Debug]" >&2
      exit 1
      ;;
esac

if ! command -v cmake >/dev/null; then
   echo "cmake is required (apt install cmake, pacman -S mingw-w64-ucrt-x86_64-cmake)" >&2
   exit 1
fi

if ! command -v "${CXX}" >/dev/null; then
   cat >&2 <<EOF
${CXX} not found; install the mingw-w64 toolchain:
  Debian/Ubuntu: sudo apt install mingw-w64
  MSYS2 UCRT64:  pacman -S ${pacman_pkg} mingw-w64-ucrt-x86_64-cmake
EOF
   exit 1
fi

# The plugin includes pinmame/PinMAMEPlugin.h from third-party/include/pinmame, which the full
# external.sh dependency build extracts from the pinmame sources. For a plugin only build, we
# simply fetch the header (it is a standalone interface declaration)
if [ ! -f third-party/include/pinmame/PinMAMEPlugin.h ]; then
   echo "Fetching PinMAME plugin API header from the vpinmame repository"
   tmp="$(mktemp -d)"
   trap 'rm -rf "${tmp}"' EXIT
   git clone --depth 1 https://github.com/vpinball/pinmame "${tmp}/pinmame"
   mkdir -p third-party/include/pinmame
   cp "${tmp}/pinmame/src/libpinmame/PinMAMEPlugin.h" third-party/include/pinmame/
fi

BUILD_DIR="build-b2s-win-${ARCH}"

cmake \
   -DPLATFORM=windows-mingw \
   -DARCH="${ARCH}" \
   -DBUILD_APP=OFF \
   -DPOST_BUILD_COPY_EXT_LIBS=OFF \
   -DCMAKE_BUILD_TYPE="${CONFIG}" \
   -DCMAKE_C_COMPILER="${CC}" \
   -DCMAKE_CXX_COMPILER="${CXX}" \
   -DCMAKE_SYSTEM_NAME=Windows \
   -DCMAKE_SYSTEM_PROCESSOR="${SYSTEM_PROCESSOR}" \
   -B "${BUILD_DIR}"

cmake --build "${BUILD_DIR}" --target B2SPlugin -j"$(nproc)"

# Stage the mingw runtime next to the plugin so the folder can be dropped into a Visual
# Pinball installation as is ('-print-file-name' resolves them inside the toolchain)
OUT="${BUILD_DIR}/plugins/b2s-pro"
# The 32 bit libgcc runtime is the dwarf2 one, the 64 bit one the SEH one
case "${ARCH}" in
   x64) gcc_runtime=libgcc_s_seh-1.dll ;;
   *)   gcc_runtime=libgcc_s_dw2-1.dll ;;
esac
for dll in libstdc++-6.dll "${gcc_runtime}" libwinpthread-1.dll; do
   src="$("${CXX}" -print-file-name="${dll}")"
   if [ -f "${src}" ]; then
      cp -f "${src}" "${OUT}/"
   else
      echo "warning: ${dll} not found in the toolchain, the plugin will need it from an official VPX build" >&2
   fi
done

echo
echo "Built: ${OUT}/$( [ "${ARCH}" = x64 ] && echo plugin-b2spro64.dll || echo plugin-b2spro.dll ) (${CONFIG})"
ls -la "${OUT}"
