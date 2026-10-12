// NIER_FSR4.dll - replaces the NVIDIA NGX core for the NIER_DLAA mod and runs
// AMD FSR 4.1.1 (FSR SDK 2.3.0 "Redstone" signed binaries) instead of DLSS.
//
// How it is wired in (see docs/REVERSE_ENGINEERING.md):
//   The mod's d3d11.dll statically links nvsdk_ngx_s.lib. Its NGX loader
//   (0x18001ffc0) gets the core module from NGXLoadCoreLibrary (0x1800229a0)
//   and resolves NVSDK_NGX_D3D11_* from it with GetProcAddress.
//   tools/patch_mod.py replaces NGXLoadCoreLibrary with
//   LoadLibraryW(L"NIER_FSR4.dll"), so every NGX call lands in the exports
//   below. That also skips NGX's NVIDIA-only adapter and signature checks.
//
// FSR 4 only has a DirectX 12 backend and the game is DirectX 11, so this DLL
// creates a D3D12 device on the same adapter and moves the frame across with
// shared textures and two shared fences ("ready" written by D3D11, "done"
// written by D3D12):
//   D3D11: copy colour/depth/motion vectors into shared textures, signal
//   D3D12: wait, FSR dispatch into a shared output texture, signal
//   D3D11: wait, copy the shared output into the mod's DLSS output target

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "ngx_params.h"

#include "../third_party/FidelityFX/api/include/ffx_api.h"
#include "../third_party/FidelityFX/api/include/ffx_api_types.h"
#include "../third_party/FidelityFX/api/include/dx12/ffx_api_dx12.h"
#include "../third_party/FidelityFX/upscalers/include/ffx_upscale.h"

#define NGX_API extern "C" __declspec(dllexport)

