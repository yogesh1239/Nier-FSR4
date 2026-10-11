# NIER_DLAA 1.1: how the DLSS path was replaced

Target: `d3d11.dll` from `NieR_Automata_DLAA_1.1` (PE32+, 452,608 bytes, sha256
`f1c77c58fdd265b25dfa262c8660d49a4463a05bb0ee88daf732727f15e717ce`, image base `0x180000000`).
The addresses below are virtual addresses at that base.

## What the mod is

A `d3d11.dll` proxy. It loads the system `d3d11.dll`, forwards the 51 exports and hooks the device and
context with MinHook. Its jobs:

- find the main camera from constant-buffer uploads, then jitter the projection
- build linear depth, device depth and camera motion vectors in compute shaders (`g_LinearDepth`,
  `g_DepthOut`, `g_MotionVectors`), and take character motion from the game's motion-blur velocity pass
- once per frame, run DLSS on the HDR scene colour (R11G11B10F / RGBA16F) and copy the result back

DLSS comes from **NVIDIA's NGX SDK linked statically** (`nvsdk_ngx_s.lib`, build path
`.../NGX/core/rel_310_9/...`). So there is no `nvngx.dll` import to redirect. The static lib loads the NGX
core itself.

## NGX static library inside the mod

| Address | Function |
|---|---|
| `0x18001ffc0` | NGX loader: log setup, `GetLUIDFromD3D11Device`, core load, `GetProcAddress` table |
| `0x18001fe00` | `GetLUIDFromD3D11Device` (works on any vendor) |
| `0x1800229a0` | `NGXLoadCoreLibrary`: registry and driver-store lookup of `_nvngx.dll`, `PCI\VEN_10DE` adapter match, `WinVerifyTrust` signer check |
| `0x180022450` | `jmp [GetProcAddress]` thunk |
| `0x1800b8270` | cached core `HMODULE` |
| `0x1800b82a0..0x1800b8308` | core entry points: Init, Init_Ext, Init_ProjectID, Shutdown, Shutdown1, GetScratchBufferSize, GetParameters, CreateFeature, EvaluateFeature, ReleaseFeature, GetFeatureRequirements, AllocateParameters, GetCapabilityParameters, DestroyParameters |
| `0x180020860` | public `NVSDK_NGX_D3D11_Init_with_ProjectID` → core `Init_ProjectID(projectId, engineType, engineVersion, path, device, sdkVersion, featureInfo*)` |
| `0x180020800` | `GetCapabilityParameters(NVSDK_NGX_Parameter**)` → core, same arguments |
| `0x180020740` | `CreateFeature(ctx, feature, params, handle**)`. Fills in the preset hints (`0x180020420`), then calls the core |
| `0x1800207c0` / `0x18001fd60` | `EvaluateFeature(ctx, handle, params, callback)`. The callback is kept in TLS, then the core is called |
| `0x180020970` | `ReleaseFeature(handle)` → core |

The loader only needs these entries to be non-null: (Init or Init_Ext), (Shutdown or Shutdown1),
(GetParameters or GetCapabilityParameters), CreateFeature, EvaluateFeature and ReleaseFeature.
`NGXLoadCoreLibrary` has a single caller, and its result is only used as a `GetProcAddress` module.

**Patch:** `tools/patch_mod.py` overwrites `NGXLoadCoreLibrary` with
`lea rcx,[rip+6]; jmp [LoadLibraryW]; L"NIER_FSR4.dll"` (41 bytes). The NVIDIA adapter check and
signature check go away with it. If the DLL is missing, `LoadLibraryW` returns null and the mod logs
"NGX init failed", then renders unchanged.

## `NVSDK_NGX_Parameter` vtable as used by the mod

MSVC puts overloaded virtuals in groups, in reverse declaration order. Confirmed slots:

| Offset | Method | Seen at |
|---|---|---|
| `0x10` | `Set(const char*, ID3D11Resource*)` | `0x18001fbf0` |
| `0x18` | `Set(const char*, int)` | `0x18001fca0` |
| `0x20` | `Set(const char*, unsigned)` | `0x18001fd00` |
| `0x30` | `Set(const char*, float)` | `0x18001fc50` |
| `0x58` | `Get(const char*, int*)` | `0x18001fb30` |
| `0x60` | `Get(const char*, unsigned*)` | `0x18001fb90` |

The full table (17 slots, through `Reset` at `0x80`) is in `src/ngx_params.cpp`.

## What the mod asks of DLSS

Init (`0x180019490`): `Init_with_ProjectID("b3f1c9e4-5a7d-4e62-8c0b-2d9f4a6e1b73", engine 0, ..., device, sdk 0x15)`.
Then `GetCapabilityParameters`, which reads `SuperSampling.Available` and `SuperSampling.NeedsUpdatedDriver`.

Create (`0x18001874d`): `Width = OutWidth`, `Height = OutHeight` (native, DLAA), `PerfQualityValue = 5` (DLAA),
`DLSS.Feature.Create.Flags = 0x43` (IsHDR, MVLowRes, AutoExposure; depth *not* inverted, MVs *not*
jittered), `DLSS.Enable.Output.Subrects = 0`, plus `DLSS.Hint.Render.Preset.DLAA` from the ini.

Evaluate (`0x180018d02`, helper `0x180019f20`):

| NGX parameter | Value |
|---|---|
| `Color` | the game's HDR scene texture |
| `Output` | the mod's target `0x1800abf70`; afterwards `CopyResource(scene, output)` |
| `Depth` | the mod's device-depth target `0x1800abf68` |
| `MotionVectors` | the mod's motion vector target `0x1800abf60` (pixels, current → previous) |
| `Jitter.Offset.X/Y` | jitter in pixels × `JitterSign` |
| `MV.Scale.X/Y` | 1.0 |
| `Reset` | history reset (size change, camera cut or jump) |
| `Render.Subrect.Dimensions` | render size |

## Mapping to FSR 4.1.1

| DLSS | FSR (`ffxDispatchDescUpscale` / create flags) |
|---|---|
| IsHDR | `FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE` |
| MVLowRes not set | `FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS` |
| MVJittered | `FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION` |
| DepthInverted | `FFX_UPSCALE_ENABLE_DEPTH_INVERTED` |
| AutoExposure | `FFX_UPSCALE_ENABLE_AUTO_EXPOSURE` |
| Jitter / MV scale / Reset | `jitterOffset` / `motionVectorScale` / `reset`. Same conventions: pixels, MV current → previous |
| (none) | `cameraNear/Far/FovAngleVertical`, read from the mod (below) |
| (none) | `frameTimeDelta`, measured with QPC |

Camera values: the mod keeps the depth constants at `0x1800abc1c` (near) and `0x1800abecc` (far − near),
written at `0x1800166ea`. The main-camera projection rows live at `0x1800abe78..`, with P11 at
`0x1800abe8c` (`0x180015788`). FOV is `2·atan(1/P11)`.

## DirectX 11 → 12

FSR 4 only ships a DX12 backend. `NIER_FSR4.dll` works like this:

1. Create a D3D12 device on the game's adapter, matched by LUID through `IDXGIFactory4::EnumAdapterByLuid`.
2. Create one shared `ID3D12Fence` and open it in D3D11 (`ID3D11Device5::OpenSharedFence`).
3. Create shared committed textures (`D3D12_HEAP_FLAG_SHARED`) for colour, depth, motion vectors and
   output (output gets UAV), and open them in D3D11 (`OpenSharedResource1`).
4. Every frame:
   - D3D11 `CopyResource` × 3, then `Signal`
   - D3D12 `Wait`, `ffxDispatch`, `Signal`
   - D3D11 `Wait`, then `CopyResource` into the mod's output

Three command allocators rotate, so the CPU never waits on the frame it just submitted.
