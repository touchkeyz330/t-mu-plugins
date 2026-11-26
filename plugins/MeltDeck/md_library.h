// md_library.h - Melt Deck: reading tracks from the MPC's own drive.
//
// SPDX-License-Identifier: MIT
//
//  * Finds a folder named "Melt Deck Music" at the top of any drive.
//  * Decodes WAV (8/16/24/32-bit, float) and AIFF/AIFC, any sample rate.
//  * Works out BPM, beat grid, key, loudness and start point on the MPC itself
//    (the same method as the Mac builder) and keeps the results in a small cache
//    file inside that folder, so every track is only analysed once.
#pragma once

#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "md_mp3.h"

#include <memory>
#if defined(_WIN32)
  #include <chrono>
  #include <filesystem>
  #include <system_error>
#else
  #include <dirent.h>
  #include <sys/stat.h>
  #include <climits>
#endif

namespace mdlib {

// ------------------------------------------------------------------ files (portable: MPC, macOS, Windows)
// Paths always use '/', which Windows accepts too.
inline std::FILE* openFile(const std::string& path, const char* mode)
{
#if defined(MPC_DESKTOP)
    return mpc::openFile(path, mode);
#else
    return std::fopen(path.c_str(), mode);
#endif
}
struct FileStat { bool ok = false, dir = false; long size = 0, mtime = 0; };
#if defined(_WIN32)
inline std::filesystem::path fsPath(std::string p)
{
    if (p.size() == 2 && p[1] == ':') p += '/';                // "C:" means the top of drive C
    return std::filesystem::u8path(p);
}
inline std::string fromFs(const std::filesystem::path& p)
{
    std::string s = p.u8string();
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}
inline FileStat statPath(const std::string& path)
{
    FileStat r; std::error_code ec;
    const std::filesystem::path p = fsPath(path);
    const auto st = std::filesystem::status(p, ec);
    if (ec || !std::filesystem::exists(st)) return r;
    r.ok = true; r.dir = std::filesystem::is_directory(st);
    if (!r.dir) r.size = (long)std::min<uintmax_t>(std::filesystem::file_size(p, ec), 0x7fffffff);
    const auto t = std::filesystem::last_write_time(p, ec);
    if (!ec) r.mtime = (long)(std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count() - 11644473600LL);
    return r;
}
#else
inline FileStat statPath(const std::string& path)
{
    FileStat r; struct stat s;
    if (::stat(path.c_str(), &s) != 0) return r;
    r.ok = true; r.dir = S_ISDIR(s.st_mode); r.size = (long)s.st_size; r.mtime = (long)s.st_mtime;
    return r;
}
#endif
inline std::string realPath(const std::string& p)
{
#if defined(_WIN32)
    std::error_code ec;
    const auto c = std::filesystem::weakly_canonical(fsPath(p), ec);
    return ec ? p : fromFs(c);
#else
    char real[PATH_MAX + 1];
    return ::realpath(p.c_str(), real) ? std::string(real) : p;
#endif
}
// the user's home folder and the usual places music lives (desktop builds only)
inline std::string homeFolder()
{
#if defined(MPC_DESKTOP)
  #if defined(_WIN32)
    const char* h = std::getenv("USERPROFILE");
  #else
    const char* h = std::getenv("HOME");
  #endif
    std::string s = h ? h : "";
    for (char& c : s) if (c == '\\') c = '/';
    return s;
#else
    return std::string();
#endif
}

// ------------------------------------------------------------------ names
inline std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// Keep what the MPC screen can draw: drop emoji / symbols, tidy spaces and '_'.
inline std::string cleanName(const std::string& in)
{
    std::string out;
    for (size_t i = 0; i < in.size();)
    {
        const unsigned char c = (unsigned char)in[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + (size_t)len > in.size()) break;
        uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < len; ++k) cp = (cp << 6) | ((unsigned char)in[i + (size_t)k] & 0x3F);
        if (cp >= 32 && cp != 127 && (cp < 0x2000 || (cp >= 0x2010 && cp <= 0x2027)) && std::strchr("\"#@,;:<>*^|?\\/\t", (int)cp) == nullptr)
            out.append(in, i, (size_t)len);
        i += (size_t)len;
    }
    for (char& c : out) if (c == '_') c = ' ';
    std::string tidy;
    for (char c : out) { if (c == ' ' && (tidy.empty() || tidy.back() == ' ')) continue; tidy += c; }
    while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
    return tidy.empty() ? std::string("Track") : tidy;
}

// ------------------------------------------------------------------ decoding
struct Decoded { std::vector<int16_t> data; long frames = 0; float rate = 44100; };

inline uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]; }
inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

inline double ext80(const uint8_t* p)
{
    const int e = ((p[0] & 0x7F) << 8 | p[1]) - 16383;
    uint64_t m = 0;
    for (int i = 0; i < 8; ++i) m = (m << 8) | p[2 + i];
    return std::ldexp((double)m, e - 63);
}