namespace
{

// ---------------------------------------------------------------------------
// Logging and configuration
// ---------------------------------------------------------------------------

std::wstring g_dir;
FILE* g_log;
std::mutex g_logLock;

void Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> g(g_logLock);
    if (!g_log)
    {
        g_log = _wfopen((g_dir + L"NIER_FSR4.log").c_str(), L"w");
        if (!g_log)
            return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

// For errors that can repeat every frame. Keyed by the formatted text: the
// first 5 of a burst are logged, then every 3600th. A message that has been
// quiet for 30 s starts a new burst.
struct RepeatEntry
{
    std::string text;
    unsigned count;
    ULONGLONG last;
};
std::mutex g_repeatLock;
std::vector<RepeatEntry> g_repeats;

void LogRepeat(const char* fmt, ...)
{
    char buf[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    unsigned n;
    {
        std::lock_guard<std::mutex> g(g_repeatLock);
        ULONGLONG now = GetTickCount64();
        auto it = std::find_if(g_repeats.begin(), g_repeats.end(), [&](const RepeatEntry& e) { return e.text == buf; });
        if (it == g_repeats.end())
        {
            if (g_repeats.size() >= 64)
                g_repeats.erase(std::min_element(g_repeats.begin(), g_repeats.end(),
                                                 [](const RepeatEntry& x, const RepeatEntry& y) { return x.last < y.last; }));
            it = g_repeats.insert(g_repeats.end(), {buf, 0u, now});
        }
        if (now - it->last > 30000)
            it->count = 0;
        it->last = now;
        n = ++it->count;
    }
    if (n > 5 && n % 3600 != 0)
        return;
    Log(n == 5 ? "%s (repeating, now logged every 3600th time)" : n > 5 ? "%s (x%u)" : "%s", buf, n);
}

struct Config
{
    std::string version = "4.1.1";  // preferred provider (substring), "auto" = SDK default
    bool sharpening = false;
    float sharpness = 0.0f;
    float jitterSignX = 1.0f;
    float jitterSignY = 1.0f;
    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;
    float cameraNear = 0.0f;  // 0 = read from the mod's camera tracking
    float cameraFar = 0.0f;
    float fovDegrees = 0.0f;
    float viewToMeters = 1.0f;
    int depthInverted = -1;  // -1 = from the DLSS create flags
    int depthInfinite = 0;
    int autoExposure = -1;
    int hdr = -1;
    bool debugView = false;
    bool debugChecking = false;
    bool readModCamera = true;
} g_cfg;

float IniFloat(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, float def)
{
    wchar_t buf[64];
    wchar_t defs[64];
    swprintf(defs, 64, L"%g", def);
    GetPrivateProfileStringW(sec, key, defs, buf, 64, ini);
    return (float)_wtof(buf);
}

void LoadConfig()
{
    std::wstring ini = g_dir + L"NIER_FSR4.ini";
    const wchar_t* f = ini.c_str();
    wchar_t buf[64];
    GetPrivateProfileStringW(L"FSR", L"Version", L"4.1.1", buf, 64, f);
    char nb[64] = {};
    if (WideCharToMultiByte(CP_UTF8, 0, buf, -1, nb, sizeof nb, nullptr, nullptr) > 0)
        g_cfg.version = nb;
    g_cfg.sharpening = GetPrivateProfileIntW(L"FSR", L"Sharpening", 0, f) != 0;
    g_cfg.sharpness = std::clamp(IniFloat(f, L"FSR", L"Sharpness", 0.0f), 0.0f, 1.0f);
    g_cfg.jitterSignX = IniFloat(f, L"FSR", L"JitterSignX", 1.0f) < 0 ? -1.0f : 1.0f;
    g_cfg.jitterSignY = IniFloat(f, L"FSR", L"JitterSignY", 1.0f) < 0 ? -1.0f : 1.0f;
    g_cfg.mvScaleX = IniFloat(f, L"FSR", L"MotionVectorScaleX", 1.0f);
    g_cfg.mvScaleY = IniFloat(f, L"FSR", L"MotionVectorScaleY", 1.0f);
    g_cfg.depthInverted = (int)GetPrivateProfileIntW(L"FSR", L"DepthInverted", -1, f);
    g_cfg.depthInfinite = (int)GetPrivateProfileIntW(L"FSR", L"DepthInfinite", 0, f);
    g_cfg.autoExposure = (int)GetPrivateProfileIntW(L"FSR", L"AutoExposure", -1, f);
    g_cfg.hdr = (int)GetPrivateProfileIntW(L"FSR", L"HDR", -1, f);
    g_cfg.debugView = GetPrivateProfileIntW(L"FSR", L"DebugView", 0, f) != 0;
    g_cfg.debugChecking = GetPrivateProfileIntW(L"FSR", L"DebugChecking", 0, f) != 0;
    g_cfg.readModCamera = GetPrivateProfileIntW(L"Camera", L"ReadFromMod", 1, f) != 0;
    g_cfg.cameraNear = IniFloat(f, L"Camera", L"Near", 0.0f);
    g_cfg.cameraFar = IniFloat(f, L"Camera", L"Far", 0.0f);
    g_cfg.fovDegrees = IniFloat(f, L"Camera", L"FovDegrees", 0.0f);
    g_cfg.viewToMeters = IniFloat(f, L"Camera", L"ViewSpaceToMeters", 1.0f);
    Log("config: Version=%s Sharpening=%d Sharpness=%.2f JitterSign=%+.0f/%+.0f MVScale=%.3f/%.3f "
        "DepthInverted=%d DepthInfinite=%d AutoExposure=%d HDR=%d DebugView=%d ReadFromMod=%d Near=%.4f Far=%.1f Fov=%.1f",
        g_cfg.version.c_str(), g_cfg.sharpening, g_cfg.sharpness, g_cfg.jitterSignX, g_cfg.jitterSignY, g_cfg.mvScaleX,
        g_cfg.mvScaleY, g_cfg.depthInverted, g_cfg.depthInfinite, g_cfg.autoExposure, g_cfg.hdr, g_cfg.debugView,
        g_cfg.readModCamera, g_cfg.cameraNear, g_cfg.cameraFar, g_cfg.fovDegrees);
}

// ---------------------------------------------------------------------------
// Camera values from the mod (NIER_DLAA 1.1, d3d11.dll sha256 f1c77c58...).
// NGX has no slots for near/far/FOV, but FSR wants them, so they are read
// from the mod's camera tracker. RVAs come from the code that logs
// "camera: depth constants near %.4f, far - near %.1f" (0x1800166ea/f2) and
// "camera: main camera found: P00 .. P11 .." (0x180015788).
// ---------------------------------------------------------------------------

constexpr uint32_t kRvaDepthNear = 0xabc1c;
constexpr uint32_t kRvaDepthRange = 0xabecc;
constexpr uint32_t kRvaProjP11 = 0xabe8c;
constexpr uint32_t kRvaProjP22 = 0xabea0;  // same projection copy (0x180015797)
constexpr uint32_t kRvaProjP23 = 0xabea4;
constexpr uint32_t kModSizeOfImage = 0xd1000;

const uint8_t* g_modBase;

// `caller` is a return address inside the module that called NGX init, i.e.
// the mod's statically linked NGX wrapper. The mod and the system d3d11.dll
// share a file name, so it is identified by address, size and a string.
const uint8_t* FindModBase(void* caller)
{
    HMODULE m = nullptr;
    if (!caller || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCWSTR)caller, &m))
        return nullptr;
    auto* b = (const uint8_t*)m;
    auto* nt = (const IMAGE_NT_HEADERS64*)(b + ((const IMAGE_DOS_HEADER*)m)->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage != kModSizeOfImage)
        return nullptr;
    const char tag[] = "NIER_DLAA proxy attached";
    for (uint32_t i = 0x4f000; i + sizeof(tag) < 0x6a000; ++i)
        if (memcmp(b + i, tag, sizeof(tag) - 1) == 0)
            return b;
    return nullptr;
}

struct CameraInfo
{
    float nearZ, farZ, fovY;
};

CameraInfo GetCamera()
{
    CameraInfo c{0.1f, 1000.0f, 0.8726646f};  // fallback: 50 degrees
    if (g_cfg.readModCamera && g_modBase)
    {
        float n, r, p11;
        memcpy(&n, g_modBase + kRvaDepthNear, 4);
        memcpy(&r, g_modBase + kRvaDepthRange, 4);
        memcpy(&p11, g_modBase + kRvaProjP11, 4);
        if (std::isfinite(n) && std::isfinite(r) && n > 0.0f && r > 0.0f)
        {
            c.nearZ = n;
            c.farZ = n + r;
        }
        else
        {
            // No depth constants accepted yet: derive them from the projection
            // the same way the mod does (0x180018ac4): near = P23 / P22,
            // far = P23 / (P22 + 1).
            float p22, p23;
            memcpy(&p22, g_modBase + kRvaProjP22, 4);
            memcpy(&p23, g_modBase + kRvaProjP23, 4);
            if (std::isfinite(p22) && std::isfinite(p23) && p22 != 0.0f && p22 + 1.0f != 0.0f)
            {
                float pn = p23 / p22, pf = p23 / (p22 + 1.0f);
                if (std::isfinite(pn) && std::isfinite(pf) && pn > 0.0f && pf > pn)
                {
                    c.nearZ = pn;
                    c.farZ = pf;
                }
            }
        }
        if (std::isfinite(p11) && std::fabs(p11) > 0.05f && std::fabs(p11) < 50.0f)
            c.fovY = 2.0f * std::atan(1.0f / std::fabs(p11));
    }
    if (g_cfg.cameraNear > 0.0f)
        c.nearZ = g_cfg.cameraNear;
    if (g_cfg.cameraFar > 0.0f)
        c.farZ = g_cfg.cameraFar;
    if (g_cfg.fovDegrees > 0.0f)
        c.fovY = g_cfg.fovDegrees * 3.14159265f / 180.0f;
    return c;
}

// ---------------------------------------------------------------------------
// FidelityFX API (loaded at runtime from amd_fidelityfx_loader_dx12.dll)
// ---------------------------------------------------------------------------

struct Ffx
{
    HMODULE dll = nullptr;
    PfnFfxCreateContext CreateContext = nullptr;
    PfnFfxDestroyContext DestroyContext = nullptr;
    PfnFfxQuery Query = nullptr;
    PfnFfxDispatch Dispatch = nullptr;

    bool Load()
    {
        if (dll)
            return true;
        std::wstring path = g_dir + L"amd_fidelityfx_loader_dx12.dll";
        dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll)
        {
            Log("FSR: could not load %ls (error %lu)", path.c_str(), GetLastError());
            return false;
        }
        CreateContext = (PfnFfxCreateContext)GetProcAddress(dll, "ffxCreateContext");
        DestroyContext = (PfnFfxDestroyContext)GetProcAddress(dll, "ffxDestroyContext");
        Query = (PfnFfxQuery)GetProcAddress(dll, "ffxQuery");
        Dispatch = (PfnFfxDispatch)GetProcAddress(dll, "ffxDispatch");
        if (!CreateContext || !DestroyContext || !Query || !Dispatch)
        {
            Log("FSR: the loader dll is missing exports");
            FreeLibrary(dll);
            dll = nullptr;
            return false;
        }
        return true;
    }
} g_ffx;

