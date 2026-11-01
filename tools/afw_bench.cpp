// Offline bench for frame warp (src/afw.cpp): re-runs the warp on the CPU from a capture (a log mark in a development
// build saves one -- afw.cpp "Capture") and compares it with the warped eye's real image one frame later, so changes to
// the warp can be tried without the game. Mirrors the shader's passes; the hole fill has switchable variants.
//
// Usage:
//   vrmod_afw_bench <capture folder> [out folder]   -> metrics for each variant, difference images in the out folder
//   vrmod_afw_bench --synth <folder>                -> writes a synthetic capture (a near box in front of a far wall,
//                                                       the box moved since the warped eye's last real frame) to test
//                                                       the bench itself
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Img {   // RGB, 8 bit
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;   // w * h * 3, top row first
    const uint8_t* At(uint32_t x, uint32_t y) const { return px.data() + (static_cast<size_t>(y) * w + x) * 3; }
    uint8_t* At(uint32_t x, uint32_t y) { return px.data() + (static_cast<size_t>(y) * w + x) * 3; }
};
struct Depth {   // linear depth, cm
    uint32_t w = 0, h = 0;
    std::vector<float> v;
    float At(uint32_t x, uint32_t y) const { return v[static_cast<size_t>(y) * w + x]; }
};

bool ReadBmp(const std::string& path, Img* img) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    uint8_t hd[54];
    f.read(reinterpret_cast<char*>(hd), 54);
    if (!f || hd[0] != 'B' || hd[1] != 'M') return false;
    int32_t w, h;
    uint32_t off;
    uint16_t bpp;
    memcpy(&off, hd + 10, 4);
    memcpy(&w, hd + 18, 4);
    memcpy(&h, hd + 22, 4);
    memcpy(&bpp, hd + 28, 2);
    if (bpp != 24 || w <= 0 || h == 0) return false;
    const bool up = h > 0;
    h = std::abs(h);
    img->w = w; img->h = h;
    img->px.assign(static_cast<size_t>(w) * h * 3, 0);
    const uint32_t row = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> line(row);
    f.seekg(off);
    for (int y = 0; y < h; ++y) {
        f.read(reinterpret_cast<char*>(line.data()), row);
        uint8_t* d = img->At(0, up ? h - 1 - y : y);
        for (int x = 0; x < w; ++x) { d[x * 3] = line[x * 3 + 2]; d[x * 3 + 1] = line[x * 3 + 1]; d[x * 3 + 2] = line[x * 3]; }
    }
    return true;
}

void WriteBmp(const std::string& path, const Img& img) {
    std::ofstream f(path, std::ios::binary);
    const uint32_t row = (img.w * 3 + 3) & ~3u, size = 54 + row * img.h;
    uint8_t hd[54] = { 'B', 'M' };
    memcpy(hd + 2, &size, 4);
    const uint32_t off = 54, dib = 40, planesBpp = 1 | (24 << 16);
    memcpy(hd + 10, &off, 4);
    memcpy(hd + 14, &dib, 4);
    memcpy(hd + 18, &img.w, 4);
    memcpy(hd + 22, &img.h, 4);
    memcpy(hd + 26, &planesBpp, 4);
    f.write(reinterpret_cast<char*>(hd), 54);
    std::vector<uint8_t> line(row, 0);
    for (uint32_t y = 0; y < img.h; ++y) {
        const uint8_t* s = img.At(0, img.h - 1 - y);
        for (uint32_t x = 0; x < img.w; ++x) { line[x * 3] = s[x * 3 + 2]; line[x * 3 + 1] = s[x * 3 + 1]; line[x * 3 + 2] = s[x * 3]; }
        f.write(reinterpret_cast<char*>(line.data()), row);
    }
}

bool ReadF32(const std::string& path, uint32_t w, uint32_t h, Depth* d) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    d->w = w; d->h = h;
    d->v.assign(static_cast<size_t>(w) * h, 0.0f);
    f.read(reinterpret_cast<char*>(d->v.data()), static_cast<std::streamsize>(d->v.size() * 4));
    return static_cast<bool>(f);
}

void WriteF32(const std::string& path, const Depth& d) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.v.data()), static_cast<std::streamsize>(d.v.size() * 4));
}

// The warp's inputs, as the capture's capture.txt gives them.
struct Params {
    uint32_t W = 0, H = 0, RW = 0, RH = 0;
    float c[40] = {};   // the shader's constants as floats (c[4] Scale, c[5] have-last, c[6] Far, c[8..] cameras)
};

