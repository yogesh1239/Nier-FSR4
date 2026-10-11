// Wine/Windows smoke test: drives NIER_FSR4.dll the way the mod's MSVC code
// does (raw vtable offsets), and checks the patched NGXLoadCoreLibrary stub.
//   smoke.exe <dir with NIER_FSR4.dll> [patched d3d11.dll]
#include <windows.h>
#include <cstdio>
#include <cstring>

typedef int (*PfnGetCaps)(void**);
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } else printf("ok   %s\n", #c); } while (0)

template <class F> F Slot(void* obj, int off) { return *(F*)(*(char**)obj + off); }

int main(int argc, char** argv)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\NIER_FSR4.dll", argc > 1 ? argv[1] : ".");
    HMODULE m = LoadLibraryA(path);
    CHECK(m != nullptr);
    if (!m) return 1;
    const char* names[] = {"NVSDK_NGX_D3D11_Init", "NVSDK_NGX_D3D11_Init_Ext", "NVSDK_NGX_D3D11_Init_ProjectID",
        "NVSDK_NGX_D3D11_Shutdown", "NVSDK_NGX_D3D11_Shutdown1", "NVSDK_NGX_D3D11_GetParameters",
        "NVSDK_NGX_D3D11_GetScratchBufferSize", "NVSDK_NGX_D3D11_CreateFeature", "NVSDK_NGX_D3D11_EvaluateFeature",
        "NVSDK_NGX_D3D11_ReleaseFeature", "NVSDK_NGX_D3D11_GetFeatureRequirements",
        "NVSDK_NGX_D3D11_AllocateParameters", "NVSDK_NGX_D3D11_GetCapabilityParameters",
        "NVSDK_NGX_D3D11_DestroyParameters"};
    for (auto n : names) { printf("%-40s", n); CHECK(GetProcAddress(m, n) != nullptr); }

    void* caps = nullptr;
    CHECK(((PfnGetCaps)GetProcAddress(m, "NVSDK_NGX_D3D11_GetCapabilityParameters"))(&caps) == 1);
    int avail = -1, drv = -1;
    // 0x58 Get(int*) as called by the mod at 0x18001fb30
    CHECK(Slot<int (*)(void*, const char*, int*)>(caps, 0x58)(caps, "SuperSampling.Available", &avail) == 1);
    CHECK(avail == 1);
    CHECK(Slot<int (*)(void*, const char*, int*)>(caps, 0x58)(caps, "SuperSampling.NeedsUpdatedDriver", &drv) == 1);
    CHECK(drv == 0);
    // 0x20 Set(uint) / 0x60 Get(uint*)  (0x18001fd00 / 0x18001fb90)
    Slot<void (*)(void*, const char*, unsigned)>(caps, 0x20)(caps, "Width", 3840u);
    unsigned w = 0;
    CHECK(Slot<int (*)(void*, const char*, unsigned*)>(caps, 0x60)(caps, "Width", &w) == 1 && w == 3840);
    // 0x18 Set(int)  (0x18001fca0), read back as int
    Slot<void (*)(void*, const char*, int)>(caps, 0x18)(caps, "DLSS.Feature.Create.Flags", 0x43);
    int fl = 0;
    CHECK(Slot<int (*)(void*, const char*, int*)>(caps, 0x58)(caps, "DLSS.Feature.Create.Flags", &fl) == 1 && fl == 0x43);
    // 0x30 Set(float) (0x18001fc50) / 0x70 Get(float*)
    Slot<void (*)(void*, const char*, float)>(caps, 0x30)(caps, "Jitter.Offset.X", -0.375f);
    float jx = 0;
    CHECK(Slot<int (*)(void*, const char*, float*)>(caps, 0x70)(caps, "Jitter.Offset.X", &jx) == 1 && jx == -0.375f);
    // 0x10 Set(ID3D11Resource*) (0x18001fbf0) / 0x50 Get(ID3D11Resource**)
    void* fake = (void*)0x1234;
    Slot<void (*)(void*, const char*, void*)>(caps, 0x10)(caps, "Color", fake);
    void* got = nullptr;
    CHECK(Slot<int (*)(void*, const char*, void**)>(caps, 0x50)(caps, "Color", &got) == 1 && got == fake);
    unsigned missing = 7;
    CHECK(Slot<int (*)(void*, const char*, unsigned*)>(caps, 0x60)(caps, "DLSS.Hint.Render.Preset.DLAA", &missing) != 1);

    if (argc > 2)
    {
        // Call the patched NGXLoadCoreLibrary (RVA 0x229a0) inside the mod and
        // check it hands back NIER_FSR4.dll. DONT_RESOLVE_DLL_REFERENCES keeps
        // the mod's DllMain (hooks) from running; LoadLibraryW is still bound
        // by hand below because the IAT is not filled in that mode.
        HMODULE mod = LoadLibraryExA(argv[2], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        CHECK(mod != nullptr);
        if (mod)
        {
            char* base = (char*)mod;
            DWORD old;
            void** iat = (void**)(base + 0x4f020);
            VirtualProtect(iat, 8, PAGE_READWRITE, &old);
            *iat = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryW");
            VirtualProtect(base + 0x229a0, 64, PAGE_EXECUTE_READ, &old);
            SetDllDirectoryA(argc > 1 ? argv[1] : ".");
            HMODULE r = ((HMODULE (*)(void*))(base + 0x229a0))(nullptr);
            printf("     stub returned %p, NIER_FSR4.dll is %p\n", (void*)r, (void*)m);
            CHECK(r == m);
        }
    }
    printf(fails ? "SMOKE TEST FAILED (%d)\n" : "SMOKE TEST PASSED\n", fails);
    return fails ? 1 : 0;
}