void FfxMessage(uint32_t type, const wchar_t* msg)
{
    Log("FSR %s: %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", msg);
}

// ---------------------------------------------------------------------------
// D3D11 <-> D3D12 bridge
// ---------------------------------------------------------------------------

template <class T>
void SafeRelease(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

DXGI_FORMAT TypedFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return f;
    }
}

bool CopyableFormat(DXGI_FORMAT f)
{
    // Depth formats with stencil cannot be copied into a colour texture.
    switch (f)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return false;
    default: return true;
    }
}

struct SharedTex
{
    ID3D12Resource* r12 = nullptr;
    ID3D11Texture2D* t11 = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool uav = false;

    void Release()
    {
        SafeRelease(t11);
        SafeRelease(r12);
        w = h = 0;
        fmt = DXGI_FORMAT_UNKNOWN;
    }
};

struct Bridge
{
    ID3D11Device* dev11 = nullptr;
    ID3D11Device5* dev11_5 = nullptr;
    ID3D11DeviceContext4* ctx11 = nullptr;
    // Two shared fences, each written by one device only, so neither can
    // ever be wound back by the other side's queued signals:
    //   ready: created on D3D11, signalled by D3D11, waited on by D3D12
    //   done:  created on D3D12, signalled by D3D12, waited on by D3D11 + CPU
    ID3D11Fence* readyFence11 = nullptr;
    ID3D12Fence* readyFence12 = nullptr;
    UINT64 readyValue = 0;
    ID3D11Fence* fence11 = nullptr;  // the done fence, D3D11 side

    ID3D12Device* dev12 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    static constexpr int kFrames = 3;
    ID3D12CommandAllocator* alloc[kFrames] = {};
    UINT64 allocFence[kFrames] = {};
    int frame = 0;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence12 = nullptr;  // the done fence
    UINT64 fenceValue = 0;           // last done value
    HANDLE fenceEvent = nullptr;

    SharedTex color, depth, mv, output;
    bool ready = false;
    bool lost = false;       // a device was removed or the GPU hung; stop using the bridge
    bool hung = false;       // lost without device removal: GPU work may still run, leak instead of release
    bool providerChosen = false;  // FSR provider query done for this device
    uint64_t provider = 0;        // version id to request, 0 = SDK default
    std::vector<uint64_t> providerIds;  // every provider for this device, newest first
    ULONGLONG failedAt = 0;  // last failed creation, retried after 10 s; cleared by NGX Shutdown

    bool Init(ID3D11Device* device);
    bool Ensure(SharedTex& s, ID3D11Resource* src, bool uav, const char* what);
    bool WaitCpu(UINT64 v);
    void MarkLost(const char* why, HRESULT hr);
    void Shutdown();

private:
    bool Create(ID3D11Device* device);
} g_bridge;

// Every failure path releases whatever was created, so `ready` is the only
// state the rest of the code has to look at.
// The caller (InitCommon) shuts the bridge down first if the device changed,
// after destroying the FSR context that lives on it.
bool Bridge::Init(ID3D11Device* device)
{
    if (ready)
        return true;
    if (failedAt && GetTickCount64() - failedAt < 10000)
        return false;
    if (Create(device))
    {
        ready = true;
        failedAt = 0;
        Log("bridge: D3D12 device ready");
        return true;
    }
    Shutdown();
    failedAt = GetTickCount64();
    return false;
}

bool Bridge::Create(ID3D11Device* device)
{
    dev11 = device;
    dev11->AddRef();
    if (FAILED(dev11->QueryInterface(__uuidof(ID3D11Device5), (void**)&dev11_5)))
    {
        Log("bridge: ID3D11Device5 not available (needs Windows 10 1703 or newer)");
        return false;
    }

    LUID luid{};
    {
        IDXGIDevice* dxgiDev = nullptr;
        IDXGIAdapter* adapter = nullptr;
        if (FAILED(dev11->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) ||
            FAILED(dxgiDev->GetAdapter(&adapter)))
        {
            SafeRelease(dxgiDev);
            Log("bridge: could not get the DXGI adapter of the game device");
            return false;
        }
        DXGI_ADAPTER_DESC d{};
        adapter->GetDesc(&d);
        luid = d.AdapterLuid;
        Log("bridge: game adapter %ls (vendor 0x%04X device 0x%04X)", d.Description, d.VendorId, d.DeviceId);
        SafeRelease(adapter);
        SafeRelease(dxgiDev);
    }

    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter12 = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory)) ||
        FAILED(factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), (void**)&adapter12)))
    {
        SafeRelease(factory);
        Log("bridge: could not find the game adapter through DXGI 1.4");
        return false;
    }
    SafeRelease(factory);

    using PfnCreate = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create = d3d12 ? (PfnCreate)GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr;
    HRESULT hr = create ? create(adapter12, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)&dev12) : E_FAIL;
    SafeRelease(adapter12);
    if (FAILED(hr))
    {
        Log("bridge: D3D12CreateDevice failed: 0x%08lX", (unsigned long)hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(hr = dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&queue)))
    {
        Log("bridge: CreateCommandQueue failed: 0x%08lX", (unsigned long)hr);
        return false;
    }
    queue->SetName(L"NIER_FSR4 queue");
    for (auto& a : alloc)
        if (FAILED(hr = dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                      (void**)&a)))
        {
            Log("bridge: CreateCommandAllocator failed: 0x%08lX", (unsigned long)hr);
            return false;
        }
    if (FAILED(hr = dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr,
                                             __uuidof(ID3D12GraphicsCommandList), (void**)&list)))
    {
        Log("bridge: CreateCommandList failed: 0x%08lX", (unsigned long)hr);
        return false;
    }
    list->Close();

    if (FAILED(hr = dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), (void**)&fence12)))
    {
        Log("bridge: CreateFence failed: 0x%08lX", (unsigned long)hr);
        return false;
    }
    HANDLE h = nullptr;
    if (FAILED(hr = dev12->CreateSharedHandle(fence12, nullptr, GENERIC_ALL, nullptr, &h)))
    {
        Log("bridge: sharing the fence failed: 0x%08lX", (unsigned long)hr);
        return false;
    }
    hr = dev11_5->OpenSharedFence(h, __uuidof(ID3D11Fence), (void**)&fence11);
    CloseHandle(h);
    if (FAILED(hr))
    {
        Log("bridge: OpenSharedFence failed: 0x%08lX", (unsigned long)hr);
        return false;
    }
    if (FAILED(hr = dev11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void**)&readyFence11)))
    {
        Log("bridge: creating the D3D11 fence failed: 0x%08lX", (unsigned long)hr);
        return false;
    }
    h = nullptr;
    if (FAILED(hr = readyFence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)))
    {
        Log("bridge: sharing the D3D11 fence failed: 0x%08lX", (unsigned long)hr);
        return false;
    }
    hr = dev12->OpenSharedHandle(h, __uuidof(ID3D12Fence), (void**)&readyFence12);
    CloseHandle(h);
    if (FAILED(hr))
    {
        Log("bridge: opening the D3D11 fence in D3D12 failed: 0x%08lX", (unsigned long)hr);
        return false;
    }
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent)
    {
        Log("bridge: CreateEvent failed: %lu", GetLastError());
        return false;
    }

    ID3D11DeviceContext* imm = nullptr;
    dev11->GetImmediateContext(&imm);
    hr = imm->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&ctx11);
    SafeRelease(imm);
    if (FAILED(hr))
    {
        Log("bridge: ID3D11DeviceContext4 not available: 0x%08lX", (unsigned long)hr);
        return false;
    }

    return true;
}

