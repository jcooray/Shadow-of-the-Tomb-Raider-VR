#include "build_config.h"
#include "dlss_mv.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "aer.h"
#include "audio_cue.h"
#include "camera_interceptor.h"
#include "head_tracking.h"
#include "logger.h"
#include "settings.h"

namespace vrmod {

namespace {

constexpr uint32_t kGridW = 32, kGridH = 18, kSamples = kGridW * kGridH;

const char* const kShader = R"(
Texture2D<float2> Mv : register(t0);
Texture2D<float> Depth : register(t1);
RWStructuredBuffer<float4> Out : register(u0);
cbuffer C : register(b0) { uint gw; uint gh; uint w; uint h; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gw || id.y >= gh) return;
    uint2 p = uint2(((float)id.x + 0.5f) * (float)w / (float)gw, ((float)id.y + 0.5f) * (float)h / (float)gh);
    float2 m = Mv.Load(int3(p, 0));
    float d = Depth.Load(int3(p, 0));
    Out[id.y * gw + id.x] = float4(m, d, 0.0f);
}
)";

// Render-thread objects (created on first use).
ID3D12Device* g_device = nullptr;
ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_pso = nullptr;
ID3D12DescriptorHeap* g_heap = nullptr;
ID3D12Resource* g_out = nullptr;
ID3D12Resource* g_readback = nullptr;
UINT g_descSize = 0;
bool g_initFailed = false;

std::atomic<bool> g_armed{false};
std::atomic<bool> g_pending{false};   // recorded; waiting for the GPU
std::atomic<int> g_waitPresents{0};
struct Capture { uint64_t frame; int eye; uint32_t w, h; DXGI_FORMAT mvFmt, depthFmt; };
Capture g_cap{};
std::mutex g_capMutex;

DXGI_FORMAT SrvFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    default: return f;
    }
}

bool Init(ID3D12Device* dev) {
    if (g_pso) return true;
    if (g_initFailed) return false;
    g_initFailed = true;   // until everything below succeeds
    g_device = dev;
    using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!compile) { modlog::Line("DLSS MV probe: d3dcompiler_47.dll / D3DCompile not available."); return false; }
    ID3DBlob* cs = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(compile(kShader, strlen(kShader), "dlss_mv_probe", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err))) {
        modlog::Line(std::string("DLSS MV probe: shader compile failed: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
        if (err) err->Release();
        return false;
    }
    // Root signature: 4 root constants (b0), one table {SRV t0-t1, UAV u0}.
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters = params;
    using Serialize_t = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    const auto serialize = reinterpret_cast<Serialize_t>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
    ID3DBlob* rsBlob = nullptr;
    if (!serialize || FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &err)) ||
        FAILED(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&g_root)))) {
        modlog::Line("DLSS MV probe: root signature failed.");
        if (err) err->Release();
        if (rsBlob) rsBlob->Release();
        cs->Release();
        return false;
    }
    rsBlob->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_root;
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    const HRESULT hp = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso));
    cs->Release();
    if (FAILED(hp)) { modlog::Line("DLSS MV probe: compute PSO failed."); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 3;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) { modlog::Line("DLSS MV probe: descriptor heap failed."); return false; }
    g_descSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, rb = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kSamples * 16;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_out)))) {
        modlog::Line("DLSS MV probe: output buffer failed.");
        return false;
    }
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback)))) {
        modlog::Line("DLSS MV probe: readback buffer failed.");
        return false;
    }
    g_initFailed = false;
    modlog::Line("DLSS MV probe: GPU objects ready.");
    return true;
}

// ---- prediction ----
struct Axes { float r[3], f[3], u[3]; };
Axes AxesOf(const float q[4]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    Axes a;
    a.r[0] = 1 - 2 * (y * y + z * z); a.r[1] = 2 * (x * y + w * z);     a.r[2] = 2 * (x * z - w * y);
    a.f[0] = 2 * (x * y - w * z);     a.f[1] = 1 - 2 * (x * x + z * z); a.f[2] = 2 * (y * z + w * x);
    a.u[0] = 2 * (x * z + w * y);     a.u[1] = 2 * (y * z - w * x);     a.u[2] = 1 - 2 * (x * x + y * y);
    return a;
}
float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Pixel of world point P in camera c (false if behind).
bool Project(const CamFrameRec& c, float tanH, float tanV, uint32_t w, uint32_t h, const float P[3], float* px, float* py) {
    const Axes a = AxesOf(c.q);
    const float d[3] = { P[0] - c.pos[0], P[1] - c.pos[1], P[2] - c.pos[2] };
    const float z = Dot(d, a.f);
    if (z <= 1.0f) return false;
    const float nx = Dot(d, a.r) / (z * tanH), ny = Dot(d, a.u) / (z * tanV);
    *px = (nx + 1.0f) * 0.5f * static_cast<float>(w);
    *py = (1.0f - ny) * 0.5f * static_cast<float>(h);
    return true;
}

