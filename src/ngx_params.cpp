#include "ngx_params.h"


// Each entry is a plain function taking `this` first, which matches the
// Windows x64 calling convention for a virtual member function (this in rcx,
// name in rdx, value in r8 or xmm2).
struct NgxParamsVtbl
{
    static void SetPtr(NgxParams* p, const char* n, void* v) { p->SetPtr(n, v, NgxParams::Type::Ptr); }
    static void SetD3D12(NgxParams* p, const char* n, ID3D12Resource* v) { p->SetPtr(n, v, NgxParams::Type::D3D12); }
    static void SetD3D11(NgxParams* p, const char* n, ID3D11Resource* v) { p->SetPtr(n, v, NgxParams::Type::D3D11); }
    static void SetI32(NgxParams* p, const char* n, int v) { p->SetI32(n, v); }
    static void SetU32(NgxParams* p, const char* n, unsigned int v) { p->SetU32(n, v); }
    static void SetF64(NgxParams* p, const char* n, double v) { p->SetF64(n, v); }
    static void SetF32(NgxParams* p, const char* n, float v) { p->SetF32(n, v); }
    static void SetU64(NgxParams* p, const char* n, unsigned long long v) { p->SetU64(n, v); }

    template <class T>
    static NVSDK_NGX_Result GetPtrT(const NgxParams* p, const char* n, T** out)
    {
        if (!out)
            return NGX_FailInvalidParameter;
        void* v = nullptr;
        if (!p->GetPointer(n, &v))
            return NGX_FailUnsupportedParameter;
        *out = (T*)v;
        return NGX_Success;
    }
    template <class T>
    static NVSDK_NGX_Result GetNumT(const NgxParams* p, const char* n, T* out)
    {
        if (!out)
            return NGX_FailInvalidParameter;
        if (!p->GetNumber(n, out))
            return NGX_FailUnsupportedParameter;
        return NGX_Success;
    }
    static NVSDK_NGX_Result GetPtr(const NgxParams* p, const char* n, void** o) { return GetPtrT(p, n, o); }
    static NVSDK_NGX_Result GetD3D12(const NgxParams* p, const char* n, ID3D12Resource** o) { return GetPtrT(p, n, o); }
    static NVSDK_NGX_Result GetD3D11(const NgxParams* p, const char* n, ID3D11Resource** o) { return GetPtrT(p, n, o); }
    static NVSDK_NGX_Result GetI32(const NgxParams* p, const char* n, int* o) { return GetNumT(p, n, o); }
    static NVSDK_NGX_Result GetU32(const NgxParams* p, const char* n, unsigned int* o) { return GetNumT(p, n, o); }
    static NVSDK_NGX_Result GetF64(const NgxParams* p, const char* n, double* o) { return GetNumT(p, n, o); }
    static NVSDK_NGX_Result GetF32(const NgxParams* p, const char* n, float* o) { return GetNumT(p, n, o); }
    static NVSDK_NGX_Result GetU64(const NgxParams* p, const char* n, unsigned long long* o) { return GetNumT(p, n, o); }
    static void Reset(NgxParams* p) { p->Clear(); }

    static void* const table[17];
};

// MSVC vtable order of NVSDK_NGX_Parameter (overload groups reversed).
void* const NgxParamsVtbl::table[17] = {
    (void*)&NgxParamsVtbl::SetPtr,   // 0x00 Set(void*)
    (void*)&NgxParamsVtbl::SetD3D12, // 0x08 Set(ID3D12Resource*)
    (void*)&NgxParamsVtbl::SetD3D11, // 0x10 Set(ID3D11Resource*)
    (void*)&NgxParamsVtbl::SetI32,   // 0x18 Set(int)
    (void*)&NgxParamsVtbl::SetU32,   // 0x20 Set(unsigned int)
    (void*)&NgxParamsVtbl::SetF64,   // 0x28 Set(double)
    (void*)&NgxParamsVtbl::SetF32,   // 0x30 Set(float)
    (void*)&NgxParamsVtbl::SetU64,   // 0x38 Set(unsigned long long)
    (void*)&NgxParamsVtbl::GetPtr,   // 0x40 Get(void**)
    (void*)&NgxParamsVtbl::GetD3D12, // 0x48 Get(ID3D12Resource**)
    (void*)&NgxParamsVtbl::GetD3D11, // 0x50 Get(ID3D11Resource**)
    (void*)&NgxParamsVtbl::GetI32,   // 0x58 Get(int*)
    (void*)&NgxParamsVtbl::GetU32,   // 0x60 Get(unsigned int*)
    (void*)&NgxParamsVtbl::GetF64,   // 0x68 Get(double*)
    (void*)&NgxParamsVtbl::GetF32,   // 0x70 Get(float*)
    (void*)&NgxParamsVtbl::GetU64,   // 0x78 Get(unsigned long long*)
    (void*)&NgxParamsVtbl::Reset,    // 0x80 Reset()
};

NgxParams::NgxParams() : m_vtbl(NgxParamsVtbl::table) {}

void NgxParams::Put(const char* name, const Value& v)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_values[name ? name : ""] = v;
}

bool NgxParams::Lookup(const char* name, Value* out) const
{
    std::lock_guard<std::mutex> g(m_lock);
    auto it = m_values.find(name ? name : "");
    if (it == m_values.end())
        return false;
    *out = it->second;
    return true;
}

bool NgxParams::GetPointer(const char* name, void** out) const
{
    std::lock_guard<std::mutex> g(m_lock);
    auto it = m_values.find(name ? name : "");
    if (it == m_values.end())
        return false;
    const Value& v = it->second;
    switch (v.type)
    {
    case Type::D3D11:
    case Type::D3D12:
    case Type::Ptr:
    case Type::U64: *out = v.ptr; return true;
    default: return false;
    }
}

void NgxParams::Clear()
{
    std::lock_guard<std::mutex> g(m_lock);
    m_values.clear();
}