// Blocks until the D3D12 queue has passed `v`. Returns false, and marks the
// bridge lost, if either device was removed or the GPU makes no progress for
// 10 s. Callers must then not reset or release anything the GPU may use.
bool Bridge::WaitCpu(UINT64 v)
{
    if (!fence12 || !fenceEvent)
        return true;
    if (lost)
        return false;
    ULONGLONG start = GetTickCount64();
    for (;;)
    {
        UINT64 done = fence12->GetCompletedValue();
        HRESULT removed12 = dev12->GetDeviceRemovedReason();
        HRESULT removed11 = dev11->GetDeviceRemovedReason();
        if (done == UINT64_MAX || FAILED(removed12) || FAILED(removed11))
        {
            // Only a removed D3D12 device guarantees nothing of ours still
            // runs; with just the game's device removed, D3D12 work may still
            // wait on it, so MarkLost leaves it as hung (leaked).
            MarkLost(FAILED(removed12) ? "D3D12 device removed" : FAILED(removed11) ? "the game's D3D11 device was removed"
                                                                                      : "the done fence reports removal",
                     FAILED(removed12) ? removed12 : removed11);
            return false;
        }
        if (done >= v)
            return true;
        if (GetTickCount64() - start > 10000)
        {
            MarkLost("the GPU made no progress for 10 s", S_OK);
            return false;
        }
        if (SUCCEEDED(fence12->SetEventOnCompletion(v, fenceEvent)))
            WaitForSingleObject(fenceEvent, 1000);
        else
            Sleep(1);  // poll instead
    }
}

bool Bridge::Ensure(SharedTex& s, ID3D11Resource* src, bool uav, const char* what)
{
    ID3D11Texture2D* tex = nullptr;
    if (!src || FAILED(src->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)))
    {
        LogRepeat("bridge: %s is not a 2D texture", what);
        return false;
    }
    D3D11_TEXTURE2D_DESC sd{};
    tex->GetDesc(&sd);
    tex->Release();

    DXGI_FORMAT fmt = TypedFormat(sd.Format);
    if (s.t11 && s.w == sd.Width && s.h == sd.Height && s.fmt == fmt && s.uav == uav)
        return true;

    // sRGB cannot be a UAV, and the mod only uses R11G11B10F / RGBA16F anyway.
    bool srgbOutput = uav && (fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    // Mips and array slices are fine: only subresource 0 is copied.
    if (!CopyableFormat(sd.Format) || sd.SampleDesc.Count != 1 || srgbOutput)
    {
        LogRepeat("bridge: %s has an unsupported layout (format %d, msaa %u)", what, sd.Format, sd.SampleDesc.Count);
        return false;
    }

    // Anything still in flight may reference the old texture.
    if (!WaitCpu(fenceValue))
        return false;
    s.Release();

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = sd.Width;
    rd.Height = sd.Height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // D3D11 refuses (E_INVALIDARG) to open a shared texture that has neither a
    // render target nor a UAV usage, so add render target where the format allows.
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{fmt};
    bool rt = SUCCEEDED(dev12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) &&
              (fs.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
    D3D12_RESOURCE_FLAGS base = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    if (rt)
        base |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    // Some drivers also want simultaneous access for cross-API sharing; try that second.
    const D3D12_RESOURCE_FLAGS attempts[] = {base, base | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS};
    HRESULT hr = E_FAIL;
    for (D3D12_RESOURCE_FLAGS flags : attempts)
    {
        s.Release();
        rd.Flags = flags;
        hr = dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            __uuidof(ID3D12Resource), (void**)&s.r12);
        if (FAILED(hr))
        {
            LogRepeat("bridge: creating the shared %s texture %ux%u fmt %d flags 0x%x failed: 0x%08lX", what, sd.Width,
                sd.Height, fmt, (unsigned)flags, (unsigned long)hr);
            continue;
        }
        HANDLE h = nullptr;
        hr = dev12->CreateSharedHandle(s.r12, nullptr, GENERIC_ALL, nullptr, &h);
        if (SUCCEEDED(hr))
        {
            hr = dev11_5->OpenSharedResource1(h, __uuidof(ID3D11Texture2D), (void**)&s.t11);
            CloseHandle(h);
        }
        if (SUCCEEDED(hr))
            break;
        LogRepeat("bridge: opening the shared %s texture (flags 0x%x) in D3D11 failed: 0x%08lX", what, (unsigned)flags,
            (unsigned long)hr);
    }
    if (FAILED(hr))
    {
        s.Release();
        return false;
    }
    s.w = sd.Width;
    s.h = sd.Height;
    s.fmt = fmt;
    s.uav = uav;
    Log("bridge: shared %s texture %ux%u, game format %d, shared format %d, flags 0x%x", what, s.w, s.h, sd.Format, fmt,
        (unsigned)rd.Flags);
    return true;
}

// Turns FSR off after a failed or stuck GPU call. Every early return in
// Evaluate leaves the queued Signal/Wait pairs balanced, so nothing is forced
// here (forcing the shared fence from the CPU would be overwritten by signals
// already queued and could block the game's context). Resources may only be
// released later if the D3D12 device itself is gone; otherwise work may still
// be queued on it and everything is leaked instead.
void Bridge::MarkLost(const char* why, HRESULT hr)
{
    lost = true;
    hung = !(dev12 && FAILED(dev12->GetDeviceRemovedReason()));
    Log("bridge: %s (0x%08lX), FSR is off%s", why, (unsigned long)hr,
        hung ? "" : " (D3D12 device removed)");
}