void Resolve() {
    Capture cap;
    { std::lock_guard<std::mutex> lock(g_capMutex); cap = g_cap; }
    std::vector<float> s(kSamples * 4);
    void* p = nullptr;
    D3D12_RANGE r = { 0, kSamples * 16 };
    if (FAILED(g_readback->Map(0, &r, &p)) || !p) { modlog::Line("DLSS MV probe: map failed."); return; }
    memcpy(s.data(), p, kSamples * 16);
    D3D12_RANGE none = { 0, 0 };
    g_readback->Unmap(0, &none);

    CamFrameRec cur{}, oth{}, same{};
    const bool haveCams = CameraFrameRecord(cap.frame, &cur) && CameraFrameRecord(cap.frame - 1, &oth) && CameraFrameRecord(cap.frame - 2, &same);
    float fovV = 0, aspect = 0;
    const bool haveProj = AerFinalProjectionOf(cap.frame, &fovV, &aspect);
    char head[400];
    snprintf(head, sizeof(head), "DLSS MV probe: frame %llu eye %d render %ux%u mv fmt %d depth fmt %d | cameras %s, projection %s (fovV %.4f rad, aspect %.4f; w/h %.4f) | near %.2f far %.1f cm",
             static_cast<unsigned long long>(cap.frame), cap.eye, cap.w, cap.h, static_cast<int>(cap.mvFmt), static_cast<int>(cap.depthFmt),
             haveCams ? "ok" : "MISSING", haveProj ? "ok" : "MISSING", fovV, aspect, cap.w / static_cast<float>(cap.h), cur.nearCm, cur.farCm);
    modlog::Line(head);
    // Raw sample summary.
    {
        std::vector<float> mags, depths;
        for (uint32_t i = 0; i < kSamples; ++i) {
            mags.push_back(std::hypot(s[i * 4], s[i * 4 + 1]));
            depths.push_back(s[i * 4 + 2]);
        }
        std::sort(mags.begin(), mags.end());
        std::sort(depths.begin(), depths.end());
        char b[300];
        snprintf(b, sizeof(b), "DLSS MV probe: |mv| median %.4f max %.4f; depth min %.6f median %.6f max %.6f | sample (16,9): mv (%.4f, %.4f) depth %.6f",
                 mags[kSamples / 2], mags.back(), depths.front(), depths[kSamples / 2], depths.back(),
                 s[(9 * kGridW + 16) * 4], s[(9 * kGridW + 16) * 4 + 1], s[(9 * kGridW + 16) * 4 + 2]);
        modlog::Line(b);
    }
    if (!haveCams || !haveProj) return;
    const float tanV = std::tan(fovV * 0.5f), tanH = tanV * aspect;
    const Axes a = AxesOf(cur.q);
    const float n = cur.nearCm, f = cur.farCm;
    const char* const kDepthName[3] = { "reverse-Z infinite", "reverse-Z finite", "forward-Z finite" };
    struct Hyp { int depth; float sign; bool uv; std::vector<float> errs; std::vector<float> corr; };
    std::vector<Hyp> hyps;
    for (int d = 0; d < 3; ++d) for (float sg : { 1.0f, -1.0f }) for (bool uv : { false, true }) hyps.push_back({ d, sg, uv, {}, {} });
    for (uint32_t gy = 0; gy < kGridH; ++gy) {
        for (uint32_t gx = 0; gx < kGridW; ++gx) {
            const uint32_t i = gy * kGridW + gx;
            const float mvx = s[i * 4], mvy = s[i * 4 + 1], dep = s[i * 4 + 2];
            if (!(dep > 1e-6f && dep < 0.999999f)) continue;   // sky / cleared
            const float px = std::floor((gx + 0.5f) * cap.w / kGridW) + 0.5f, py = std::floor((gy + 0.5f) * cap.h / kGridH) + 0.5f;
            const float nx = px / cap.w * 2.0f - 1.0f, ny = 1.0f - py / cap.h * 2.0f;
            for (Hyp& hp : hyps) {
                float z;
                if (hp.depth == 0) z = n / dep;
                else if (hp.depth == 1) z = n * f / (dep * (f - n) + n);
                else z = n * f / (f - dep * (f - n));
                if (!(z > 0.0f) || z > 1e7f) continue;
                const float x = nx * tanH * z, y = ny * tanV * z;
                const float P[3] = { cur.pos[0] + a.r[0] * x + a.f[0] * z + a.u[0] * y, cur.pos[1] + a.r[1] * x + a.f[1] * z + a.u[1] * y,
                                     cur.pos[2] + a.r[2] * x + a.f[2] * z + a.u[2] * y };
                float ox, oy, sx, sy;
                if (!Project(oth, tanH, tanV, cap.w, cap.h, P, &ox, &oy)) continue;
                float ex = hp.sign * (ox - px), ey = hp.sign * (oy - py);
                if (hp.uv) { ex /= cap.w; ey /= cap.h; }
                const float err = std::hypot(mvx - ex, mvy - ey) / (hp.uv ? 1.0f / cap.w : 1.0f);   // error in pixels
                hp.errs.push_back(err);
                if (Project(same, tanH, tanV, cap.w, cap.h, P, &sx, &sy)) hp.corr.push_back(std::hypot(sx - ox, sy - oy));
            }
        }
    }
    std::sort(hyps.begin(), hyps.end(), [](const Hyp& l, const Hyp& r2) {
        auto med = [](std::vector<float> v) { if (v.empty()) return 1e30f; std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
        return med(l.errs) < med(r2.errs);
    });
    for (size_t k = 0; k < hyps.size() && k < 4; ++k) {
        std::vector<float> e = hyps[k].errs, c = hyps[k].corr;
        std::sort(e.begin(), e.end());
        std::sort(c.begin(), c.end());
        char b[300];
        snprintf(b, sizeof(b), "DLSS MV probe: #%zu %s, mv = %s(previous - current), %s: median error %.3f px, 90%% %.3f px over %zu samples | "
                 "D2 correction (same eye 2 back vs other eye 1 back) median %.2f px, 90%% %.2f px",
                 k + 1, kDepthName[hyps[k].depth], hyps[k].sign > 0 ? "+" : "-", hyps[k].uv ? "UV units" : "pixels",
                 e.empty() ? -1.0f : e[e.size() / 2], e.empty() ? -1.0f : e[e.size() * 9 / 10], e.size(),
                 c.empty() ? -1.0f : c[c.size() / 2], c.empty() ? -1.0f : c[c.size() * 9 / 10]);
        modlog::Line(b);
    }
}


// ---- D2b: the motion-vector correction ----
// For each pixel: reconstruct the point from depth with the frame's camera (reverse-Z, D2a), project it with the same
// eye's camera two frames back and with the other eye's one frame back; the camera part of the game's vector (which
// is the other eye's) becomes the same eye's, and the object part is scaled from one frame to two.
// mv = previous - current, pixels at render resolution (D2a: 0.002-0.01 px median error).
const char* const kFixShader = R"(
Texture2D<float2> Mv : register(t0);
Texture2D<float> Depth : register(t1);
RWTexture2D<float2> OutMv : register(u0);
cbuffer C : register(b0) {
    float4 curR, curF, curU;
    float4 othP, othR, othF, othU;
    float4 samP, samR, samF, samU;
    float4 proj;
    float4 size;
};
bool Pixel(float3 d, float3 r, float3 f, float3 u, out float2 px) {
    float z = dot(d, f);
    px = float2(0, 0);
    if (z <= 1.0f) return false;
    float2 ndc = float2(dot(d, r) / (z * proj.x), dot(d, u) / (z * proj.y));
    px = float2((ndc.x + 1.0f) * 0.5f * size.x, (1.0f - ndc.y) * 0.5f * size.y);
    return true;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;
    float2 mv = Mv.Load(int3(id.xy, 0));
    float dep = Depth.Load(int3(id.xy, 0));
    float n = proj.z, fa = proj.w;
    float z = dep > 0.0f ? n * fa / (dep * (fa - n) + n) : 1.0e7f;
    float2 pc = float2(id.xy) + 0.5f;
    float2 ndc = float2(pc.x / size.x * 2.0f - 1.0f, 1.0f - pc.y / size.y * 2.0f);
    float3 d = curR.xyz * (ndc.x * proj.x * z) + curF.xyz * z + curU.xyz * (ndc.y * proj.y * z);
    float2 pO, pS;
    if (!Pixel(d - othP.xyz, othR.xyz, othF.xyz, othU.xyz, pO) || !Pixel(d - samP.xyz, samR.xyz, samF.xyz, samU.xyz, pS)) {
        OutMv[id.xy] = mv;
        return;
    }
    float2 obj = mv - (pO - pc);
    OutMv[id.xy] = (pS - pc) + obj * size.z;
}
)";