bool ReadParams(const std::string& path, Params* p) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    bool inConst = false;
    int k = 0;
    while (std::getline(f, line)) {
        if (sscanf_s(line.c_str(), "W %u H %u RW %u RH %u", &p->W, &p->H, &p->RW, &p->RH) == 4) continue;
        if (line.rfind("constants", 0) == 0) { inConst = true; continue; }
        if (inConst) {
            std::istringstream s(line);
            float v;
            bool any = false;
            while (s >> v && k < 40) { p->c[k++] = v; any = true; }
            if (!any || k >= 40) inConst = false;
        }
    }
    return p->W && p->H && p->RW && p->RH && k == 40;
}

struct V3 { float x, y, z; };
V3 Vec(const float* c) { return { c[0], c[1], c[2] }; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 Mul(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }

// Hole fill variants.
struct Variant {
    const char* name;
    bool useLast = true;        // the warped eye's last real image (else the row fill only)
    bool depthCheck = true;     // reject last-image samples that were much nearer than the point (afw-edges, 2026-11-01)
    float threshold = 0.85f;    // ... nearer than threshold * the point's depth
    bool bothSides = true;      // the hole's background from the nearest filled pixel on each side (afw-edges)
};

// The CPU warp: passes 2-4 of the shader (pass 0's linear depth is the capture's).
Img Warp(const Img& src, const Depth& lin, const Img* last, const Depth* lastLin, const Params& p, const Variant& v, std::vector<uint8_t>* holeMask) {
    const uint32_t W = p.W, H = p.H, RW = p.RW, RH = p.RH;
    const float Scale = p.c[4], Far = p.c[6];
    const bool haveLast = p.c[5] > 0.5f && last;
    const V3 R1 = Vec(p.c + 8), F1 = Vec(p.c + 12), U1 = Vec(p.c + 16), R0 = Vec(p.c + 20), F0 = Vec(p.c + 24), U0 = Vec(p.c + 28), Delta = Vec(p.c + 32);
    const float TanH = p.c[35], TanV = p.c[36];
    const auto linAt = [&](uint32_t x, uint32_t y) {
        return lin.At((std::min)((x * RW + RW / 2) / W, RW - 1), (std::min)((y * RH + RH / 2) / H, RH - 1));
    };
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> Z(static_cast<size_t>(W) * H, inf);
    Img out;
    out.w = W; out.h = H;
    out.px.assign(static_cast<size_t>(W) * H * 3, 0);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const float z = linAt(x, y);
            const float xt = static_cast<float>(x) + 0.5f - Scale / z;
            if (xt < 0.0f || xt >= static_cast<float>(W)) continue;
            float& t = Z[static_cast<size_t>(y) * W + static_cast<uint32_t>(xt)];
            if (z <= t) { t = z; memcpy(out.At(static_cast<uint32_t>(xt), y), src.At(x, y), 3); }
        }
    if (holeMask) holeMask->assign(static_cast<size_t>(W) * H, 0);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            if (Z[static_cast<size_t>(y) * W + x] != inf) continue;
            if (holeMask) (*holeMask)[static_cast<size_t>(y) * W + x] = 1;
            int bestX = -1;
            float bestZ = -1.0f;
            if (v.bothSides) {   // the nearest filled pixel on each side, the farther of the two (afw-edges)
                bool doneL = false, doneR = false;
                for (int s = 1; s <= 768 && !(doneL && doneR); ++s) {
                    const int xl = static_cast<int>(x) - s, xr = static_cast<int>(x) + s;
                    if (!doneL) {
                        if (xl < 0) doneL = true;
                        else { const float f = Z[static_cast<size_t>(y) * W + xl]; if (f != inf) { doneL = true; if (f > bestZ) { bestZ = f; bestX = xl; } } }
                    }
                    if (!doneR) {
                        if (xr >= static_cast<int>(W)) doneR = true;
                        else { const float f = Z[static_cast<size_t>(y) * W + xr]; if (f != inf) { doneR = true; if (f > bestZ) { bestZ = f; bestX = xr; } } }
                    }
                }
            } else {   // 1.1.0: within 8 px once anything is found
                for (int s = 1; s <= 768; ++s) {
                    const int xl = static_cast<int>(x) - s, xr = static_cast<int>(x) + s;
                    if (xl >= 0) { const float f = Z[static_cast<size_t>(y) * W + xl]; if (f != inf && f > bestZ) { bestZ = f; bestX = xl; } }
                    if (xr < static_cast<int>(W)) { const float f = Z[static_cast<size_t>(y) * W + xr]; if (f != inf && f > bestZ) { bestZ = f; bestX = xr; } }
                    if (bestX >= 0 && s >= 8) break;
                }
            }
            uint8_t* o = out.At(x, y);
            if (getenv("AFW_BENCH_DEBUG") && y == H / 2 && (x % 4) == 0) printf("  hole x %u: bestX %d bestZ %.0f\n", x, bestX, bestZ);
            if (haveLast && v.useLast) {
                const float z = bestX >= 0 ? bestZ : Far;
                const float nx = (static_cast<float>(x) + 0.5f) / W * 2.0f - 1.0f, ny = 1.0f - (static_cast<float>(y) + 0.5f) / H * 2.0f;
                const V3 P = Add(Add(Add(Mul(R0, nx * TanH * z), Mul(F0, z)), Mul(U0, ny * TanV * z)), Mul(Delta, -1.0f));
                const float zz = Dot(P, F1);
                if (zz > 1.0f) {
                    const float px = (Dot(P, R1) / (zz * TanH) + 1.0f) * 0.5f * W;
                    const float py = (1.0f - Dot(P, U1) / (zz * TanV)) * 0.5f * H;
                    if (px >= 0.0f && py >= 0.0f && px < W && py < H) {
                        bool seen = true;
                        if (v.depthCheck && lastLin) {
                            const uint32_t qx = (std::min)(static_cast<uint32_t>(px) * RW / W, RW - 1), qy = (std::min)(static_cast<uint32_t>(py) * RH / H, RH - 1);
                            seen = lastLin->At(qx, qy) > v.threshold * zz;
                        }
                        if (seen) { memcpy(o, last->At(static_cast<uint32_t>(px), static_cast<uint32_t>(py)), 3); continue; }
                    }
                }
                if (bestX >= 0) memcpy(o, out.At(bestX, y), 3);
                else memcpy(o, last->At(x, y), 3);
                continue;
            }
            if (bestX >= 0) memcpy(o, out.At(bestX, y), 3);
            else memcpy(o, src.At(x, y), 3);
        }
    return out;
}