void Bridge::Shutdown()
{
    // After a hang the GPU may still run work that uses these objects: leak
    // them instead of releasing them under it. After a device removal nothing
    // runs any more, so they are released normally.
    if (!WaitCpu(fenceValue) && hung)
    {
        Log("bridge: shutting down without releasing GPU objects (GPU hung)");
        *this = Bridge{};
        return;
    }
    ready = false;
    color.Release();
    depth.Release();
    mv.Release();
    output.Release();
    SafeRelease(list);
    for (auto& a : alloc)
        SafeRelease(a);
    SafeRelease(queue);
    SafeRelease(fence11);
    SafeRelease(fence12);
    SafeRelease(readyFence12);
    SafeRelease(readyFence11);
    SafeRelease(ctx11);
    SafeRelease(dev12);
    SafeRelease(dev11_5);
    SafeRelease(dev11);
    if (fenceEvent)
        CloseHandle(fenceEvent);
    fenceEvent = nullptr;
    fenceValue = 0;
    readyValue = 0;
    frame = 0;
    for (auto& f : allocFence)
        f = 0;
    lost = false;
    hung = false;
    providerChosen = false;
    provider = 0;
    providerIds.clear();
}

// ---------------------------------------------------------------------------
// FSR feature (what NGX calls a DLSS feature handle)
// ---------------------------------------------------------------------------

// NGX create flags (nvsdk_ngx_defs.h, NVSDK_NGX_DLSS_Feature_Flags)
constexpr int kDlssIsHDR = 1 << 0;
constexpr int kDlssMVLowRes = 1 << 1;
constexpr int kDlssMVJittered = 1 << 2;
constexpr int kDlssDepthInverted = 1 << 3;
constexpr int kDlssAutoExposure = 1 << 6;

struct NVSDK_NGX_Handle
{
    unsigned int Id;
};

struct Feature
{
    // Allocated per feature and never freed (4 bytes each), so a stale handle
    // can never compare equal to a later feature's.
    NVSDK_NGX_Handle* handle = new NVSDK_NGX_Handle{};
    ~Feature()
    {
        if (handle && handle->Id == 0)  // never handed out, safe to free
            delete handle;
    }
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
    int dlssFlags = 0;
    ffxContext ctx = nullptr;
    uint64_t frames = 0;
    LARGE_INTEGER last{};
    uint32_t createW = 0, createH = 0;  // render size requested by CreateFeature
    bool createFailed = false;          // CreateContext failed for failedSizes
    uint32_t failedSizes[4] = {};

    bool CreateContext();
    void DestroyContext();
};

std::mutex g_lock;
Feature* g_feature;

struct CreateRequest
{
    uint32_t w = 0, h = 0, outW = 0, outH = 0;
    int flags = 0;
    bool operator==(const CreateRequest& o) const
    {
        return w == o.w && h == o.h && outW == o.outW && outH == o.outH && flags == o.flags;
    }
};

// The last CreateFeature that failed; cleared on success and on a bridge rebuild.
struct FailedCreate : CreateRequest
{
    bool valid = false;
    NVSDK_NGX_Result result = NGX_Fail;
    NVSDK_NGX_Result Remember(const CreateRequest& r, NVSDK_NGX_Result res)
    {
        static_cast<CreateRequest&>(*this) = r;
        valid = true;
        result = res;
        return res;
    }
    bool operator==(const CreateRequest& o) const { return CreateRequest::operator==(o); }
} g_failedCreate;

void DestroyFeature()
{
    if (g_feature)
    {
        g_feature->DestroyContext();
        delete g_feature;
        g_feature = nullptr;
    }
}
unsigned int g_nextHandle = 1;
NgxParams* g_capParams;

