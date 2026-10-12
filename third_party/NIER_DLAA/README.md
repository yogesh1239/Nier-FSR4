# NIER_DLAA 1.1 (third party)

Files in this folder come from the **NIER_DLAA 1.1** mod for NieR:Automata (NIER_DLAA on Nexus Mods).
They are the mod author's work, kept here unmodified as the build input for NIER_FSR4.

| File | sha256 |
|-|-|
| `d3d11.dll` (original) | `f1c77c58fdd265b25dfa262c8660d49a4463a05bb0ee88daf732727f15e717ce` |
| `NIER_DLAA.ini` | `7016a23820c56b8afc3d17a7f4a3c84af1a8df57930b1f2334f7f6bc00a504cc` |

The original `d3d11.dll` is never released as is. Releases ship it only after `tools/patch_mod.py`
replaced its NGX loader (41 bytes), which gives the patched build

`3d628eb576b505180206653d72e4e4da6a4345334f19863256ab5b10e06b6915`

To reproduce it:

```
python3 tools/patch_mod.py third_party/NIER_DLAA/d3d11.dll patched_d3d11.dll
```

`nvngx_dlss.dll` (NVIDIA) is not part of this repo or of the release; it is not needed.

Permission to redistribute the mod is the repo owner's responsibility. Credit goes to the NIER_DLAA author.
