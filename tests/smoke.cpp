// Wine/Windows smoke test: drives NIER_FSR4.dll the way the mod's MSVC code
// does (raw vtable offsets), and checks the patched NGXLoadCoreLibrary stub.
//   smoke.exe <dir of smoke.exe, holding NIER_FSR4.dll> [patched mod dll]
// Under Wine pass the patched mod under a name other than d3d11.dll (or set
// WINEDLLOVERRIDES=d3d11=n,b), otherwise Wine loads its built-in d3d11.
#include <windows.h>
#include <cstdio>
#include <cstring>

typedef int (*PfnGetCaps)(void**);
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } else printf("ok   %s\n", #c); } while (0)

template <class F> F Slot(void* obj, int off) { return *(F*)(*(char**)obj + off); }

// Loads the patched mod normally (the Windows loader binds its IAT and runs
// its DllMain) before NIER_FSR4.dll is loaded, calls the patched
// NGXLoadCoreLibrary (RVA 0x229a0) and checks that it loaded NIER_FSR4.dll
// from `dir` through LoadLibraryW's normal search. Also checks that a stack
// walk stopped on the stub's jmp (RVA 0x229a7) unwinds to the caller.
static HMODULE StubTest(const char* modPath, const char* dir)
{
    HMODULE mod = LoadLibraryA(modPath);
    CHECK(mod != nullptr);
    if (!mod)
        return nullptr;
    char* base = (char*)mod;
    CHECK(GetModuleHandleA("NIER_FSR4.dll") == nullptr);
    bool bound = *(void**)(base + 0x4f020) == (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryW");
    CHECK(bound);
    if (!bound)
    {
        // e.g. Wine handed back its built-in d3d11.dll: pass the mod under
        // another name (or set WINEDLLOVERRIDES=d3d11=n,b)
        printf("     %s is not the patched mod, skipping the stub checks\n", modPath);
        return nullptr;
    }

    HMODULE r = ((HMODULE (*)(void*))(base + 0x229a0))((void*)0x1234);
    char got[MAX_PATH] = {}, want[MAX_PATH];
    if (r)
        GetModuleFileNameA(r, got, MAX_PATH);
    snprintf(want, sizeof want, "%s\\NIER_FSR4.dll", dir);
    printf("     stub loaded %s\n", got);
    CHECK(r != nullptr && lstrcmpiA(got, want) == 0);

    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry((DWORD64)(base + 0x229a7), &imageBase, nullptr);
    CHECK(fn != nullptr);
    if (fn)
    {
        // v1, no exception/unwind handler, no prolog, no unwind codes
        const BYTE* ui = (const BYTE*)(imageBase + fn->UnwindData);
        CHECK(ui[0] == 0x01 && ui[1] == 0 && ui[2] == 0 && ui[3] == 0);
        DWORD64 stack[4] = {0x1122334455667788ull, 0, 0, 0};
        CONTEXT c = {};
        c.Rip = (DWORD64)(base + 0x229a7);
        c.Rsp = (DWORD64)stack;
        void* handlerData = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &frame, nullptr);
        CHECK(c.Rip == 0x1122334455667788ull && c.Rsp == (DWORD64)stack + 8);

        // and from the first instruction (the lea)
        CONTEXT c0 = {};
        c0.Rip = (DWORD64)(base + 0x229a0);
        c0.Rsp = (DWORD64)stack;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c0.Rip, fn, &c0, &handlerData, &frame, nullptr);
        CHECK(c0.Rip == 0x1122334455667788ull && c0.Rsp == (DWORD64)stack + 8);
    }
    return r;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : ".";
    HMODULE fromStub = argc > 2 ? StubTest(argv[2], dir) : nullptr;

    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\NIER_FSR4.dll", dir);
    HMODULE m = LoadLibraryA(path);
    CHECK(m != nullptr);
    if (argc > 2)
        CHECK(fromStub == m);
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
    // int -1 read back as unsigned keeps its bit pattern, as in NGX
    Slot<void (*)(void*, const char*, int)>(caps, 0x18)(caps, "Sentinel", -1);
    unsigned bits = 0;
    CHECK(Slot<int (*)(void*, const char*, unsigned*)>(caps, 0x60)(caps, "Sentinel", &bits) == 1 && bits == 0xFFFFFFFFu);
    // a missing parameter reports NVSDK_NGX_Result_FAIL_UnsupportedParameter
    unsigned missing = 7;
    CHECK(Slot<int (*)(void*, const char*, unsigned*)>(caps, 0x60)(caps, "NotSet", &missing) == (int)0xBAD00010);
    CHECK(Slot<int (*)(void*, const char*, unsigned*)>(caps, 0x60)(caps, "DLSS.Hint.Render.Preset.DLAA", &missing) != 1);

    printf(fails ? "SMOKE TEST FAILED (%d)\n" : "SMOKE TEST PASSED\n", fails);
    return fails ? 1 : 0;
}