struct Score { double all = 0, holes = 0, nearEdges = 0, bad = 0; size_t nHoles = 0, nNear = 0; };

// Mean absolute error (0-255, mean of R, G, B) against the truth: everywhere, in the holes, and within 12 px of a depth
// edge where the near side is closer than 4 m (what players see round Lara, the bow, hands); and the share of pixels
// off by more than 24.
Score Compare(const Img& a, const Img& truth, const std::vector<uint8_t>& holes, const Depth& truthLin, const Params& p, Img* diff) {
    Score s;
    const uint32_t W = p.W, H = p.H;
    std::vector<uint8_t> nearEdge(static_cast<size_t>(W) * H, 0);
    const auto zAt = [&](uint32_t x, uint32_t y) {
        return truthLin.At((std::min)((x * p.RW + p.RW / 2) / W, p.RW - 1), (std::min)((y * p.RH + p.RH / 2) / H, p.RH - 1));
    };
    for (uint32_t y = 0; y < H; y += 1)
        for (uint32_t x = 1; x < W; ++x) {
            const float z0 = zAt(x - 1, y), z1 = zAt(x, y);
            const float n = (std::min)(z0, z1), f = (std::max)(z0, z1);
            if (n < 400.0f && f > 1.3f * n)
                for (int d = -12; d <= 12; ++d) {
                    const int xx = static_cast<int>(x) + d;
                    if (xx >= 0 && xx < static_cast<int>(W)) nearEdge[static_cast<size_t>(y) * W + xx] = 1;
                }
        }
    if (diff) { diff->w = W; diff->h = H; diff->px.assign(static_cast<size_t>(W) * H * 3, 0); }
    size_t bad = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t* u = a.At(x, y);
            const uint8_t* t = truth.At(x, y);
            const double e = (std::abs(u[0] - t[0]) + std::abs(u[1] - t[1]) + std::abs(u[2] - t[2])) / 3.0;
            s.all += e;
            if (e > 24.0) ++bad;
            const size_t i = static_cast<size_t>(y) * W + x;
            if (holes[i]) { s.holes += e; ++s.nHoles; }
            if (nearEdge[i]) { s.nearEdges += e; ++s.nNear; }
            if (diff) {
                const uint8_t g = static_cast<uint8_t>((std::min)(255.0, e * 4.0));
                uint8_t* o = diff->At(x, y);
                o[0] = g; o[1] = holes[i] ? g / 2 : 0; o[2] = nearEdge[i] ? 60 : 0;
            }
        }
    const double n = static_cast<double>(W) * H;
    s.all /= n;
    s.holes = s.nHoles ? s.holes / s.nHoles : 0;
    s.nearEdges = s.nNear ? s.nearEdges / s.nNear : 0;
    s.bad = 100.0 * bad / n;
    return s;
}

