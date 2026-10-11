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
            ├─ D3D11 → D3D12 bridge on the same GPU (shared textures + shared fence)
            └─ amd_fidelityfx_loader_dx12.dll → amd_fidelityfx_upscaler_dx12.dll (FSR 4.1.1)
```

## Requirements

- **FSR 4 needs an AMD Radeon RX 7000 or RX 9000 GPU** with a current driver. On other GPUs the SDK only
  offers FSR 3.1.x. The mod still works there, and `NIER_FSR4.log` shows which provider was picked.
- Windows 10 1703 or newer (D3D11.4 shared fences), with a DirectX 12 capable driver.
- The original NIER_DLAA 1.1 archive (`d3d11.dll` sha256 `f1c77c58…7ce`).

## Install

Copy these into the folder that contains `NieRAutomata.exe`:

| File | From |
|---|---|
| `d3d11.dll` | the mod's `d3d11.dll` after running `tools/patch_mod.py` |
| `NIER_DLAA.ini` | the mod (unchanged; Home still toggles AA) |
| `NIER_FSR4.dll`, `NIER_FSR4.ini` | this repo (`./build.sh`, `dist/`) |
| `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` | FSR SDK 2.3.0, `Kits/FidelityFX/signedbin/` |

`nvngx_dlss.dll` is no longer needed.

The game's own AA should be off, as it is with the DLAA mod. Keep in-game Motion Blur **on** if you want
the character motion vectors (`ObjectMotion=1`). The mod hides the blur itself.

## Build

```
sudo apt install mingw-w64            # or any MinGW-w64 g++ on Windows
git clone --depth 1 --branch v2.3.0 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK sdk   # pinned: matches third_party/
./build.sh <folder with the extracted NIER_DLAA files> sdk
# → out/package/ is ready to drop into the game folder
```

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

Logs go to `NIER_FSR4.log` (FSR side) and `NIER_DLAA.log` (mod side).

## Status

This was built and checked on Linux with MinGW (exports, vtable layout, patch disassembly and a Wine
smoke test of the NGX entry points). It has **not been run in the game on Radeon hardware yet**. The first
in-game run should check `NIER_FSR4.log` for `provider 4.1.1` and for the evaluate lines.

How the mod was taken apart is in [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md).

FSR headers in `third_party/FidelityFX` are © AMD, under the license in that folder.
