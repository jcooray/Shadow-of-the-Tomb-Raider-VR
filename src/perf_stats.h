#pragma once

#include <windows.h>
#include <sstream>
#include <string>

namespace vrmod {

// Tiny per-phase timing accumulator (avg / max / count over slow frames), used to
// find out *which* wait a frame-rate dip is spent in. Not thread-safe by design:
// every user calls it from the game's render thread inside hkPresent.
struct PhaseStat {
    double sumMs = 0.0;
    double maxMs = 0.0;
    unsigned long long n = 0;
    unsigned long long over8 = 0;   // samples slower than 8 ms

    void Add(double ms) {
        sumMs += ms;
        if (ms > maxMs) maxMs = ms;
        if (ms > 8.0) ++over8;
        ++n;
    }
    std::string Summary() const {
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(2);
        o << (n ? sumMs / static_cast<double>(n) : 0.0) << "/" << maxMs << "ms(>8:" << over8 << ")";
        return o.str();
    }
    void Reset() { *this = PhaseStat{}; }
};

inline double NowMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

} // namespace vrmod
