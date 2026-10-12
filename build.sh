#!/usr/bin/env bash
# Builds NIER_FSR4.dll with MinGW-w64 and, given the original mod archive
# contents, assembles a ready-to-install package in out/.
#
#   ./build.sh                              # just the dll
#   ./build.sh <mod dir> <FidelityFX SDK>   # dll + patched mod + FSR binaries in out/package
#
# NIER_FSR4_BUILD (env) is compiled in and printed in the log; default "dev".
# PYTHON (env) selects the interpreter for tools/patch_mod.py; default python3.
# The CI release uses the two-argument form with third_party/NIER_DLAA.
set -euo pipefail
# Resolve the arguments against the caller's directory before changing to the repo.
ARGS=()
for a in "$@"; do ARGS+=("$(realpath "$a")"); done
set -- "${ARGS[@]+"${ARGS[@]}"}"
cd "$(dirname "$0")"

CXX=${CXX:-x86_64-w64-mingw32-g++}
mkdir -p out
"$CXX" -std=c++17 -O2 -Wall -Wno-unused-parameter "-DNIER_FSR4_BUILD=\"${NIER_FSR4_BUILD:-dev}\"" -shared -static -static-libgcc -static-libstdc++ \
    -o out/NIER_FSR4.dll src/nier_fsr4.cpp src/ngx_params.cpp \
    -ldxgi -ldxguid -luuid -Wl,--kill-at
echo "built out/NIER_FSR4.dll"

if [[ $# -ge 2 ]]; then
    MOD=$1
    SDK=$2
    # The DLLs must come from the same SDK release as third_party/FidelityFX.
    if ! git -C "$SDK" describe --tags --exact-match 2>/dev/null | grep -qx v2.3.0; then
        echo "warning: cannot confirm $SDK is the v2.3.0 tag the headers were taken from" >&2
    fi
    PKG=out/package
    rm -rf "$PKG"
    mkdir -p "$PKG"
    "${PYTHON:-python3}" tools/patch_mod.py "$MOD/d3d11.dll" "$PKG/d3d11.dll"
    cp "$MOD/NIER_DLAA.ini" "$PKG/"
    cp out/NIER_FSR4.dll dist/NIER_FSR4.ini "$PKG/"
    cp "$SDK/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll" \
       "$SDK/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll" "$PKG/"
    cp "$SDK/Kits/FidelityFX/docs/license.md" "$PKG/AMD_FidelityFX_LICENSE.md"
    cp README.md "$PKG/NIER_FSR4_README.md"
    echo "package in $PKG"
fi