constexpr UINT kFixConsts = 52;
constexpr UINT kFixRing = 8;
ID3D12RootSignature* g_fixRoot = nullptr;
ID3D12PipelineState* g_fixPso = nullptr;
ID3D12DescriptorHeap* g_fixHeap = nullptr;
ID3D12Resource* g_fixOut = nullptr;
D3D12_RESOURCE_DESC g_fixOutDesc = {};
D3D12_RESOURCE_STATES g_fixOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
UINT g_fixSlot = 0;
bool g_fixInitFailed = false;
std::atomic<bool> g_fixOn{true};
std::atomic<uint64_t> g_fixApplied{0}, g_fixSkipped{0};

bool InitFix(ID3D12Device* dev) {
    if (g_fixPso) return true;
    if (g_fixInitFailed) return false;
    g_fixInitFailed = true;
    using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    ID3DBlob* cs = nullptr;
    ID3DBlob* err = nullptr;
    if (!compile || FAILED(compile(kFixShader, strlen(kFixShader), "dlss_mv_fix", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err))) {
        modlog::Line(std::string("DLSS D2: shader compile failed: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "no compiler"));
        if (err) err->Release();
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = kFixConsts;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters = params;
    using Serialize_t = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    const auto serialize = reinterpret_cast<Serialize_t>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
    ID3DBlob* rsBlob = nullptr;
    const bool rsOk = serialize && SUCCEEDED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &err)) &&
                      SUCCEEDED(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&g_fixRoot)));
    if (rsBlob) rsBlob->Release();
    if (err) err->Release();
    if (!rsOk) { cs->Release(); modlog::Line("DLSS D2: root signature failed."); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_fixRoot;
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    const HRESULT hp = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_fixPso));
    cs->Release();
    if (FAILED(hp)) { modlog::Line("DLSS D2: compute PSO failed."); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 3 * kFixRing;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_fixHeap)))) { modlog::Line("DLSS D2: descriptor heap failed."); return false; }
    if (!g_descSize) g_descSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_fixInitFailed = false;
    modlog::Line("DLSS D2: motion-vector correction ready.");
    return true;
}

