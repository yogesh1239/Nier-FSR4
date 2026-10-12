# NieR:Automata FSR 4.1.1 AA (NIER_DLAA port)

This port takes the **NIER_DLAA 1.1** mod (a `d3d11.dll` proxy that adds DLAA to NieR:Automata) and runs
**AMD FSR 4.1.1** in native-AA mode instead of NVIDIA DLSS. It uses the signed binaries from the
AMD FSR SDK 2.3.0 ("Redstone").

The mod's own work is unchanged. That covers finding the HDR scene, jittering the camera, building camera and character motion
vectors, linear depth and mip bias. Only the DLSS backend is replaced:

```
NieRAutomata.exe
  └─ d3d11.dll  (NIER_DLAA 1.1, one 41-byte patch)
       └─ NGX static lib → NIER_FSR4.dll  (stands in for NVIDIA's _nvngx.dll core)
            ├─ D3D11 → D3D12 bridge on the same GPU (shared textures + shared fences)
            └─ amd_fidelityfx_loader_dx12.dll → amd_fidelityfx_upscaler_dx12.dll (FSR 4.1.1)
```

## Requirements

- **FSR 4 needs an AMD Radeon RX 7000 or RX 9000 GPU** with a current driver. On other GPUs the SDK only
  offers FSR 3.1.x. The mod still works there, and `NIER_FSR4.log` shows which provider was picked.
- Windows 10 1703 or newer (D3D11.4 shared fences), with a DirectX 12 capable driver.

## Install

1. Download `NIER_FSR4.zip` from the [Releases](https://github.com/yogesh1239/Nier-FSR4/releases) page and extract it.
2. If the game folder already has a `d3d11.dll` (Special K, ReShade, the original NIER_DLAA mod), back it up first.
   The package's `d3d11.dll` replaces it.
   If it is Special K installed as a local `d3d11.dll`, switch Special K to global injection (SKIF) or rename it to
   `dxgi.dll`; the `d3d11.dll` slot belongs to this package. Special K started through Steam launch options works as is.
3. Copy the contents of the `NIER_FSR4` folder into the folder that contains `NieRAutomata.exe`.

The package is self-contained. It holds the already patched NIER_DLAA `d3d11.dll` (so there is nothing to patch),
`NIER_DLAA.ini`, `NIER_FSR4.dll`, `NIER_FSR4.ini`, the two AMD FSR 4 DLLs and the AMD license.
`nvngx_dlss.dll` is no longer needed and can be deleted.

The game's own AA should be off, as it is with the DLAA mod. Keep in-game Motion Blur **on** if you want
the character motion vectors (`ObjectMotion=1`). The mod hides the blur itself. Home toggles the AA.

Tested on an RX 9070 XT, standalone and together with Special K (launched via Steam launch options).

To uninstall, delete the 8 files of the package (`d3d11.dll`, `NIER_DLAA.ini`, `NIER_FSR4.dll`, `NIER_FSR4.ini`,
`amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`, `AMD_FidelityFX_LICENSE.md`,
`NIER_FSR4_README.md`) plus `NIER_FSR4.log`, `NIER_FSR4.prev.log` and `NIER_DLAA.log`, and restore the
`d3d11.dll` you backed up.

## Troubleshooting

Attach `NIER_FSR4.log`, `NIER_FSR4.prev.log` (the previous session) and `NIER_DLAA.log` from the game folder.
The first lines of `NIER_FSR4.log` show the build, Windows version, the GPU and driver, which `d3d11.dll` called
into the DLL (and whether it is the patched NIER_DLAA 1.1) and any overlay or hook DLLs found in the process.
If `NIER_FSR4.log` is missing, the patched `d3d11.dll` is not in place or another mod that also uses
`d3d11.dll` overwrote it, or the game folder is not writable (Controlled Folder Access, Program Files).

## Build from source

The repo carries the NIER_DLAA 1.1 files it builds on in `third_party/NIER_DLAA` (see the README there, in the repository).

```
sudo apt install mingw-w64            # or any MinGW-w64 g++ on Windows
git clone --depth 1 --branch v2.3.0 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK sdk   # pinned: matches third_party/
./build.sh third_party/NIER_DLAA sdk
# -> out/package/ is ready to drop into the game folder
```

Set `NIER_FSR4_BUILD` to put a version string in the log (default `dev`). Every push to `main` builds the
same package in GitHub Actions, smoke-tests it on Windows and publishes it as a release.

`tools/patch_mod.py` only needs the Python standard library. It refuses any `d3d11.dll` other than the
NIER_DLAA 1.1 build.

## Settings (`NIER_FSR4.ini`)

- `Version`: the FSR provider to request (`4.1.1` by default, or `auto`).
- `Sharpening` / `Sharpness`: optional RCAS sharpening.
- `JitterSignX/Y`: flips the jitter passed to FSR. If the image looks soft or shimmers, try `JitterSignY=-1`.
  The mod's `JitterSign` and its debug keys still work as before.
- `[Camera]`: FSR wants camera near/far and FOV, which DLSS doesn't take. These are read from the
  mod's camera tracker. Fixed values can be set here instead.
- `DebugView=1`: FSR's debug overlay.

Logs go to `NIER_FSR4.log` (FSR side, the previous session is kept as `NIER_FSR4.prev.log`) and
`NIER_DLAA.log` (mod side).

## Status

Built and checked on Linux with MinGW (exports, vtable layout, patch disassembly and a Wine smoke test of the
NGX entry points), and tested in game on an AMD Radeon RX 9070 XT, standalone and together with Special K.

How the mod was taken apart is in [docs/REVERSE_ENGINEERING.md](https://github.com/yogesh1239/Nier-FSR4/blob/main/docs/REVERSE_ENGINEERING.md).

FSR headers in `third_party/FidelityFX` are © AMD, under the license in that folder.

## Credits

- **NIER_DLAA 1.1** by its author (NIER_DLAA on Nexus Mods) does all the work around the upscaler. It is shipped
  only with the NGX loader patch applied.
- **AMD FidelityFX SDK 2.3.0** provides FSR 4.1.1, used under the license in `AMD_FidelityFX_LICENSE.md`.