std::string Lower(std::string s)
{
    for (auto& c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

bool Feature::CreateContext()
{
    if (ctx)
        return true;
    if (g_bridge.lost || !g_bridge.ready)
        return false;
    if (!g_ffx.Load())
        return false;

    uint32_t f = 0;
    bool hdr = g_cfg.hdr >= 0 ? g_cfg.hdr != 0 : (dlssFlags & kDlssIsHDR) != 0;
    bool inverted = g_cfg.depthInverted >= 0 ? g_cfg.depthInverted != 0 : (dlssFlags & kDlssDepthInverted) != 0;
    bool autoExp = g_cfg.autoExposure >= 0 ? g_cfg.autoExposure != 0 : (dlssFlags & kDlssAutoExposure) != 0;
    if (hdr)
        f |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (!(dlssFlags & kDlssMVLowRes))
        f |= FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    if (dlssFlags & kDlssMVJittered)
        f |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    if (inverted)
        f |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (g_cfg.depthInfinite > 0)  // only 1 turns it on; 0 and -1 mean off
        f |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (autoExp)
        f |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (g_cfg.debugChecking)
        f |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    if (g_cfg.debugView)
        f |= FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;

    // Pick the provider. FSR 4.x needs a Radeon RX 7000 / 9000 GPU; on other
    // hardware the SDK only offers FSR 3.1.x.
    if (!g_bridge.providerChosen)
    {
        uint64_t count = 0;
        ffxQueryDescGetVersions q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device = g_bridge.dev12;
        q.outputCount = &count;
        g_ffx.Query(nullptr, &q.header);
        std::vector<uint64_t> ids(count);
        std::vector<const char*> names(count);
        q.versionIds = ids.data();
        q.versionNames = names.data();
        if (count && g_ffx.Query(nullptr, &q.header) == FFX_API_RETURN_OK)
        {
            g_bridge.providerChosen = true;
            g_bridge.providerIds.assign(ids.begin(), ids.begin() + count);
            std::string want = Lower(g_cfg.version);
            for (uint64_t i = 0; i < count; ++i)
            {
                Log("FSR: provider available: %s (id 0x%llX)", names[i] ? names[i] : "?", (unsigned long long)ids[i]);
                if (!g_bridge.provider && want != "auto" && !want.empty() && names[i] &&
                    Lower(names[i]).find(want) != std::string::npos)
                    g_bridge.provider = ids[i];
            }
            if (!g_bridge.provider && want != "auto")
                Log("FSR: no provider matches Version=%s, trying the available ones newest first (FSR 4 needs an RX 7000/9000 GPU)",
                    g_cfg.version.c_str());
        }
    }
    // Providers to try in order: the requested one, then every other one
    // newest first (e.g. 4.1.1 -> 3.1.5 -> 2.3.4). The SDK does not fall back
    // by itself, and without an override it would pick the newest provider
    // again, so each fallback is requested explicitly. 0 = SDK default, used
    // only when the version query gave nothing.
    std::vector<uint64_t> candidates;
    if (g_bridge.provider)
        candidates.push_back(g_bridge.provider);
    for (uint64_t id : g_bridge.providerIds)
        if (id != g_bridge.provider)
            candidates.push_back(id);
    if (candidates.empty())
        candidates.push_back(0);

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = g_bridge.dev12;

    ffxOverrideVersion over{};
    over.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;

    ffxCreateContextDescUpscaleVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version = FFX_UPSCALER_VERSION;
    ver.header.pNext = &backend.header;

    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &ver.header;
    desc.flags = f;
    desc.maxRenderSize = {renderW, renderH};
    desc.maxUpscaleSize = {outW, outH};
    desc.fpMessage = &FfxMessage;

    ffxReturnCode_t rc = FFX_API_RETURN_ERROR;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        over.versionId = candidates[i];
        backend.header.pNext = candidates[i] ? &over.header : nullptr;
        ctx = nullptr;
        rc = g_ffx.CreateContext(&ctx, &desc.header, nullptr);
        if (rc == FFX_API_RETURN_OK)
            break;
        // Each failed attempt leaks some memory inside the SDK. Out of memory
        // or a removed device will fail on every provider, so stop there.
        if (rc == FFX_API_RETURN_ERROR_MEMORY || FAILED(g_bridge.dev12->GetDeviceRemovedReason()))
            break;
        if (i + 1 < candidates.size())
            LogRepeat("FSR: provider 0x%llX failed (%u), trying the next one", (unsigned long long)candidates[i], rc);
    }
    if (rc != FFX_API_RETURN_OK)
    {
        LogRepeat("FSR: ffxCreateContext failed: %u (render %ux%u, output %ux%u)", rc, renderW, renderH, outW, outH);
        ctx = nullptr;
        createFailed = true;
        failedSizes[0] = renderW;
        failedSizes[1] = renderH;
        failedSizes[2] = outW;
        failedSizes[3] = outH;
        return false;
    }

    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    g_ffx.Query(&ctx, &pv.header);
    Log("FSR: context created, render %ux%u -> %ux%u, flags 0x%X (DLSS flags 0x%X), provider %s", renderW, renderH, outW,
        outH, f, dlssFlags, pv.versionName ? pv.versionName : "?");
    return true;
}

void Feature::DestroyContext()
{
    if (ctx)
    {
        // Leak the context only after a hang, when the GPU may still use its
        // resources. After a device removal nothing runs any more.
        bool idle = g_bridge.WaitCpu(g_bridge.fenceValue);
        if (idle || !g_bridge.hung)
            g_ffx.DestroyContext(&ctx, nullptr);
        ctx = nullptr;
    }
}

FfxApiResource FfxRes(const SharedTex& s, bool uav)
{
    FfxApiResource r{};
    r.resource = s.r12;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffxApiGetSurfaceFormatDX12(s.fmt);
    r.description.width = s.w;
    r.description.height = s.h;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = uav ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    r.state = FFX_API_RESOURCE_STATE_COMMON;
    return r;
}

NVSDK_NGX_Result Evaluate(ID3D11DeviceContext* ctx, Feature& ft, const NgxParams& p)
{
    auto* color = (ID3D11Resource*)p.P("Color");
    auto* output = (ID3D11Resource*)p.P("Output");
    auto* depth = (ID3D11Resource*)p.P("Depth");
    auto* mvs = (ID3D11Resource*)p.P("MotionVectors");
    if (!color || !output || !depth || !mvs)
    {
        LogRepeat("evaluate: missing input (color %p output %p depth %p mv %p)", color, output, depth, mvs);
        return NGX_FailInvalidParameter;
    }
    if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        LogRepeat("evaluate: called on a deferred context, not supported");
        return NGX_FailInvalidParameter;
    }

    Bridge& b = g_bridge;
    if (!b.ready || b.lost)
        return NGX_FailNotInitialized;
    {
        ID3D11Device* owner = nullptr;
        ctx->GetDevice(&owner);
        bool same = owner == b.dev11;
        SafeRelease(owner);
        if (!same)
        {
            LogRepeat("evaluate: the context belongs to a different D3D11 device than NGX Init was given");
            return NGX_FailNotInitialized;
        }
    }
    if (!b.Ensure(b.color, color, false, "colour") || !b.Ensure(b.depth, depth, false, "depth") ||
        !b.Ensure(b.mv, mvs, false, "motion vector") || !b.Ensure(b.output, output, true, "output"))
        return NGX_Fail;

    uint32_t rw = (uint32_t)p.I64("DLSS.Render.Subrect.Dimensions.Width", 0);
    uint32_t rh = (uint32_t)p.I64("DLSS.Render.Subrect.Dimensions.Height", 0);
    if (!rw || !rh)
    {
        rw = ft.renderW;
        rh = ft.renderH;
    }
    rw = std::min(rw, b.color.w);
    rh = std::min(rh, b.color.h);

    // The FSR context is sized at creation. If the textures outgrew it (or the
    // output size changed without a new CreateFeature), rebuild it.
    bool resized = rw > ft.renderW || rh > ft.renderH || b.output.w != ft.outW || b.output.h != ft.outH;
    if (!ft.ctx || resized)
    {
        uint32_t want[4] = {std::max(ft.createW, rw), std::max(ft.createH, rh), b.output.w, b.output.h};
        if (!ft.ctx && ft.createFailed && memcmp(want, ft.failedSizes, sizeof want) == 0)
            return NGX_Fail;  // already failed at these sizes; retry only when something changes
        if (ft.ctx)
            Log("evaluate: size changed (render %ux%u, output %ux%u), rebuilding the FSR context", rw, rh, b.output.w,
                b.output.h);
        ft.DestroyContext();
        ft.renderW = want[0];
        ft.renderH = want[1];
        ft.outW = want[2];
        ft.outH = want[3];
        ft.frames = 0;
        ft.createFailed = false;
        if (!ft.CreateContext())
            return NGX_Fail;
    }

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dtMs = p.F32("FrameTimeDeltaInMsec", 0.0f);
    if (dtMs <= 0.0f)
        dtMs = ft.last.QuadPart ? (float)((now.QuadPart - ft.last.QuadPart) * 1000.0 / freq.QuadPart) : 16.6f;
    dtMs = std::clamp(dtMs, 0.1f, 200.0f);
    ft.last = now;

    // 1. D3D11 -> shared textures
    // Subresource 0 only: the shared textures have one mip and one slice.
    ctx->CopySubresourceRegion(b.color.t11, 0, 0, 0, 0, color, 0, nullptr);
    ctx->CopySubresourceRegion(b.depth.t11, 0, 0, 0, 0, depth, 0, nullptr);
    ctx->CopySubresourceRegion(b.mv.t11, 0, 0, 0, 0, mvs, 0, nullptr);
    UINT64 ready = ++b.readyValue;
    HRESULT hr = b.ctx11->Signal(b.readyFence11, ready);
    if (FAILED(hr))
    {
        b.MarkLost("D3D11 fence signal failed", hr);
        return NGX_Fail;
    }
    ctx->Flush();

    // 2. FSR on the D3D12 queue
    int slot = b.frame;
    b.frame = (b.frame + 1) % Bridge::kFrames;
    if (!b.WaitCpu(b.allocFence[slot]))
    {
        // Leave D3D11 unblocked: nothing will signal `ready` from D3D12 now.
        return NGX_Fail;
    }
    if (FAILED(hr = b.alloc[slot]->Reset()) || FAILED(hr = b.list->Reset(b.alloc[slot], nullptr)))
    {
        b.MarkLost("command list reset failed", hr);
        return NGX_Fail;
    }

    CameraInfo cam = GetCamera();
    float pre = p.F32("DLSS.Pre.Exposure", 0.0f);

    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = b.list;
    d.color = FfxRes(b.color, false);
    d.depth = FfxRes(b.depth, false);
    d.motionVectors = FfxRes(b.mv, false);
    d.output = FfxRes(b.output, true);
    d.jitterOffset.x = p.F32("Jitter.Offset.X", 0.0f) * g_cfg.jitterSignX;
    d.jitterOffset.y = p.F32("Jitter.Offset.Y", 0.0f) * g_cfg.jitterSignY;
    d.motionVectorScale.x = p.F32("MV.Scale.X", 1.0f) * g_cfg.mvScaleX;
    d.motionVectorScale.y = p.F32("MV.Scale.Y", 1.0f) * g_cfg.mvScaleY;
    d.renderSize = {rw, rh};
    d.upscaleSize = {b.output.w, b.output.h};
    d.enableSharpening = g_cfg.sharpening;
    d.sharpness = g_cfg.sharpness;
    d.frameTimeDelta = dtMs;
    d.preExposure = pre > 0.0f ? pre : 1.0f;
    d.reset = p.I64("Reset", 0) != 0 || ft.frames == 0;
    d.cameraNear = cam.nearZ;
    d.cameraFar = cam.farZ;
    d.cameraFovAngleVertical = cam.fovY;
    d.viewSpaceToMetersFactor = g_cfg.viewToMeters;
    d.flags = g_cfg.debugView ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0;

    if (FAILED(hr = b.queue->Wait(b.readyFence12, ready)))
    {
        b.list->Close();
        b.MarkLost("D3D12 queue wait failed", hr);
        return NGX_Fail;
    }
    ffxReturnCode_t rc = g_ffx.Dispatch(&ft.ctx, &d.header);
    HRESULT closed = b.list->Close();
    if (rc == FFX_API_RETURN_OK && SUCCEEDED(closed))
    {
        ID3D12CommandList* lists[] = {b.list};
        b.queue->ExecuteCommandLists(1, lists);
    }
    UINT64 done = ++b.fenceValue;
    b.allocFence[slot] = done;
    if (FAILED(hr = b.queue->Signal(b.fence12, done)))
    {
        b.MarkLost("D3D12 queue signal failed", hr);
        return NGX_Fail;
    }

    // 3. shared output -> D3D11
    if (FAILED(hr = b.ctx11->Wait(b.fence11, done)))
    {
        b.MarkLost("D3D11 fence wait failed", hr);
        return NGX_Fail;
    }
    if (FAILED(closed))
    {
        b.MarkLost("closing the command list failed", closed);
        return NGX_Fail;
    }
    if (rc != FFX_API_RETURN_OK)
    {
        LogRepeat("FSR: ffxDispatch failed: %u", rc);
        return NGX_Fail;
    }
    ctx->CopySubresourceRegion(output, 0, 0, 0, 0, b.output.t11, 0, nullptr);

    if (ft.frames % 3600 == 0)
    {
        Log("evaluate: frame %llu, render %ux%u -> %ux%u, jitter %.3f %.3f, mv scale %.2f %.2f, reset %d, "
            "near %.4f far %.1f fov %.1f deg, dt %.2f ms",
            (unsigned long long)ft.frames, rw, rh, b.output.w, b.output.h, d.jitterOffset.x, d.jitterOffset.y,
            d.motionVectorScale.x, d.motionVectorScale.y, d.reset, cam.nearZ, cam.farZ, cam.fovY * 57.29578f, dtMs);
    }
    ++ft.frames;
    return NGX_Success;
}