// Our motion-vector texture, matching the game's (size and format), UAV-capable.
bool EnsureFixOut(ID3D12Device* dev, const D3D12_RESOURCE_DESC& md) {
    if (g_fixOut && g_fixOutDesc.Width == md.Width && g_fixOutDesc.Height == md.Height && g_fixOutDesc.Format == md.Format) return true;
    if (g_fixOut) { g_fixOut->Release(); g_fixOut = nullptr; }
    D3D12_RESOURCE_DESC d = md;
    d.Format = SrvFormat(md.Format);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    d.MipLevels = 1;
    d.DepthOrArraySize = 1;
    d.SampleDesc = { 1, 0 };
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT };
    if (FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_fixOut)))) {
        modlog::Line("DLSS D2: motion-vector texture failed.");
        return false;
    }
    g_fixOutDesc = md;
    g_fixOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    char b[160];
    snprintf(b, sizeof(b), "DLSS D2: motion-vector texture %llux%u format %d", static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(d.Format));
    modlog::Line(b);
    return true;
}

void PutAxes(float* c, const float q[4]) {
    const Axes a = AxesOf(q);
    for (int k = 0; k < 3; ++k) { c[k] = a.r[k]; c[4 + k] = a.f[k]; c[8 + k] = a.u[k]; }
    c[3] = c[7] = c[11] = 0.0f;
}

// ---- D2c: which frame is DLSS rendering? ----
// D1/D2 predict the frame of an evaluate as AER's last paired frame + 1, which assumes the Present was handled before
// the render thread started the next frame. After a stall (the pause menu: a 37-Present mono resync) the render thread
// can run a frame ahead, and D2 then corrects with the wrong cameras (the user: broken after the pause menu until
// per-eye was cycled). The game's own motion vectors identify the frame: our cameras of the right frame (and the one
// before it, the other eye) reproduce them to ~0.01 px (D2a). Every second routed evaluate samples 16x9 of them; six
// Presents later the Present thread finds the offset (-2..+2 from the prediction) whose cameras match, and after two
// agreeing checks D1/D2 use it (both histories reset on a change).
constexpr uint32_t kChkW = 16, kChkH = 9, kChkN = kChkW * kChkH, kChkSlots = 8;
struct ChkSlot {
    std::atomic<int> state{0};   // 0 free, 1 recorded (render thread), resolved by the Present thread
    ID3D12Resource* out = nullptr;
    ID3D12Resource* readback = nullptr;
    uint64_t raw = 0, present = 0;
    uint32_t w = 0, h = 0;
};
ChkSlot g_chk[kChkSlots];
ID3D12DescriptorHeap* g_chkHeap = nullptr;
bool g_chkInitFailed = false;
std::atomic<int> g_frameOffset{0};
std::atomic<bool> g_offsetChanged{false};
std::atomic<uint64_t> g_chkDone{0}, g_chkAgree{0}, g_chkAmbiguous{0};
int g_chkVoteOffset = 0, g_chkVotes = 0;   // Present thread only
// Pairing follow (settings aer_pair_follow_d2c). The Present shows the frame DLSS evaluated just before it, so an offset
// here means AER paired that Present with the neighbouring game frame -- the other eye. Instead of only re-routing DLSS,
// the pairing is moved (AerRequestPairShift) and the offset returns to 0. Checks recorded before the move are dropped
// (they measured the old pairing). Guard: two reversals within 10 s each mean the model is wrong here -- undo the moves
// and fall back to re-routing DLSS only.
bool g_followOff = false;
uint64_t g_chkIgnoreUpTo = 0;
int g_followNet = 0, g_followFlips = 0, g_followLastSign = 0;
ULONGLONG g_followLastMs = 0;
float g_chkLastBest = 0.0f, g_chkLastSecond = 0.0f;