// sample format: 0 = signed int, 1 = unsigned 8-bit, 2 = float
inline bool convertBlock(std::FILE* f, long frames, int ch, int bytes, int kind, bool bigEndian, Decoded& out)
{
    const long maxFrames = 44100L * 60 * 30 * 2;               // safety: 30 minutes at 88.2 kHz
    frames = std::min(frames, maxFrames);
    out.data.assign((size_t)frames * 2, 0);
    const int frameBytes = ch * bytes;
    std::vector<uint8_t> buf((size_t)frameBytes * 8192);
    long done = 0;
    while (done < frames)
    {
        const long n = std::min(8192L, frames - done);
        const size_t got = std::fread(buf.data(), (size_t)frameBytes, (size_t)n, f);
        if (got == 0) break;
        for (size_t i = 0; i < got; ++i)
        {
            for (int c = 0; c < 2; ++c)
            {
                const uint8_t* p = buf.data() + i * (size_t)frameBytes + (size_t)(std::min(c, ch - 1) * bytes);
                double v = 0;
                if (kind == 2)
                {
                    if (bytes == 4) { uint32_t u = bigEndian ? be32(p) : le32(p); float x; std::memcpy(&x, &u, 4); v = x; }
                    else { uint64_t u = 0; for (int k = 0; k < 8; ++k) u |= (uint64_t)p[bigEndian ? k : 7 - k] << (8 * (7 - k)); double x; std::memcpy(&x, &u, 8); v = x; }
                }
                else if (kind == 1) v = ((int)p[0] - 128) / 128.0;
                else
                {
                    int32_t s = 0;
                    for (int k = 0; k < bytes; ++k) s |= (int32_t)p[bigEndian ? k : bytes - 1 - k] << (8 * (bytes - 1 - k));
                    s <<= (32 - 8 * bytes);                        // sign-extend
                    v = s / 2147483648.0;
                }
                const double q = std::round(std::max(-1.0, std::min(1.0, v)) * 32767.0);
                out.data[(size_t)(done + (long)i) * 2 + (size_t)c] = (int16_t)q;
            }
        }
        done += (long)got;
        if ((long)got < n) break;
    }
    out.frames = done;
    out.data.resize((size_t)done * 2 + 16, 0);
    return done > 0;
}

inline bool isMp3Name(const std::string& p) { const std::string l = lower(p); return l.size() > 4 && l.compare(l.size() - 4, 4, ".mp3") == 0; }

inline bool decodeMp3(const std::string& path, Decoded& out)
{
    std::FILE* f = openFile(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 400L * 1024 * 1024) { std::fclose(f); return false; }
    std::vector<uint8_t> bytes((size_t)n);
    const size_t got = std::fread(bytes.data(), 1, (size_t)n, f);
    std::fclose(f);
    std::unique_ptr<mdmp3::Decoder> dec(new mdmp3::Decoder());
    long frames = 0; int rate = 0;
    if (!dec->decode(bytes.data(), got, out.data, frames, rate)) return false;
    out.frames = frames; out.rate = (float)rate;
    out.data.resize((size_t)frames * 2 + 16, 0);
    return true;
}

inline bool decodeFile(const std::string& path, Decoded& out)
{
    if (isMp3Name(path)) return decodeMp3(path, out);
    std::FILE* f = openFile(path, "rb");
    if (!f) return false;
    uint8_t h[12];
    bool ok = false;
    if (std::fread(h, 1, 12, f) == 12)
    {
        if (!std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "WAVE", 4))
        {
            int tag = 0, ch = 0, bits = 0; uint32_t rate = 0;
            uint8_t c[8];
            while (std::fread(c, 1, 8, f) == 8)
            {
                const uint32_t size = le32(c + 4);
                if (!std::memcmp(c, "fmt ", 4))
                {
                    std::vector<uint8_t> b(size);
                    if (std::fread(b.data(), 1, size, f) != size || size < 16) break;
                    tag = le16(&b[0]); ch = le16(&b[2]); rate = le32(&b[4]); bits = le16(&b[14]);
                    if (tag == 0xFFFE && size >= 26) tag = le16(&b[24]);
                    if (size & 1) std::fseek(f, 1, SEEK_CUR);
                }
                else if (!std::memcmp(c, "data", 4))
                {
                    if (ch < 1 || rate < 8000 || bits < 8) break;
                    const int bytes = bits / 8;
                    const int kind = tag == 3 ? 2 : (bits == 8 ? 1 : 0);
                    if (tag != 1 && tag != 3) break;
                    ok = convertBlock(f, (long)(size / (uint32_t)(bytes * ch)), ch, bytes, kind, false, out);
                    out.rate = (float)rate;
                    break;
                }
                else std::fseek(f, (long)(size + (size & 1)), SEEK_CUR);
            }
        }
        else if (!std::memcmp(h, "FORM", 4) && (!std::memcmp(h + 8, "AIFF", 4) || !std::memcmp(h + 8, "AIFC", 4)))
        {
            const bool aifc = !std::memcmp(h + 8, "AIFC", 4);
            int ch = 0, bits = 0, kind = 0; bool big = true; double rate = 0; long frames = 0;
            uint8_t c[8];
            while (std::fread(c, 1, 8, f) == 8)
            {
                const uint32_t size = be32(c + 4);
                if (!std::memcmp(c, "COMM", 4))
                {
                    std::vector<uint8_t> b(size);
                    if (std::fread(b.data(), 1, size, f) != size || size < 18) break;
                    ch = be16(&b[0]); frames = (long)be32(&b[2]); bits = be16(&b[6]); rate = ext80(&b[8]);
                    if (aifc && size >= 22)
                    {
                        if (!std::memcmp(&b[18], "sowt", 4)) big = false;
                        else if (!std::memcmp(&b[18], "fl32", 4) || !std::memcmp(&b[18], "FL32", 4)) kind = 2;
                        else if (!std::memcmp(&b[18], "fl64", 4) || !std::memcmp(&b[18], "FL64", 4)) { kind = 2; bits = 64; }
                        else if (std::memcmp(&b[18], "NONE", 4) && std::memcmp(&b[18], "twos", 4)) break;
                    }
                    if (size & 1) std::fseek(f, 1, SEEK_CUR);
                }
                else if (!std::memcmp(c, "SSND", 4))
                {
                    uint8_t o[8];
                    if (std::fread(o, 1, 8, f) != 8 || ch < 1 || rate < 8000) break;
                    std::fseek(f, (long)be32(o), SEEK_CUR);
                    const int bytes = (bits + 7) / 8;
                    ok = convertBlock(f, frames, ch, bytes, kind, big, out);
                    out.rate = (float)rate;
                    break;
                }
                else std::fseek(f, (long)(size + (size & 1)), SEEK_CUR);
            }
        }
    }
    std::fclose(f);
    return ok;
}