// ---- Synthetic capture: camera axes x right, y forward, z up; a far checkered wall and a near red box ----
void Synth(const std::string& dir) {
    CreateDirectoryA(dir.c_str(), nullptr);
    const uint32_t W = 960, H = 720, RW = 640, RH = 480;
    const float tanH = 0.9f, tanV = 0.675f, baseline = 6.4f, wall = 2000.0f, boxZ = 150.0f, boxHalf = 20.0f;
    // The rendered eye A at x = 0, the warped eye B at x = +baseline; in B's last real frame the box stood 8 cm further
    // right (it moves with the camera, as Lara does in third person).
    const auto render = [&](float camX, float boxX, Img* img, Depth* dep, uint32_t w, uint32_t h, uint32_t dw, uint32_t dh) {
        img->w = w; img->h = h; img->px.assign(static_cast<size_t>(w) * h * 3, 0);
        dep->w = dw; dep->h = dh; dep->v.assign(static_cast<size_t>(dw) * dh, 0.0f);
        const auto trace = [&](float nx, float ny, uint8_t* c, float* z) {
            const float dx = nx * tanH, dz = ny * tanV;   // ray (dx, 1, dz)
            const float bx = camX + dx * boxZ, bz = dz * boxZ;
            if (std::fabs(bx - boxX) < boxHalf && std::fabs(bz) < boxHalf) {
                *z = boxZ;
                if (c) { c[0] = 220; c[1] = 40; c[2] = 40; }
                return;
            }
            *z = wall;
            if (c) {
                const float wx = camX + dx * wall, wz = dz * wall;
                const bool chk = (static_cast<int>(std::floor(wx / 50.0f)) + static_cast<int>(std::floor(wz / 50.0f))) & 1;
                c[0] = chk ? 200 : 60; c[1] = chk ? 200 : 90; c[2] = chk ? 210 : 140;
            }
        };
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                float z;
                trace((x + 0.5f) / w * 2.0f - 1.0f, 1.0f - (y + 0.5f) / h * 2.0f, img->At(x, y), &z);
            }
        for (uint32_t y = 0; y < dh; ++y)
            for (uint32_t x = 0; x < dw; ++x) {
                float z;
                trace((x + 0.5f) / dw * 2.0f - 1.0f, 1.0f - (y + 0.5f) / dh * 2.0f, nullptr, &z);
                dep->v[static_cast<size_t>(y) * dw + x] = z;
            }
    };
    Img rendered, truth, last;
    Depth rd, td, ld;
    render(0.0f, 0.0f, &rendered, &rd, W, H, RW, RH);
    render(baseline, 0.0f, &truth, &td, W, H, RW, RH);
    render(baseline, 8.0f, &last, &ld, W, H, RW, RH);
    // As the game's pass 0 does: the nearest depth of each 3x3 neighbourhood, so foreground edges grow by one depth
    // pixel and the colour edge moves with the object.
    for (Depth* d : { &rd, &td, &ld }) {
        const Depth in = *d;
        for (uint32_t y = 0; y < d->h; ++y)
            for (uint32_t x = 0; x < d->w; ++x) {
                float m = in.At(x, y);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = (std::min)((std::max)(static_cast<int>(x) + dx, 0), static_cast<int>(d->w) - 1);
                        const int yy = (std::min)((std::max)(static_cast<int>(y) + dy, 0), static_cast<int>(d->h) - 1);
                        m = (std::min)(m, in.At(xx, yy));
                    }
                d->v[static_cast<size_t>(y) * d->w + x] = m;
            }
    }
    WriteBmp(dir + "\\rendered.bmp", rendered);
    WriteBmp(dir + "\\truth.bmp", truth);
    WriteBmp(dir + "\\fill_last_real.bmp", last);
    WriteF32(dir + "\\rendered_depth.f32", rd);
    WriteF32(dir + "\\truth_depth.f32", td);
    WriteF32(dir + "\\fill_last_depth.f32", ld);
    float c[40] = {};
    c[4] = (0.5f * W / tanH) * baseline;   // Scale
    c[5] = 1.0f;                           // have the last image
    c[6] = wall * 2.0f;                    // Far
    const float R[3] = { 1, 0, 0 }, F[3] = { 0, 1, 0 }, U[3] = { 0, 0, 1 };
    memcpy(c + 8, R, 12); memcpy(c + 12, F, 12); memcpy(c + 16, U, 12);
    memcpy(c + 20, R, 12); memcpy(c + 24, F, 12); memcpy(c + 28, U, 12);
    c[35] = tanH; c[36] = tanV; c[37] = 1.0f;
    std::ofstream m(dir + "\\capture.txt");
    m << "synthetic: a near box (150 cm) moved 8 cm since the warped eye's last frame, a wall at 20 m\n";
    m << "W " << W << " H " << H << " RW " << RW << " RH " << RH << "\n";
    m << "constants (as floats; see the shader's cbuffer C):";
    for (int i = 0; i < 40; ++i) m << (i % 8 == 0 ? "\n" : " ") << c[i];
    m << "\n";
    printf("synthetic capture written to %s\n", dir.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--synth") { Synth(argv[2]); return 0; }
    if (argc < 2) { printf("usage: vrmod_afw_bench <capture folder> [out folder] | --synth <folder>\n"); return 2; }
    const std::string dir = argv[1], out = argc > 2 ? argv[2] : dir;
    Params p;
    if (!ReadParams(dir + "\\capture.txt", &p)) { printf("capture.txt unreadable or incomplete\n"); return 1; }
    Img rendered, warped, truth, last;
    Depth rd, td, ld;
    if (!ReadBmp(dir + "\\rendered.bmp", &rendered) || !ReadBmp(dir + "\\truth.bmp", &truth) || !ReadF32(dir + "\\rendered_depth.f32", p.RW, p.RH, &rd)
        || !ReadF32(dir + "\\truth_depth.f32", p.RW, p.RH, &td)) {
        printf("rendered / truth images or depths missing\n");
        return 1;
    }
    const bool haveLast = ReadBmp(dir + "\\fill_last_real.bmp", &last);
    const bool haveLastDepth = ReadF32(dir + "\\fill_last_depth.f32", p.RW, p.RH, &ld);
    const bool haveWarped = ReadBmp(dir + "\\warped.bmp", &warped);
    printf("capture %s: %ux%u (depth %ux%u), last image %s, last depth %s\n", dir.c_str(), p.W, p.H, p.RW, p.RH, haveLast ? "yes" : "no",
           haveLastDepth ? "yes" : "no");
    CreateDirectoryA(out.c_str(), nullptr);
    std::vector<uint8_t> holes;
    {   // the reference: the rendered eye as it is, no warp (what plain alternate-eye shows for this eye)
        Warp(rendered, rd, nullptr, nullptr, p, Variant{ "probe", false, false }, &holes);
        Img diff;
        const Score s = Compare(rendered, truth, holes, td, p, &diff);
        printf("%-34s all %6.2f | holes %6.2f | near edges %6.2f | off >24: %5.2f %%\n", "no warp (the other eye's image)", s.all, s.holes, s.nearEdges, s.bad);
    }
    if (haveWarped) {
        Img diff;
        const Score s = Compare(warped, truth, holes, td, p, &diff);
        printf("%-34s all %6.2f | holes %6.2f | near edges %6.2f | off >24: %5.2f %%\n", "the game's warp (captured)", s.all, s.holes, s.nearEdges, s.bad);
        WriteBmp(out + "\\diff_game.bmp", diff);
    }
    const Variant variants[] = {
        { "1.1.0 (unchecked, 8 px search)", true, false, 0.85f, false },
        { "row fill only, both sides", false, false, 0.85f, true },
        { "last image, both sides, unchecked", true, false, 0.85f, true },
        { "last image, both sides, check 0.85", true, true, 0.85f, true },
        { "last image, both sides, check 0.95", true, true, 0.95f, true },
        { "last image, both sides, check 0.70", true, true, 0.70f, true },
    };
    for (const Variant& v : variants) {
        if (v.useLast && !haveLast) continue;
        if (v.depthCheck && !haveLastDepth) continue;
        std::vector<uint8_t> hm;
        const Img w = Warp(rendered, rd, haveLast ? &last : nullptr, haveLastDepth ? &ld : nullptr, p, v, &hm);
        Img diff;
        const Score s = Compare(w, truth, hm, td, p, &diff);
        printf("%-34s all %6.2f | holes %6.2f | near edges %6.2f | off >24: %5.2f %%  (%zu hole px)\n", v.name, s.all, s.holes, s.nearEdges, s.bad, s.nHoles);
        std::string file = v.name;
        for (char& ch : file) if (!isalnum(static_cast<unsigned char>(ch))) ch = '_';
        WriteBmp(out + "\\warp_" + file + ".bmp", w);
        WriteBmp(out + "\\diff_" + file + ".bmp", diff);
    }
    return 0;
}