bool InitCheck(ID3D12Device* dev) {
    if (g_chkHeap) return true;
    if (g_chkInitFailed || !Init(dev)) return false;
    g_chkInitFailed = true;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 3 * kChkSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_chkHeap)))) return false;
    D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, rb = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kChkN * 16;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (ChkSlot& c : g_chk) {
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&c.out)))) return false;
        bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&c.readback)))) return false;
    }
    g_chkInitFailed = false;
    modlog::Line("DLSS D2c: frame check ready.");
    return true;
}

// Median error (px) of the game's samples against the prediction for frame X (its camera and X-1's); -1 if unusable.
float FrameError(const float* s, uint32_t w, uint32_t h, uint64_t X, int* count) {
    CamFrameRec cur{}, oth{};
    float fovV = 0.0f, aspect = 0.0f;
    *count = 0;
    if (!CameraFrameRecord(X, &cur) || !CameraFrameRecord(X - 1, &oth)) return -1.0f;
    if (!AerFinalProjectionOf(X, &fovV, &aspect) && !AerFinalProjectionOf(X - 1, &fovV, &aspect)) return -1.0f;
    const float tanV = std::tan(fovV * 0.5f), tanH = tanV * aspect;
    const Axes a = AxesOf(cur.q);
    const float n = cur.nearCm, f = cur.farCm;
    std::vector<float> errs;
    for (uint32_t gy = 0; gy < kChkH; ++gy) {
        for (uint32_t gx = 0; gx < kChkW; ++gx) {
            const uint32_t i = gy * kChkW + gx;
            const float mvx = s[i * 4], mvy = s[i * 4 + 1], dep = s[i * 4 + 2];
            if (!(dep > 1e-6f && dep < 0.999999f)) continue;
            const float px = std::floor((gx + 0.5f) * w / kChkW) + 0.5f, py = std::floor((gy + 0.5f) * h / kChkH) + 0.5f;
            const float nx = px / w * 2.0f - 1.0f, ny = 1.0f - py / h * 2.0f;
            const float z = n * f / (dep * (f - n) + n);
            const float x = nx * tanH * z, y = ny * tanV * z;
            const float P[3] = { cur.pos[0] + a.r[0] * x + a.f[0] * z + a.u[0] * y, cur.pos[1] + a.r[1] * x + a.f[1] * z + a.u[1] * y,
                                 cur.pos[2] + a.r[2] * x + a.f[2] * z + a.u[2] * y };
            float ox, oy;
            if (!Project(oth, tanH, tanV, w, h, P, &ox, &oy)) continue;
            errs.push_back(std::hypot(mvx - (ox - px), mvy - (oy - py)));
        }
    }
    *count = static_cast<int>(errs.size());
    if (errs.size() < 12) return -1.0f;
    std::sort(errs.begin(), errs.end());
    return errs[errs.size() / 2];
}

