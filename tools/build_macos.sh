#!/usr/bin/env bash
# Configure and build on macOS (Apple Silicon: Xcode's clang, CMake, Ninja), like build_linux.sh.
#   tools/build_macos.sh [--disc <game.rvz|.iso|main.dol>] [--build-dir <dir>] [--configure]
# Result: build/macos/Red Two.app (Dawn and SDL3 linked in statically: nothing else to install).
# Needs: Xcode command line tools, cmake (3.25+), ninja (brew install cmake ninja).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build/macos"
disc=""
configure=0
config="RelWithDebInfo"
cmake_args=()
while [ $# -gt 0 ]; do
    case "$1" in
    --disc) disc="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; configure=1; shift 2 ;;
    --build-dir) mkdir -p "$2"; build="$(cd "$2" && pwd)"; shift 2 ;;
    --configure) configure=1; shift ;;
    --config) config="$2"; shift 2 ;;
    -D*) cmake_args+=("$1"); shift ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done

if [ ! -f "$root/extern/aurora/CMakeLists.txt" ] || [ ! -f "$root/extern/rcheevos/include/rc_client.h" ]; then
    git -C "$root" submodule update --init --recursive
fi

# Our fixes to Aurora, as build_linux.sh
aurora="$root/extern/aurora"
clean=0
[ -z "$(git -C "$aurora" status --porcelain --untracked-files=no)" ] && clean=1
for patch in "$root"/patches/aurora/*.patch; do
    if [ $clean -eq 0 ] && git -C "$aurora" apply --reverse --check "$patch" 2>/dev/null; then continue; fi
    if git -C "$aurora" apply "$patch" 2>/dev/null; then echo "applied $(basename "$patch")"; continue; fi
    if [ $clean -eq 1 ]; then echo "could not apply $(basename "$patch") to extern/aurora" >&2; exit 1; fi
    echo "note: $(basename "$patch") neither applies nor reverses on the modified extern/aurora (assumed applied)"
done

if [ $configure -eq 1 ] || [ ! -f "$build/build.ninja" ]; then
    args=(-S "$root" -B "$build" -G Ninja "-DCMAKE_BUILD_TYPE=$config"
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
        -DAURORA_SDL3_PROVIDER=vendor
        "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local" -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON)
    # Homebrew's libraries (abseil, fmt, zstd, libpng...) are not used: Aurora fetches and links
    # them statically, so the .app keeps working after a brew upgrade and on another Mac. (Both
    # options stay in the cache, so CMake re-running itself during a build keeps them too.)
    # (a fresh cache, so no earlier package lookup sticks; the disc it named stays)
    if [ -z "$disc" ] && [ -f "$build/CMakeCache.txt" ]; then
        disc="$(sed -n 's/^R2_DOL:FILEPATH=//p' "$build/CMakeCache.txt")"
    fi
    rm -f "$build/CMakeCache.txt"
    [ -n "$disc" ] && args+=("-DR2_DOL=$disc")
    cmake "${args[@]}" ${cmake_args[@]+"${cmake_args[@]}"}
fi
cmake --build "$build" --target red_two
# ad-hoc signature over the whole bundle (arm64 code must be signed to run)
codesign --force --deep --sign - "$build/Red Two.app"
echo "built $build/Red Two.app"