// ------------------------------------------------------------------ analysis
struct Analysis { float bpm = 120, first = 0, start = 0, length = 0, gain = 0; char key[12] = {0}; };

inline void fft(std::vector<std::complex<float>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2 * M_PI / (double)len;
        const std::complex<float> wl((float)std::cos(ang), (float)std::sin(ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<float> w(1, 0);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// trailing moving average, centred like numpy's convolve(..., "same")
inline std::vector<float> movingAvg(const std::vector<float>& x, int w)
{
    const long n = (long)x.size();
    std::vector<float> out((size_t)n, 0.0f);
    std::vector<double> cs((size_t)n + 1, 0.0);
    for (long i = 0; i < n; ++i) cs[(size_t)i + 1] = cs[(size_t)i] + x[(size_t)i];
    const long lo = (w - 1) / 2, hi = w / 2;
    for (long i = 0; i < n; ++i)
    {
        const long a = std::max(0L, i - hi), b = std::min(n, i + lo + 1);
        out[(size_t)i] = (float)((cs[(size_t)b] - cs[(size_t)a]) / w);
    }
    return out;
}

inline float parseBpmFromName(const std::string& stem)
{
    const std::string s = lower(stem);
    for (size_t p = s.find("bpm"); p != std::string::npos; p = s.find("bpm", p + 1))
    {
        long e = (long)p - 1;
        while (e >= 0 && (s[(size_t)e] == ' ' || s[(size_t)e] == '-')) --e;
        long b = e;
        while (b >= 0 && (std::isdigit((unsigned char)s[(size_t)b]) || s[(size_t)b] == '.')) --b;
        if (e > b)
        {
            const float v = (float)std::atof(s.substr((size_t)(b + 1), (size_t)(e - b)).c_str());
            if (v >= 50 && v <= 220) return v;
        }
    }
    return 0;
}

inline void analyse(const int16_t* stereo, long frames, float rate, const std::string& stem, Analysis& out)
{
    out.length = (float)frames / rate;
    // mono and peak
    std::vector<float> mono((size_t)frames);
    float pk = 1e-9f;
    for (long i = 0; i < frames; ++i)
    {
        const float v = (stereo[(size_t)i * 2] + stereo[(size_t)i * 2 + 1]) * (0.5f / 32768.0f);
        mono[(size_t)i] = v; pk = std::max(pk, std::fabs(v));
    }
    long st = 0;
    while (st < frames && std::fabs(mono[(size_t)st]) <= pk * 0.01f) ++st;
    out.start = std::max(0.0f, (float)st / rate - 0.005f);
    // loudness: louder half of 0.4 s blocks
    {
        const long blk = (long)(rate * 0.4f), nb = frames / std::max(1L, blk);
        if (nb >= 2)
        {
            std::vector<double> r((size_t)nb);
            for (long b = 0; b < nb; ++b)
            {
                double s = 0;
                for (long i = b * blk; i < (b + 1) * blk; ++i) s += (double)mono[(size_t)i] * mono[(size_t)i];
                r[(size_t)b] = s / (double)blk + 1e-12;
            }
            std::sort(r.begin(), r.end());
            double m = 0; long c = 0;
            for (long b = nb / 2; b < nb; ++b) { m += r[(size_t)b]; ++c; }
            const double level = 10.0 * std::log10(m / (double)c);
            out.gain = (float)std::round(std::max(-12.0, std::min(12.0, -12.0 - level)) * 10.0) / 10.0f;
        }
    }
    // decimate to ~11 kHz (first 6 minutes)
    const int dec = std::max(1, (int)std::lround(rate / 11025.0f));
    const double SR = (double)rate / dec;
    const long n6 = std::min(frames, (long)(rate * 360));
    std::vector<float> y((size_t)(n6 / dec));
    for (size_t i = 0; i < y.size(); ++i)
    {
        float s = 0;
        for (int k = 0; k < dec; ++k) s += mono[i * (size_t)dec + (size_t)k];
        y[i] = s / dec;
    }
    // ---- onset envelope
    const int NFFT = 1024, HOP = 128;
    const double FPS = SR / HOP;
    const long nfr = ((long)y.size() - NFFT) / HOP;
    out.bpm = 120; out.first = 0;
    if (nfr >= 64)
    {
        std::vector<float> env((size_t)nfr, 0.0f), prev(300, 0.0f), win((size_t)NFFT);
        for (int i = 0; i < NFFT; ++i) win[(size_t)i] = 0.5f - 0.5f * std::cos(2.0f * (float)M_PI * i / (NFFT - 1));
        std::vector<std::complex<float>> buf((size_t)NFFT);
        for (long fr = 0; fr < nfr; ++fr)
        {
            for (int i = 0; i < NFFT; ++i) buf[(size_t)i] = { y[(size_t)(fr * HOP + i)] * win[(size_t)i], 0.0f };
            fft(buf);
            float e = 0;
            for (int b = 0; b < 300; ++b)
            {
                const float m = std::log1p(1000.0f * std::abs(buf[(size_t)b]));
                if (fr > 0 && m > prev[(size_t)b]) e += m - prev[(size_t)b];
                prev[(size_t)b] = m;
            }
            env[(size_t)fr] = e;
        }
        {
            const int w = std::max(1, std::min((int)FPS, (int)nfr));
            const std::vector<float> avg = movingAvg(env, w);
            for (long i = 0; i < nfr; ++i) env[(size_t)i] = std::max(0.0f, env[(size_t)i] - avg[(size_t)i]);
        }
        // ---- tempo by autocorrelation
        double mean = 0;
        for (float v : env) mean += v;
        mean /= (double)nfr;
        const long maxLag = std::min(nfr - 2, (long)std::ceil(4 * FPS) + 3);
        std::vector<double> ac((size_t)maxLag + 2, 0.0);
        for (long L = 0; L <= maxLag + 1 && L < nfr; ++L)
        {
            double s = 0;
            for (long t = 0; t + L < nfr; ++t) s += (env[(size_t)t] - mean) * (env[(size_t)(t + L)] - mean);
            ac[(size_t)L] = s;
        }
        double best = 120, bestS = -1e300;
        for (double bpm = 60.0; bpm < 200.0; bpm += 0.5)
        {
            const double lag = 60.0 * FPS / bpm;
            double s = 0;
            for (int m : { 1, 2, 4 })
            {
                const double L = lag * m;
                if (L >= (double)maxLag) continue;
                const long i = (long)L; const double fr = L - (double)i;
                s += (ac[(size_t)i] * (1 - fr) + ac[(size_t)i + 1] * fr) / m;
            }
            const double g = std::log2(bpm / 120.0) / 0.9;
            s *= std::exp(-0.5 * g * g);
            if (s > bestS) { bestS = s; best = bpm; }
        }
        while (best < 80) best *= 2;
        while (best >= 160) best /= 2;
        double envSum = 1e-9;
        for (float v : env) envSum += v;
        auto comb = [&](double bpm, double& off) {
            const double P = 60.0 * FPS / bpm;
            double h[96] = {0};
            for (long t = 0; t < nfr; ++t) { const double ph = std::fmod((double)t, P) / P; h[(int)(ph * 96) % 96] += env[(size_t)t]; }
            int j = 0; double hb = -1;
            for (int k = 0; k < 96; ++k) { const double v = h[k] + 0.5 * h[(k + 95) % 96] + 0.5 * h[(k + 1) % 96]; if (v > hb) { hb = v; j = k; } }
            off = (j + 0.5) / 96.0 * P;
            return hb / envSum;
        };
        double bpm = best, top = -1, off = 0;
        for (double b = best * 0.985; b < best * 1.015; b += 0.01) { double o; const double s = comb(b, o); if (s > top) { top = s; bpm = b; } }
        const double r = std::round(bpm);
        double o2;
        if (std::fabs(bpm - r) < 0.35 && comb(r, o2) >= 0.97 * top) bpm = r;
        else bpm = std::round(bpm * 100) / 100;
        comb(bpm, off);
        double first = off / FPS + (NFFT / 2.0 - HOP / 2.0) / SR;
        const float named = parseBpmFromName(stem);
        if (named > 0)
        {
            bpm = named;
            const double P = 60.0 / bpm;
            double h[96] = {0};
            for (long t = 0; t < nfr; ++t)
            {
                const double tt = t / FPS + (NFFT / 2.0 - HOP / 2.0) / SR;
                h[(int)(std::fmod(tt, P) / P * 96) % 96] += env[(size_t)t];
            }
            int j = (int)(std::max_element(h, h + 96) - h);
            first = (j + 0.5) / 96.0 * P;
        }
        const double P = 60.0 / bpm;
        first = std::fmod(first, P);
        // ---- refine the grid on the kick (zero-phase low-pass 150 Hz)
        {
            const long n = (long)y.size();
            std::vector<float> low(y);
            const double w0 = 2 * M_PI * 150.0 / SR, al = std::sin(w0) / (2 * 0.7071), cw = std::cos(w0);
            const double b0 = (1 - cw) / 2 / (1 + al), b1 = (1 - cw) / (1 + al), a1 = -2 * cw / (1 + al), a2 = (1 - al) / (1 + al);
            for (int pass = 0; pass < 2; ++pass)
            {
                double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
                for (long k = 0; k < n; ++k)
                {
                    const long i = pass == 0 ? k : n - 1 - k;
                    const double x = low[(size_t)i];
                    const double o = b0 * x + b1 * x1 + b0 * x2 - a1 * y1 - a2 * y2;
                    x2 = x1; x1 = x; y2 = y1; y1 = o;
                    low[(size_t)i] = (float)o;
                }
            }
            for (float& v : low) v = v * v;
            std::vector<float> e2 = movingAvg(low, std::max(1, (int)(SR * 0.010)));
            float emax = 1e-12f;
            for (float& v : e2) { v = std::sqrt(std::max(0.0f, v)); emax = std::max(emax, v); }
            std::vector<float> d((size_t)n, 0.0f);
            for (long i = 1; i < n; ++i) d[(size_t)i] = std::max(0.0f, e2[(size_t)i] - e2[(size_t)i - 1]);
            const long nb = (long)(((double)n / SR - first) / P) - 1;
            if (nb >= 4)
            {
                const long stepb = std::max(1L, nb / 256);
                auto score = [&](double ph) {
                    double s = 0; const long rr = (long)(0.03 * SR);
                    for (long b = 0; b < nb; b += stepb)
                    {
                        const long c = (long)((ph + b * P) * SR);
                        if (c - rr >= 0 && c + rr < n) s += *std::max_element(d.begin() + (c - rr), d.begin() + (c + rr));
                    }
                    return s;
                };
                const double alt = std::fmod(first + P / 2, P);
                if (score(alt) > 1.15 * score(first)) first = alt;
                for (int it = 0; it < 2; ++it)
                {
                    std::vector<double> res;
                    const long rr = (long)(0.06 * SR);
                    for (long b = 0; b < nb; b += stepb)
                    {
                        const long c = (long)((first + b * P) * SR);
                        if (c - rr < 0 || c + rr >= n) continue;
                        float wp = 0;
                        for (long i = c - rr; i < c + rr; ++i) wp = std::max(wp, e2[(size_t)i]);
                        if (wp > 0.25f * emax && e2[(size_t)(c - rr)] < 0.5f * wp)
                        {
                            long i = c - rr;
                            while (e2[(size_t)i] <= 0.5f * wp) ++i;
                            res.push_back((double)(i - c) / SR);
                        }
                    }
                    if (res.size() >= 4)
                    {
                        std::nth_element(res.begin(), res.begin() + (long)res.size() / 2, res.end());
                        first = std::fmod(first + res[res.size() / 2] + P, P);
                    }
                }
            }
        }
        out.bpm = (float)bpm; out.first = (float)first;
    }
    // ---- key (chroma + Krumhansl profiles), 20 s - 200 s
    {
        static const double maj[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
        static const double mnr[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
        static const char* names[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
        static const char* cmaj[12] = {"8B", "3B", "10B", "5B", "12B", "7B", "2B", "9B", "4B", "11B", "6B", "1B"};
        static const char* cmin[12] = {"5A", "12A", "7A", "2A", "9A", "4A", "11A", "6A", "1A", "8A", "3A", "10A"};
        long a0 = 0, a1 = (long)y.size();
        if (out.length > 60) { a0 = (long)(20 * SR); a1 = std::min(a1, (long)(200 * SR)); }
        const int N = 8192;
        double chroma[12] = {0};
        if (a1 - a0 >= 2 * N)
        {
            std::vector<std::complex<float>> buf((size_t)N);
            std::vector<int> pc((size_t)N / 2, -1);
            for (int k = 1; k < N / 2; ++k)
            {
                const double fq = k * SR / N;
                if (fq > 60 && fq < 2000) pc[(size_t)k] = (int)(((long)std::lround(12 * std::log2(fq / 261.63)) % 12 + 12) % 12);
            }
            for (long a = a0; a + N <= a1; a += 2 * N)
            {
                for (int i = 0; i < N; ++i) buf[(size_t)i] = { y[(size_t)(a + i)] * (0.5f - 0.5f * std::cos(2.0f * (float)M_PI * i / (N - 1))), 0.0f };
                fft(buf);
                for (int k = 1; k < N / 2; ++k) if (pc[(size_t)k] >= 0) chroma[pc[(size_t)k]] += std::sqrt(std::abs(buf[(size_t)k]));
            }
            double m = 0, sd = 0;
            for (double v : chroma) m += v;
            m /= 12;
            for (double v : chroma) sd += (v - m) * (v - m);
            sd = std::sqrt(sd / 12) + 1e-9;
            if (m > 0)
            {
                double best = -9; int br = 0; bool bmin = false;
                for (int r = 0; r < 12; ++r)
                    for (int mi = 0; mi < 2; ++mi)
                    {
                        const double* prof = mi ? mnr : maj;
                        double pm = 0, ps = 0, cov = 0;
                        for (int k = 0; k < 12; ++k) pm += prof[k];
                        pm /= 12;
                        for (int k = 0; k < 12; ++k) { const double p = prof[((k - r) % 12 + 12) % 12] - pm; ps += p * p; cov += (chroma[k] - m) / sd * p; }
                        const double c = cov / (std::sqrt(ps) * std::sqrt(12.0));
                        if (c > best) { best = c; br = r; bmin = mi; }
                    }
                std::snprintf(out.key, sizeof out.key, "%s%s %s", names[br], bmin ? "m" : "", bmin ? cmin[br] : cmaj[br]);
            }
        }
    }
}

// ------------------------------------------------------------------ finding the folder
inline bool isDir(const std::string& p) { return statPath(p).dir; }

inline std::vector<std::string> listDir(const std::string& dir)
{
    std::vector<std::string> out;
#if defined(_WIN32)
    std::error_code ec;
    for (std::filesystem::directory_iterator it(fsPath(dir), std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string n = it->path().filename().u8string();
        if (!n.empty() && n[0] != '.' && n[0] != '$') out.push_back(n);
    }
#else
    if (DIR* d = ::opendir(dir.c_str()))
    {
        while (dirent* e = ::readdir(d))
            if (e->d_name[0] != '.') out.push_back(e->d_name);
        ::closedir(d);
    }
#endif
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    return out;
}

inline const char* kFolderNames[] = { "Melt Deck Music", "MeltDeck Music", "Melt Deck", "MeltDeck" };

// "Melt Deck Music" at the top of the drive the plugin is on, or of any mounted drive.
inline std::vector<std::string> findMusicFolders(const std::string& pluginFolder)
{
    std::vector<std::string> roots, found;
    for (std::string p = pluginFolder; p.size() > 1;)
    {
        roots.push_back(p);
        const size_t s = p.find_last_of('/');
        if (s == std::string::npos || s == 0) break;
        p = p.substr(0, s);
    }
    for (const char* base : { "/media", "/mnt", "/run/media", "/Volumes" })
        for (const std::string& a : listDir(base))
        {
            roots.push_back(std::string(base) + "/" + a);
            for (const std::string& b : listDir(std::string(base) + "/" + a)) roots.push_back(std::string(base) + "/" + a + "/" + b);
        }
#if defined(MPC_DESKTOP)
    const std::string home = homeFolder();
    if (!home.empty()) for (const char* sub : { "", "/Music", "/Desktop", "/Documents", "/Downloads" }) roots.push_back(home + sub);
  #if defined(_WIN32)
    for (char d = 'C'; d <= 'Z'; ++d) { const std::string drv = std::string(1, d) + ":"; if (isDir(drv)) roots.push_back(drv); }
  #endif
#endif
    for (const std::string& r : roots)
        for (const char* n : kFolderNames)
        {
            const std::string c = r + "/" + n;
            if (!isDir(c)) continue;
            const std::string key = realPath(c);
            if (std::find(found.begin(), found.end(), key) == found.end()) found.push_back(key);
        }
    return found;
}

struct FileEntry { std::string path, rel, crate, stem; long size = 0; long mtime = 0; };

inline void listAudio(const std::string& root, const std::string& rel, int depth, std::vector<FileEntry>& out)
{
    if (depth > 4 || out.size() > 6000) return;
    const std::string dir = rel.empty() ? root : root + "/" + rel;
    for (const std::string& n : listDir(dir))
    {
        const std::string full = dir + "/" + n, r = rel.empty() ? n : rel + "/" + n;
        const FileStat s = statPath(full);
        if (!s.ok) continue;
        if (s.dir) { listAudio(root, r, depth + 1, out); continue; }
        const std::string ln = lower(n);
        const size_t dot = ln.rfind('.');
        if (dot == std::string::npos) continue;
        const std::string ext = ln.substr(dot);
        if (ext != ".wav" && ext != ".wave" && ext != ".aif" && ext != ".aiff" && ext != ".aifc" && ext != ".mp3") continue;
        FileEntry e;
        e.path = full; e.rel = r; e.size = s.size; e.mtime = s.mtime;
        e.stem = n.substr(0, dot);
        const size_t sl = r.find('/');
        e.crate = sl == std::string::npos ? std::string() : r.substr(0, sl);
        out.push_back(e);
    }
}

// cache: rel \t size \t mtime \t bpm \t first \t key \t start \t length \t gain   (later lines win)
struct CacheLine { long size = 0, mtime = 0; Analysis a; };

inline std::map<std::string, CacheLine> loadCache(const std::string& folder)
{
    std::map<std::string, CacheLine> m;
    std::FILE* f = openFile(folder + "/.meltdeck-cache.txt", "rb");
    if (!f) return m;
    std::string line;
    int ch;
    auto flush = [&]() {
        std::vector<std::string> v; std::string cur;
        for (char c : line) { if (c == '\t') { v.push_back(cur); cur.clear(); } else if (c != '\r') cur += c; }
        v.push_back(cur);
        if (v.size() >= 9)
        {
            CacheLine c;
            c.size = std::atol(v[1].c_str()); c.mtime = std::atol(v[2].c_str());
            c.a.bpm = (float)std::atof(v[3].c_str()); c.a.first = (float)std::atof(v[4].c_str());
            std::snprintf(c.a.key, sizeof c.a.key, "%s", v[5].c_str());
            c.a.start = (float)std::atof(v[6].c_str()); c.a.length = (float)std::atof(v[7].c_str()); c.a.gain = (float)std::atof(v[8].c_str());
            if (c.a.bpm >= 40 && c.a.bpm <= 250) m[v[0]] = c;
        }
        line.clear();
    };
    while ((ch = std::fgetc(f)) != EOF) { if (ch == '\n') flush(); else line += (char)ch; }
    if (!line.empty()) flush();
    std::fclose(f);
    return m;
}

inline void appendCache(const std::string& folder, const std::string& rel, long size, long mtime, const Analysis& a)
{
    if (std::FILE* f = openFile(folder + "/.meltdeck-cache.txt", "ab"))
    {
        std::fprintf(f, "%s\t%ld\t%ld\t%.2f\t%.4f\t%s\t%.3f\t%.2f\t%.1f\n", rel.c_str(), size, mtime, a.bpm, a.first, a.key, a.start, a.length, a.gain);
        std::fclose(f);
    }
}

// ------------------------------------------------------------------ file browser
struct BrowseEntry { std::string name, path; bool dir = false; };
static const char* const kAllPrefix = "\x01ALL:";                  // marks an "all songs" list
struct BrowseList { std::string path, title; std::vector<BrowseEntry> items; bool drives = false; };

inline bool isAudioName(const std::string& n)
{
    const std::string l = lower(n);
    const size_t d = l.rfind('.');
    if (d == std::string::npos) return false;
    const std::string e = l.substr(d);
    return e == ".wav" || e == ".wave" || e == ".aif" || e == ".aiff" || e == ".aifc" || e == ".mp3";
}

// every mounted drive (USB sticks, hard drives, SD cards) plus the drive the plugin lives on
// Every real storage drive the system has mounted (internal drives, USB hard drives and flash
// drives), read from /proc/mounts on Linux (the MPC). Found wherever they are mounted.
inline std::vector<std::pair<std::string, std::string>> mountedDrives()
{
    std::vector<std::pair<std::string, std::string>> out;
#if defined(__linux__)
    std::FILE* f = std::fopen("/proc/mounts", "rb");
    if (!f) return out;
    char line[1024];
    static const char* kFs[] = { "vfat", "exfat", "ntfs", "ntfs3", "fuseblk", "ext2", "ext3", "ext4", "hfsplus", "apfs", "btrfs", "xfs", "f2fs", "msdos", "fat" };
    while (std::fgets(line, sizeof line, f))
    {
        char dev[256], mnt[512], fs[64], opts[256];
        if (std::sscanf(line, "%255s %511s %63s %255s", dev, mnt, fs, opts) != 4) continue;
        if (std::strncmp(dev, "/dev/", 5) != 0) continue;
        if (std::strncmp(opts, "ro", 2) == 0 && (opts[2] == ',' || opts[2] == 0)) continue;   // read-only: system, not your drives
        bool ok = false;
        for (const char* k : kFs) if (std::strcmp(fs, k) == 0) ok = true;
        if (!ok) continue;
        std::string m;                                               // undo /proc/mounts escapes ("\040" = space)
        for (const char* c = mnt; *c; ++c)
        {
            if (c[0] == '\\' && c[1] >= '0' && c[1] <= '7' && c[2] && c[3]) { m += (char)((c[1] - '0') * 64 + (c[2] - '0') * 8 + (c[3] - '0')); c += 3; }
            else m += *c;
        }
        if (m == "/" || m.rfind("/opt", 0) == 0 || m.rfind("/boot", 0) == 0 || m.rfind("/etc", 0) == 0 || m.rfind("/usr", 0) == 0 || m.rfind("/var", 0) == 0
            || m.rfind("/proc", 0) == 0 || m.rfind("/sys", 0) == 0 || m.rfind("/snap", 0) == 0) continue;
        const size_t sl = m.find_last_of('/');
        out.push_back({ m, sl == std::string::npos ? m : m.substr(sl + 1) });
    }
    std::fclose(f);
#endif
    return out;
}
// changes whenever a drive is plugged in or pulled out
inline std::string driveSignature()
{
    std::string sig;
    for (const auto& m : mountedDrives()) sig += m.first + "|";
    for (const char* base : { "/media", "/run/media", "/mnt", "/Volumes" })
        for (const std::string& a : listDir(base)) sig += std::string(base) + "/" + a + "|";
#if defined(_WIN32)
    for (char d = 'C'; d <= 'Z'; ++d) if (isDir(std::string(1, d) + ":")) sig += d;
#endif
    return sig;
}

inline BrowseList listDrives(const std::string& pluginFolder)
{
    BrowseList L;
    L.drives = true; L.title = "DRIVES";
    std::vector<std::string> seen;
    auto add = [&](const std::string& path, const std::string& name) {
        const std::string key = realPath(path);
        if (std::find(seen.begin(), seen.end(), key) != seen.end() || !isDir(path)) return;
        seen.push_back(key);
        L.items.push_back({ name, path, true });
    };
    // the drive holding "VST2 Plugins"
    for (std::string p = pluginFolder; p.size() > 1;)
    {
        const size_t s = p.find_last_of('/');
        if (s == std::string::npos || s == 0) break;
        const std::string parent = p.substr(0, s);
        if (p.substr(s + 1) == "VST2 Plugins") { add(parent, "MPC Drive (" + parent.substr(parent.find_last_of('/') + 1) + ")"); break; }
        p = parent;
    }
    // every mounted drive: the MPC's internal drives, USB hard drives, flash drives
    for (const auto& m : mountedDrives()) add(m.first, m.second);
#if defined(MPC_DESKTOP)
    {   // on a computer: your home folders first, then every drive
        const std::string home = homeFolder();
        if (!home.empty())
        {
            add(home + "/Music", "Music");
            add(home + "/Desktop", "Desktop");
            add(home + "/Downloads", "Downloads");
            add(home + "/Documents", "Documents");
            add(home, "Home");
        }
  #if defined(_WIN32)
        for (char d = 'C'; d <= 'Z'; ++d) { const std::string drv = std::string(1, d) + ":"; add(drv, std::string(1, d) + ": drive"); }
  #endif
    }
#endif
    for (const char* base : { "/media", "/run/media", "/mnt", "/Volumes" })
        for (const std::string& a : listDir(base))
        {
            const std::string pa = std::string(base) + "/" + a;
            if (!isDir(pa)) continue;
            // /run/media/<user>/<drive>: one level deeper
            if (std::string(base) == "/run/media") { for (const std::string& b : listDir(pa)) add(pa + "/" + b, b); }
            else add(pa, a);
        }
    return L;
}

inline BrowseList listFolder(const std::string& path)
{
    BrowseList L;
    L.path = path;
    const size_t s = path.find_last_of('/');
    L.title = s == std::string::npos ? path : path.substr(s + 1);
    std::vector<BrowseEntry> dirs, files;
    for (const std::string& n : listDir(path))
    {
        const std::string full = path + "/" + n;
        if (isDir(full)) dirs.push_back({ n, full, true });
        else if (isAudioName(n)) files.push_back({ n.substr(0, n.rfind('.')), full, false });
    }
    auto byName = [](const BrowseEntry& a, const BrowseEntry& b) { return lower(a.name) < lower(b.name); };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    // a folder with folders inside: offer every song in all of them as one list
    if (!dirs.empty()) L.items.push_back({ "[ ALL SONGS IN HERE ]", kAllPrefix + path, true });
    L.items.insert(L.items.end(), dirs.begin(), dirs.end());
    L.items.insert(L.items.end(), files.begin(), files.end());
    return L;
}

// every song in a folder and the folders inside it, A to Z (up to 6000)
inline void collectAudio(const std::string& dir, int depth, std::vector<BrowseEntry>& out)
{
    if (depth > 8 || out.size() >= 6000) return;
    for (const std::string& n : listDir(dir))
    {
        const std::string full = dir + "/" + n;
        if (isAudioName(n)) out.push_back({ n.substr(0, n.rfind('.')), full, false });
        else if (n != "System Volume Information" && n != "VST2 Plugins" && isDir(full)) collectAudio(full, depth + 1, out);
        if (out.size() >= 6000) return;
    }
}
inline BrowseList listAll(const std::string& path)
{
    BrowseList L;
    L.path = kAllPrefix + path;
    const size_t s = path.find_last_of('/');
    L.title = "ALL: " + (s == std::string::npos ? path : path.substr(s + 1));
    collectAudio(path, 0, L.items);
    std::sort(L.items.begin(), L.items.end(), [](const BrowseEntry& a, const BrowseEntry& b) { return lower(a.name) < lower(b.name); });
    return L;
}

} // namespace mdlib
