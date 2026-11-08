// Égaliseur 10 bandes : filtres « peaking » de l'Audio EQ Cookbook (R. Bristow-Johnson), un par bande, en série.
// Les réglages sont partagés ; chaque flux audio (moteur Windows, piste Mac) a son propre Filter (état par canal).
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace eq {
constexpr int N = 10;
constexpr float kFreq[N] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

struct Shared {
    std::mutex mx;
    float gain[N] = {};  // dB, -12..+12
    bool on = false;
    std::atomic<int> version{0};
};
inline Shared& shared() { static Shared s; return s; }
inline void set(const float* g, bool on) {
    Shared& s = shared();
    { std::lock_guard lk(s.mx); std::copy(g, g + N, s.gain); s.on = on; }
    s.version++;
}

struct Filter {
    int version = -1, rate = 0, ch = 0;
    bool on = false, active[N] = {};
    float pre = 1;  // baisse globale = le plus fort gain positif : pas de saturation
    double b[N][3] = {}, a[N][2] = {};
    std::vector<double> z;  // [canal][bande][2]

    void reset() { std::fill(z.begin(), z.end(), 0.0); }
    void update(int sr, int nch) {  // thread audio : ne recalcule que si les réglages ou le format changent
        Shared& s = shared();
        int v = s.version;
        if (v == version && sr == rate && nch == ch) return;
        float g[N];
        { std::lock_guard lk(s.mx); std::copy(s.gain, s.gain + N, g); on = s.on; }
        if (sr != rate || nch != ch) z.assign((size_t)nch * N * 2, 0.0);  // gains seuls : on garde l'état (pas de clic)
        version = v; rate = sr; ch = nch;
        float mx = 0;
        for (float x : g) mx = std::max(mx, x);
        pre = std::pow(10.f, -mx / 20.f);
        for (int i = 0; i < N; i++) {
            active[i] = std::fabs(g[i]) > 0.05f && kFreq[i] < sr * 0.45;
            double A = std::pow(10.0, g[i] / 40.0), w = 2 * 3.14159265358979 * kFreq[i] / sr, al = std::sin(w) / (2 * 1.41), c = std::cos(w), a0 = 1 + al / A;
            b[i][0] = (1 + al * A) / a0; b[i][1] = -2 * c / a0; b[i][2] = (1 - al * A) / a0;
            a[i][0] = -2 * c / a0; a[i][1] = (1 - al / A) / a0;
        }
    }
    float run(float in, int c) {  // un échantillon d'un canal, toutes les bandes (forme directe II transposée)
        double v = in * pre, *zz = &z[(size_t)c * N * 2];
        for (int i = 0; i < N; i++, zz += 2) {
            if (!active[i]) continue;
            double y = b[i][0] * v + zz[0];
            zz[0] = b[i][1] * v - a[i][0] * y + zz[1];
            zz[1] = b[i][2] * v - a[i][1] * y;
            v = y;
        }
        return (float)v;
    }
    void process(float* x, size_t frames, int sr, int nch) {  // échantillons entrelacés (Windows)
        if (nch <= 0 || sr <= 0) return;
        update(sr, nch);
        if (!on) return;
        for (size_t f = 0; f < frames; f++) for (int c = 0; c < nch; c++) x[f * nch + c] = run(x[f * nch + c], c);
    }
    void processPlanar(float** bufs, size_t frames, int sr, int nch) {  // un tampon par canal (Mac)
        if (nch <= 0 || sr <= 0) return;
        update(sr, nch);
        if (!on) return;
        for (int c = 0; c < nch; c++) for (size_t f = 0; f < frames; f++) bufs[c][f] = run(bufs[c][f], c);
    }
};
}  // namespace eq