NVSDK_NGX_Result InitCommon(ID3D11Device* device, const char* who, void* caller)
{
    std::lock_guard<std::mutex> g(g_lock);
    Log("%s: device %p", who, device);
    if (!device)
        return NGX_FailInvalidParameter;
    g_modBase = FindModBase(caller);
    if (g_bridge.ready && (g_bridge.dev11 != device || g_bridge.lost))
    {
        Log("bridge: %s, rebuilding", g_bridge.lost ? "the previous device was lost" : "the game device changed");
        DestroyFeature();  // its FSR context lives on the old D3D12 device
        g_bridge.Shutdown();
    }
    g_failedCreate.valid = false;
    Log("mod camera data: %s", g_modBase ? "found (NIER_DLAA 1.1)" : "not found, using [Camera] values");
    // Load FFX first so a missing FSR dll does not leave a D3D12 device behind.
    if (!g_ffx.Load())
        return NGX_FailFeatureNotSupported;
    if (!g_bridge.Init(device))
        return NGX_FailFeatureNotSupported;
    return NGX_Success;
}

void FillCapabilities(NgxParams* p)
{
    p->SetI32("SuperSampling.Available", 1);
    p->SetI32("SuperSampling.NeedsUpdatedDriver", 0);
    p->SetI32("SuperSampling.MinDriverVersionMajor", 0);
    p->SetI32("SuperSampling.MinDriverVersionMinor", 0);
    p->SetI32("SuperSampling.FeatureInitResult", NGX_Success);
    p->SetI32("SuperSamplingDenoising.Available", 0);
    p->SetI32("FrameGeneration.Available", 0);
}

}  // namespace

