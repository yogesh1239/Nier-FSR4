// NVSDK_NGX_Parameter replacement.
//
// The mod (statically linked against nvsdk_ngx_s.lib, NGX core rel 310.9) only
// talks to parameter objects through their vtable. NVIDIA declares the
// interface with overloaded virtuals, which MSVC lays out grouped and in
// reverse declaration order. Offsets were confirmed in the mod's code:
//   +0x10 Set(ID3D11Resource*)   0x18001fbf0
//   +0x18 Set(int)               0x18001fca0
//   +0x20 Set(unsigned int)      0x18001fd00
//   +0x30 Set(float)             0x18001fc50
//   +0x58 Get(int*)              0x18001fb30
//   +0x60 Get(unsigned int*)     0x18001fb90
// The vtable is built by hand so the layout does not depend on the compiler's
// C++ ABI (this file is built with MinGW, the mod with MSVC).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

struct ID3D11Resource;
struct ID3D12Resource;

typedef int NVSDK_NGX_Result;
constexpr NVSDK_NGX_Result NGX_Success = 0x1;
constexpr NVSDK_NGX_Result NGX_Fail = (NVSDK_NGX_Result)0xBAD00000;
constexpr NVSDK_NGX_Result NGX_FailUnsupportedParameter = (NVSDK_NGX_Result)0xBAD00009;
constexpr NVSDK_NGX_Result NGX_FailInvalidParameter = (NVSDK_NGX_Result)0xBAD0000A;
constexpr NVSDK_NGX_Result NGX_FailNotInitialized = (NVSDK_NGX_Result)0xBAD00007;
constexpr NVSDK_NGX_Result NGX_FailFeatureNotSupported = (NVSDK_NGX_Result)0xBAD00002;

class NgxParams
{
public:
    enum class Type : uint8_t { U64, F32, F64, U32, I32, D3D11, D3D12, Ptr };
    struct Value
    {
        Type type;
        union
        {
            uint64_t u64;
            float f32;
            double f64;
            uint32_t u32;
            int32_t i32;
            void* ptr;
        };
    };

    NgxParams();

    // Typed accessors used by the shim itself.
    void SetU64(const char* name, uint64_t v) { Value x{Type::U64, {}}; x.u64 = v; Put(name, x); }
    void SetF32(const char* name, float v) { Value x{Type::F32, {}}; x.f32 = v; Put(name, x); }
    void SetF64(const char* name, double v) { Value x{Type::F64, {}}; x.f64 = v; Put(name, x); }
    void SetU32(const char* name, uint32_t v) { Value x{Type::U32, {}}; x.u32 = v; Put(name, x); }
    void SetI32(const char* name, int32_t v) { Value x{Type::I32, {}}; x.i32 = v; Put(name, x); }
    void SetPtr(const char* name, void* v, Type t) { Value x{t, {}}; x.ptr = v; Put(name, x); }

    bool GetNumber(const char* name, double* out) const;
    bool GetPointer(const char* name, void** out) const;

    float F32(const char* name, float def) const
    {
        double d;
        return GetNumber(name, &d) ? (float)d : def;
    }
    int64_t I64(const char* name, int64_t def) const
    {
        double d;
        return GetNumber(name, &d) ? (int64_t)d : def;
    }
    void* P(const char* name) const
    {
        void* p = nullptr;
        return GetPointer(name, &p) ? p : nullptr;
    }

    void Clear();

private:
    // Stores the whole value under the lock, so readers never see a half-written entry.
    void Put(const char* name, const Value& v);

    // Must stay the first member: the mod reads the vtable pointer at offset 0.
    void* const* m_vtbl;
    mutable std::mutex m_lock;
    std::unordered_map<std::string, Value> m_values;

    friend struct NgxParamsVtbl;
};