void ResolveChecks() {
    const uint64_t now = HeadTrackingGetPresentFrame();
    for (ChkSlot& c : g_chk) {
        if (c.state.load(std::memory_order_acquire) != 1 || now < c.present + 6) continue;
        if (c.present <= g_chkIgnoreUpTo) { c.state.store(0, std::memory_order_release); continue; }
        float s[kChkN * 4];
        void* p = nullptr;
        D3D12_RANGE r = { 0, kChkN * 16 };
        bool ok = SUCCEEDED(c.readback->Map(0, &r, &p)) && p;
        if (ok) {
            memcpy(s, p, sizeof s);
            D3D12_RANGE none = { 0, 0 };
            c.readback->Unmap(0, &none);
        }
        const uint64_t raw = c.raw;
        const uint32_t w = c.w, h = c.h;
        c.state.store(0, std::memory_order_release);
        if (!ok) continue;
        float best = 1e30f, second = 1e30f;
        int bestO = 0;
        for (int o = -2; o <= 2; ++o) {
            int cnt = 0;
            const float e = FrameError(s, w, h, raw + o, &cnt);
            if (e < 0.0f) continue;
            if (e < best) { second = best; best = e; bestO = o; } else if (e < second) second = e;
        }
        g_chkDone.fetch_add(1, std::memory_order_relaxed);
        g_chkLastBest = best;
        g_chkLastSecond = second;
        // A clear match: well under a pixel, and clearly better than any other offset.
        if (!(best < 0.3f && second > 1.0f && second > 4.0f * best)) { g_chkAmbiguous.fetch_add(1, std::memory_order_relaxed); continue; }
        const int cur = g_frameOffset.load();
        if (bestO == cur) { g_chkAgree.fetch_add(1, std::memory_order_relaxed); g_chkVotes = 0; continue; }
        if (bestO == g_chkVoteOffset) ++g_chkVotes; else { g_chkVoteOffset = bestO; g_chkVotes = 1; }
        if (g_chkVotes >= 2) {
            g_chkVotes = 0;
            if (GetAerPairFollowD2c() && !g_followOff) {
                const int shift = bestO - cur;
                const int sign = shift > 0 ? 1 : -1;
                const ULONGLONG nowMs = GetTickCount64();
                g_followFlips = (g_followLastSign == -sign && nowMs - g_followLastMs < 10000) ? g_followFlips + 1 : 0;
                g_followLastSign = sign;
                g_followLastMs = nowMs;
                char b[300];
                // 2026-10-02: after a load the offset genuinely flips back and forth for a few seconds; switching the
                // follow off then fell back to re-routing DLSS only -- the swapped eyes again, for the whole session.
                // Reversals are only noted in the line below now.
                if (false) {
                    g_followOff = true;
                    if (g_followNet) AerRequestPairShift(-g_followNet);
                    snprintf(b, sizeof(b), "DLSS D2c: pairing follow OFF -- it reversed twice within 10 s; pairing moves undone (%+d), "
                             "DLSS is re-routed instead (offset %+d)", -g_followNet, bestO + g_followNet);
                    g_followNet = 0;
                    g_frameOffset.store(0);
                    g_offsetChanged.store(true);
                    g_chkIgnoreUpTo = now;
                    modlog::Line(b);
                    continue;
                }
                AerRequestPairShift(shift);
                g_followNet += shift;
                g_frameOffset.store(0);
                g_offsetChanged.store(true);
                g_chkIgnoreUpTo = now;
                snprintf(b, sizeof(b), "DLSS D2c: the evaluated frame is the prediction %+d (median error %.3f px, next best %.2f px) -- the "
                         "Presents were paired with the wrong game frame (each eye showed the other eye's image); eye pairing moved %+d "
                         "(net %+d, window lag now %d from next Present%s), both histories reset",
                         bestO, best, second, shift, g_followNet, AerPairExtraLag() - shift, g_followFlips ? "; reversal" : "");
                modlog::Line(b);
                continue;
            }
            g_frameOffset.store(bestO);
            g_offsetChanged.store(true);
            char b[200];
            snprintf(b, sizeof(b), "DLSS D2c: the evaluated frame is the prediction %+d (was %+d; median error %.3f px, next best %.2f px) -- both histories reset",
                     bestO, cur, best, second);
            modlog::Line(b);
        }
    }
}
} // namespace

bool MvProbeArmed() { return g_armed.load(std::memory_order_relaxed); }

void MvProbeOnEvaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* motion, uint32_t renderW,
                       uint32_t renderH, uint64_t frame, int eye) {
    if (!g_armed.exchange(false) || !list || !depth || !motion) return;
    ID3D12Device* dev = nullptr;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return;
    const bool ok = Init(dev);
    dev->Release();
    if (!ok) { PlayCue(Cue::Failure); return; }
    const D3D12_RESOURCE_DESC md = motion->GetDesc(), dd = depth->GetDesc();
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = SrvFormat(md.Format);
    g_device->CreateShaderResourceView(motion, &sv, h);
    h.ptr += g_descSize;
    sv.Format = SrvFormat(dd.Format);
    g_device->CreateShaderResourceView(depth, &sv, h);
    h.ptr += g_descSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uv.Format = DXGI_FORMAT_UNKNOWN;
    uv.Buffer.NumElements = kSamples;
    uv.Buffer.StructureByteStride = 16;
    g_device->CreateUnorderedAccessView(g_out, nullptr, &uv, h);

    const uint32_t consts[4] = { kGridW, kGridH, renderW, renderH };
    list->SetDescriptorHeaps(1, &g_heap);
    list->SetComputeRootSignature(g_root);
    list->SetComputeRoot32BitConstants(0, 4, consts, 0);
    list->SetComputeRootDescriptorTable(1, g_heap->GetGPUDescriptorHandleForHeapStart());
    list->SetPipelineState(g_pso);
    list->Dispatch((kGridW + 7) / 8, (kGridH + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_out;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    list->CopyResource(g_readback, g_out);
    {
        std::lock_guard<std::mutex> lock(g_capMutex);
        g_cap = { frame, eye, renderW, renderH, md.Format, dd.Format };
    }
    g_waitPresents.store(8);
    g_pending.store(true);
}

bool MvFixEnabled() { return g_fixOn.load(std::memory_order_relaxed); }

ID3D12Resource* MvFixOnEvaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* motion, uint32_t renderW,
                                uint32_t renderH, uint64_t frame) {
    if (!g_fixOn.load(std::memory_order_relaxed) || !list || !depth || !motion || frame < 3) return nullptr;
    CamFrameRec cur{}, oth{}, sam{};
    float fovV = 0.0f, aspect = 0.0f;
    // The frame's own projection is recorded one frame late; the previous frame's is the same unless the FOV changes.
    const bool proj = AerFinalProjectionOf(frame, &fovV, &aspect) || AerFinalProjectionOf(frame - 1, &fovV, &aspect);
    if (!proj || !CameraFrameRecord(frame, &cur) || !CameraFrameRecord(frame - 1, &oth) || !CameraFrameRecord(frame - 2, &sam) ||
        AerEyeOfFrame(frame) != AerEyeOfFrame(frame - 2) || AerEyeOfFrame(frame) == AerEyeOfFrame(frame - 1)) {
        g_fixSkipped.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    ID3D12Device* dev = nullptr;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return nullptr;
    const D3D12_RESOURCE_DESC md = motion->GetDesc(), dd = depth->GetDesc();
    const bool ok = InitFix(dev) && EnsureFixOut(dev, md);
    if (!ok) { dev->Release(); return nullptr; }
    // Descriptors for this frame (a ring, so the GPU can still be reading older ones).
    const UINT slot = (g_fixSlot++ % kFixRing) * 3;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_fixHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * g_descSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = SrvFormat(md.Format);
    dev->CreateShaderResourceView(motion, &sv, h);
    h.ptr += g_descSize;
    sv.Format = SrvFormat(dd.Format);
    dev->CreateShaderResourceView(depth, &sv, h);
    h.ptr += g_descSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = SrvFormat(md.Format);
    dev->CreateUnorderedAccessView(g_fixOut, nullptr, &uv, h);
    dev->Release();

    // Constants: everything relative to the current camera position.
    float c[kFixConsts] = {};
    PutAxes(c + 0, cur.q);
    for (int k = 0; k < 3; ++k) c[12 + k] = oth.pos[k] - cur.pos[k];
    PutAxes(c + 16, oth.q);
    for (int k = 0; k < 3; ++k) c[28 + k] = sam.pos[k] - cur.pos[k];
    PutAxes(c + 32, sam.q);
    const float tanV = std::tan(fovV * 0.5f);
    c[44] = tanV * aspect;
    c[45] = tanV;
    c[46] = cur.nearCm;
    c[47] = cur.farCm;
    c[48] = static_cast<float>(renderW);
    c[49] = static_cast<float>(renderH);
    c[50] = 2.0f;   // object motion: one frame -> two frames
    c[51] = 0.0f;

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_fixOut;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (g_fixOutState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        b.Transition.StateBefore = g_fixOutState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list->ResourceBarrier(1, &b);
    }
    list->SetDescriptorHeaps(1, &g_fixHeap);
    list->SetComputeRootSignature(g_fixRoot);
    list->SetComputeRoot32BitConstants(0, kFixConsts, c, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE g = g_fixHeap->GetGPUDescriptorHandleForHeapStart();
    g.ptr += static_cast<UINT64>(slot) * g_descSize;
    list->SetComputeRootDescriptorTable(1, g);
    list->SetPipelineState(g_fixPso);
    list->Dispatch((renderW + 7) / 8, (renderH + 7) / 8, 1);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &b);
    g_fixOutState = b.Transition.StateAfter;
    g_fixApplied.fetch_add(1, std::memory_order_relaxed);
    return g_fixOut;
}

int MvFrameOffset() { return g_frameOffset.load(std::memory_order_relaxed); }
bool MvTakeOffsetChange() { return g_offsetChanged.exchange(false); }

void MvFrameCheckOnEvaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* motion, uint32_t renderW,
                            uint32_t renderH, uint64_t rawFrame) {
    static uint32_t tick = 0;
    if (!list || !depth || !motion || rawFrame < 4 || (tick++ & 1)) return;
    ChkSlot* slot = nullptr;
    UINT idx = 0;
    for (UINT i = 0; i < kChkSlots; ++i) if (g_chk[i].state.load(std::memory_order_acquire) == 0) { slot = &g_chk[i]; idx = i; break; }
    if (!slot) return;
    ID3D12Device* dev = nullptr;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return;
    const bool ok = InitCheck(dev);
    if (!ok) { dev->Release(); return; }
    const D3D12_RESOURCE_DESC md = motion->GetDesc(), dd = depth->GetDesc();
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_chkHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(idx * 3) * g_descSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = SrvFormat(md.Format);
    dev->CreateShaderResourceView(motion, &sv, h);
    h.ptr += g_descSize;
    sv.Format = SrvFormat(dd.Format);
    dev->CreateShaderResourceView(depth, &sv, h);
    h.ptr += g_descSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uv.Format = DXGI_FORMAT_UNKNOWN;
    uv.Buffer.NumElements = kChkN;
    uv.Buffer.StructureByteStride = 16;
    dev->CreateUnorderedAccessView(slot->out, nullptr, &uv, h);
    dev->Release();
    const uint32_t consts[4] = { kChkW, kChkH, renderW, renderH };
    list->SetDescriptorHeaps(1, &g_chkHeap);
    list->SetComputeRootSignature(g_root);
    list->SetComputeRoot32BitConstants(0, 4, consts, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE g = g_chkHeap->GetGPUDescriptorHandleForHeapStart();
    g.ptr += static_cast<UINT64>(idx * 3) * g_descSize;
    list->SetComputeRootDescriptorTable(1, g);
    list->SetPipelineState(g_pso);
    list->Dispatch((kChkW + 7) / 8, (kChkH + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = slot->out;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    list->CopyResource(slot->readback, slot->out);
    slot->raw = rawFrame;
    slot->present = HeadTrackingGetPresentFrame();
    slot->w = renderW;
    slot->h = renderH;
    slot->state.store(1, std::memory_order_release);
}

void MvProbeOnPresent() {
    static bool qWas = false;
    const bool q = (VRMOD_DEV_KEY('Q') & 0x8000) != 0;
    const bool ca = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    if (q && !qWas && ca && !g_pending.load()) {
        g_armed.store(true);
        modlog::Line("DLSS MV probe: armed (next DLSS evaluate). Keep moving the head a little; hold still in the world.");
        PlayCue(Cue::On);
    }
    qWas = q;
    static bool fWas = false;
    const bool fk = (VRMOD_DEV_KEY('F') & 0x8000) != 0;
    if (fk && !fWas && ca) {
        const bool on = !g_fixOn.load();
        g_fixOn.store(on);
        modlog::Line(std::string("DLSS D2: same-eye motion vectors ") + (on ? "ON" : "off") + " (Ctrl+Alt+F; used with per-eye history)");
        PlayCue(on ? Cue::On : Cue::Off);
    }
    fWas = fk;
    {
        static ULONGLONG last = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - last >= 10000) {
            last = now;
            const uint64_t a = g_fixApplied.exchange(0), k = g_fixSkipped.exchange(0);
            if (a + k > 0) {
                char b[260];
                snprintf(b, sizeof(b), "DLSS D2 (10 s): motion vectors corrected %llu, skipped %llu (no same-eye frame) | frame checks %llu: agree %llu, "
                         "ambiguous %llu; offset %+d (last best %.3f px, next %.2f px)",
                         static_cast<unsigned long long>(a), static_cast<unsigned long long>(k), static_cast<unsigned long long>(g_chkDone.exchange(0)),
                         static_cast<unsigned long long>(g_chkAgree.exchange(0)), static_cast<unsigned long long>(g_chkAmbiguous.exchange(0)),
                         g_frameOffset.load(), g_chkLastBest, g_chkLastSecond);
                modlog::Line(b);
            }
        }
    }
    ResolveChecks();
    if (g_pending.load() && g_waitPresents.fetch_sub(1) <= 1) {
        g_pending.store(false);
        Resolve();
        PlayCue(Cue::Success);
    }
}

} // namespace vrmod