// ---------------------------------------------------------------------------
// NGX core exports. Signatures follow how the mod's statically linked NGX
// wrappers call the core (see docs/REVERSE_ENGINEERING.md).
// ---------------------------------------------------------------------------

NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Init(unsigned long long appId, const wchar_t* path, ID3D11Device* device,
                                              const void* featureInfo, int sdkVersion)
{
    return InitCommon(device, "NVSDK_NGX_D3D11_Init", __builtin_return_address(0));
}

NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Init_Ext(unsigned long long appId, const wchar_t* path, ID3D11Device* device,
                                                  int sdkVersion, const void* featureInfo)
{
    return InitCommon(device, "NVSDK_NGX_D3D11_Init_Ext", __builtin_return_address(0));
}

NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Init_ProjectID(const char* projectId, int engineType, const char* engineVersion,
                                                        const wchar_t* path, ID3D11Device* device, int sdkVersion,
                                                        const void* featureInfo)
{
    Log("NVSDK_NGX_D3D11_Init_ProjectID: project %s, sdk 0x%X", projectId ? projectId : "?", sdkVersion);
    return InitCommon(device, "NVSDK_NGX_D3D11_Init_ProjectID", __builtin_return_address(0));
}

NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Shutdown()
{
    std::lock_guard<std::mutex> g(g_lock);
    Log("shutdown");
    DestroyFeature();
    g_bridge.Shutdown();
    g_bridge.failedAt = 0;
    g_failedCreate.valid = false;
    return NGX_Success;
}

NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Shutdown1(ID3D11Device*)
{
    return NVSDK_NGX_D3D11_Shutdown();
}

NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_GetCapabilityParameters(NgxParams** out)
{
    if (!out)
        return NGX_FailInvalidParameter;
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_capParams)
        g_capParams = new NgxParams();
    FillCapabilities(g_capParams);
    *out = g_capParams;
    return NGX_Success;
}

NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_GetParameters(NgxParams** out)
{
    return NVSDK_NGX_D3D11_GetCapabilityParameters(out);
}

NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_AllocateParameters(NgxParams** out)
{
    if (!out)
        return NGX_FailInvalidParameter;
    *out = new NgxParams();
    return NGX_Success;
}

NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_DestroyParameters(NgxParams* p)
{
    std::lock_guard<std::mutex> g(g_lock);
    if (p && p != g_capParams)
        delete p;
    return NGX_Success;
}

NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_GetScratchBufferSize(int, const NgxParams*, size_t* size)
{
    if (size)
        *size = 0;
    return NGX_Success;
}

NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_GetFeatureRequirements(void*, const void*, void*)
{
    return NGX_FailFeatureNotSupported;
}

NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_CreateFeature(ID3D11DeviceContext* ctx, int feature, NgxParams* p,
                                                       NVSDK_NGX_Handle** handle)
{
    if (!p || !handle)
        return NGX_FailInvalidParameter;
    std::lock_guard<std::mutex> g(g_lock);
    if (feature != 1)  // NVSDK_NGX_Feature_SuperSampling
    {
        Log("CreateFeature: feature %d is not supported", feature);
        return NGX_FailFeatureNotSupported;
    }
    if (!g_bridge.ready || g_bridge.lost)
        return NGX_FailNotInitialized;

    CreateRequest req;
    req.w = (uint32_t)p->I64("Width", 0);
    req.h = (uint32_t)p->I64("Height", 0);
    req.outW = (uint32_t)p->I64("OutWidth", req.w);
    req.outH = (uint32_t)p->I64("OutHeight", req.h);
    req.flags = (int)p->I64("DLSS.Feature.Create.Flags", 0);
    // After a failed CreateFeature the mod asks again on every frame
    // (0x18001882b clears its handle, 0x1800186f9 recreates). Answer an
    // unchanged request from memory instead of retrying FSR each time.
    if (g_failedCreate.valid && g_failedCreate == req)
        return g_failedCreate.result;
    Log("CreateFeature: %ux%u -> %ux%u, quality %d, DLSS flags 0x%X", req.w, req.h, req.outW, req.outH,
        (int)p->I64("PerfQualityValue", -1), req.flags);
    if (!req.w || !req.h || !req.outW || !req.outH)
        return g_failedCreate.Remember(req, NGX_FailInvalidParameter);

    // Free the previous context first so two sets of FSR resources never
    // coexist. The mod releases its old handle before every CreateFeature
    // (0x18001871b), so nothing still uses the feature dropped here.
    DestroyFeature();
    auto* ft = new Feature();
    ft->renderW = ft->createW = req.w;
    ft->renderH = ft->createH = req.h;
    ft->outW = req.outW;
    ft->outH = req.outH;
    ft->dlssFlags = req.flags;
    if (!ft->CreateContext())
    {
        delete ft;  // its handle was never handed out
        return g_failedCreate.Remember(req, NGX_Fail);
    }
    g_failedCreate.valid = false;
    ft->handle->Id = g_nextHandle++;
    g_feature = ft;
    *handle = ft->handle;
    return NGX_Success;
}

NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext* ctx, const NVSDK_NGX_Handle* handle,
                                                         const NgxParams* p, void* /*progressCallback*/)
{
    if (!ctx || !handle || !p)
        return NGX_FailInvalidParameter;
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_feature || g_feature->handle != handle)
        return NGX_FailInvalidParameter;
    return Evaluate(ctx, *g_feature, *p);
}

NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_ReleaseFeature(NVSDK_NGX_Handle* handle)
{
    std::lock_guard<std::mutex> g(g_lock);
    if (g_feature && g_feature->handle == handle)
    {
        Log("ReleaseFeature: %u", handle->Id);
        DestroyFeature();
    }
    return NGX_Success;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(inst);
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(inst, path, MAX_PATH);
        std::wstring p(path, n);
        g_dir = p.substr(0, p.find_last_of(L"\\/") + 1);
        Log("NIER_FSR4 loaded from %ls (FSR SDK 2.3.0 / FSR %d.%d.%d API)", path, FFX_UPSCALER_VERSION_MAJOR,
            FFX_UPSCALER_VERSION_MINOR, FFX_UPSCALER_VERSION_PATCH);
        LoadConfig();
    }
    return TRUE;
}
