// md_mp3.h - a small MPEG-1/2/2.5 Layer III (MP3) decoder for Melt Deck.
//
// SPDX-License-Identifier: MIT
//
// Plain C++ (no dependencies): frame parsing, bit reservoir, scalefactors,
// Huffman decoding, requantisation, joint stereo (M/S and MPEG-1 intensity),
// reordering, alias reduction, IMDCT with overlap, and the polyphase synthesis
// filter bank. Skips ID3v2 tags and the Xing/Info header frame.
#pragma once

#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "md_mp3_tables.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mdmp3 {

class BitReader
{
public:
    BitReader(const uint8_t* d, size_t bytes) : d_(d), bits_(bytes * 8) {}
    uint32_t get(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i)
        {
            const uint32_t b = pos_ < bits_ ? (d_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u : 0u;
            v = (v << 1) | b; ++pos_;
        }
        return v;
    }
    int bit() { const int b = pos_ < bits_ ? (d_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1 : 0; ++pos_; return b; }
    size_t pos() const { return pos_; }
    void seek(size_t p) { pos_ = p; }
private:
    const uint8_t* d_; size_t bits_, pos_ = 0;
};

// Huffman tree built from (code, length) tables
struct Tree
{
    std::vector<int32_t> node;   // pairs: [2n] child for 0, [2n+1] child for 1; >=0 node index, <0 leaf -(value+1)
    void build(const uint16_t* code, const uint8_t* len, int n)
    {
        node.assign(2, 0);
        for (int s = 0; s < n; ++s)
        {
            if (len[s] == 0) continue;
            int cur = 0;
            for (int b = len[s] - 1; b >= 0; --b)
            {
                const int bit = (code[s] >> b) & 1;
                const size_t slot = (size_t)(cur * 2 + bit);
                if (b == 0) { node[slot] = -(s + 1); break; }
                if (node[slot] <= 0) { node[slot] = (int)node.size() / 2; node.push_back(0); node.push_back(0); }
                cur = node[slot];
            }
        }
    }
    int decode(BitReader& br) const
    {
        int cur = 0;
        for (int guard = 0; guard < 24; ++guard)
        {
            const int nx = node[(size_t)(cur * 2 + br.bit())];
            if (nx < 0) return -nx - 1;
            if (nx == 0) return 0;
            cur = nx;
        }
        return 0;
    }
};

struct Granule
{
    int part23 = 0, bigValues = 0, globalGain = 0, sfCompress = 0, winSwitch = 0, blockType = 0, mixed = 0;
    int table[3] = {0, 0, 0}, subGain[3] = {0, 0, 0}, region0 = 0, region1 = 0, preflag = 0, sfScale = 0, count1Table = 0;
};

class Decoder
{
public:
    Decoder()
    {
        for (int k = 0; k < 32; ++k) if (kBig[k].code) big_[k].build(kBig[k].code, kBig[k].len, kBig[k].xlen * kBig[k].xlen);
        quadA_.build(kQuad[0].code, kQuad[0].len, 16);
        for (int i = 0; i < 8207; ++i) pow43_[i] = (float)std::pow((double)i, 4.0 / 3.0);
        for (int i = 0; i < 257; ++i)
        {
            double v = kEnWindow[i] / 65536.0;
            D_[i] = (float)v;
            if (i & 63) v = -v;
            if (i) D_[512 - i] = (float)v;
        }
        for (int m = 0; m <= 32; ++m)
            for (int k = 0; k < 32; ++k) N_[m][k] = (float)std::cos(m * (2 * k + 1) * M_PI / 64.0);
        for (int i = 0; i < 36; ++i)
        {
            win_[0][i] = (float)std::sin(M_PI / 36 * (i + 0.5));
            win_[1][i] = i < 18 ? win_[0][i] : i < 24 ? 1.0f : i < 30 ? (float)std::sin(M_PI / 12 * (i - 18 + 0.5)) : 0.0f;
            win_[3][i] = i < 6 ? 0.0f : i < 12 ? (float)std::sin(M_PI / 12 * (i - 6 + 0.5)) : i < 18 ? 1.0f : win_[0][i];
            for (int k = 0; k < 18; ++k) cosL_[i][k] = (float)std::cos(M_PI / 72 * (2 * i + 1 + 18) * (2 * k + 1));
        }
        for (int i = 0; i < 12; ++i)
        {
            win_[2][i] = (float)std::sin(M_PI / 12 * (i + 0.5));
            for (int k = 0; k < 6; ++k) cosS_[i][k] = (float)std::cos(M_PI / 24 * (2 * i + 1 + 6) * (2 * k + 1));
        }
        static const double ci[8] = {-0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037};
        for (int i = 0; i < 8; ++i) { cs_[i] = (float)(1 / std::sqrt(1 + ci[i] * ci[i])); ca_[i] = (float)(ci[i] / std::sqrt(1 + ci[i] * ci[i])); }
    }

    // Decode a whole file in memory to interleaved stereo 16-bit. Returns false if no frames.
    bool decode(const uint8_t* data, size_t size, std::vector<int16_t>& out, long& frames, int& rate)
    {
        size_t p = 0;
        if (size > 10 && !std::memcmp(data, "ID3", 3))
            p = 10 + (size_t)(((data[6] & 0x7F) << 21) | ((data[7] & 0x7F) << 14) | ((data[8] & 0x7F) << 7) | (data[9] & 0x7F));
        out.clear(); frames = 0; rate = 0;
        std::memset(prev_, 0, sizeof prev_); std::memset(V_, 0, sizeof V_); vOff_[0] = vOff_[1] = 0;
        res_.clear();
        bool first = true;
        int badRun = 0;
        while (p + 4 <= size)
        {
            Header h;
            if (!parseHeader(data + p, h) || p + (size_t)h.frameLen > size)
            {
                ++p; if (++badRun > (1 << 20)) break; continue;
            }
            // require the next header to agree (avoid false syncs), unless this is the last frame
            if (p + (size_t)h.frameLen + 4 <= size)
            {
                Header h2;
                if (!parseHeader(data + p + h.frameLen, h2) || h2.rate != h.rate || h2.layer3 != h.layer3) { ++p; continue; }
            }
            badRun = 0;
            if (rate == 0) rate = h.rate;
            if (h.rate != rate) { p += (size_t)h.frameLen; continue; }
            if (first && isInfoFrame(data + p, h)) { first = false; p += (size_t)h.frameLen; continue; }
            first = false;
            decodeFrame(data + p, h, out);
            frames = (long)(out.size() / 2);
            p += (size_t)h.frameLen;
        }
        return frames > 0 && rate > 0;
    }

private:
    struct Header { int v1 = 1, lsf = 0, mpeg25 = 0, layer3 = 0, crc = 0, brIdx = 0, srIdx = 0, pad = 0, mode = 0, modeExt = 0, rate = 0, frameLen = 0, nch = 2; };

    bool parseHeader(const uint8_t* b, Header& h) const
    {
        if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return false;
        const int ver = (b[1] >> 3) & 3, layer = (b[1] >> 1) & 3;
        if (ver == 1 || layer != 1) return false;           // only Layer III
        h.layer3 = 1;
        h.v1 = ver == 3; h.lsf = !h.v1; h.mpeg25 = ver == 0;
        h.crc = !(b[1] & 1);
        h.brIdx = (b[2] >> 4) & 15; h.srIdx = (b[2] >> 2) & 3; h.pad = (b[2] >> 1) & 1;
        if (h.brIdx == 0 || h.brIdx == 15 || h.srIdx == 3) return false;
        h.mode = (b[3] >> 6) & 3; h.modeExt = (b[3] >> 4) & 3;
        h.nch = h.mode == 3 ? 1 : 2;
        static const int br1[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
        static const int br2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
        static const int sr[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
        h.rate = sr[h.v1 ? 0 : (h.mpeg25 ? 2 : 1)][h.srIdx];
        const int kbps = h.v1 ? br1[h.brIdx] : br2[h.brIdx];
        h.frameLen = (h.v1 ? 144000 : 72000) * kbps / h.rate + h.pad;
        return h.frameLen > 4;
    }
    int sideInfoLen(const Header& h) const { return h.v1 ? (h.nch == 1 ? 17 : 32) : (h.nch == 1 ? 9 : 17); }
    bool isInfoFrame(const uint8_t* f, const Header& h) const
    {
        const uint8_t* s = f + 4 + (h.crc ? 2 : 0) + sideInfoLen(h);
        return !std::memcmp(s, "Xing", 4) || !std::memcmp(s, "Info", 4);
    }

    // scalefactor band edges [sample-rate group][...]
    const int* sfbLong(const Header& h) const
    {
        static const int t[9][23] = {
            {0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 52, 62, 74, 90, 110, 134, 162, 196, 238, 288, 342, 418, 576},
            {0, 4, 8, 12, 16, 20, 24, 30, 36, 42, 50, 60, 72, 88, 106, 128, 156, 190, 230, 276, 330, 384, 576},
            {0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 54, 66, 82, 102, 126, 156, 194, 240, 296, 364, 448, 550, 576},
            {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
            {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 114, 136, 162, 194, 232, 278, 332, 394, 464, 540, 576},
            {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
            {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
            {0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
            {0, 12, 24, 36, 48, 60, 72, 88, 108, 132, 160, 192, 232, 280, 336, 400, 476, 566, 568, 570, 572, 574, 576}};
        return t[group(h)];
    }
    const int* sfbShort(const Header& h) const
    {
        static const int t[9][14] = {
            {0, 4, 8, 12, 16, 22, 30, 40, 52, 66, 84, 106, 136, 192},
            {0, 4, 8, 12, 16, 22, 28, 38, 50, 64, 80, 100, 126, 192},
            {0, 4, 8, 12, 16, 22, 30, 42, 58, 78, 104, 138, 180, 192},
            {0, 4, 8, 12, 18, 24, 32, 42, 56, 74, 100, 132, 174, 192},
            {0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 136, 180, 192},
            {0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 134, 174, 192},
            {0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 134, 174, 192},    // MPEG-2.5 11.025/12 kHz use the 16 kHz layout
            {0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 134, 174, 192},
            {0, 8, 16, 24, 36, 52, 72, 96, 124, 160, 162, 164, 166, 192}};
        return t[group(h)];
    }
    int group(const Header& h) const { return (h.v1 ? 0 : (h.mpeg25 ? 6 : 3)) + h.srIdx; }

    void decodeFrame(const uint8_t* f, const Header& h, std::vector<int16_t>& out)
    {
        const int hdr = 4 + (h.crc ? 2 : 0), si = sideInfoLen(h);
        BitReader br(f + hdr, (size_t)si);
        const int nch = h.nch, ngr = h.v1 ? 2 : 1;
        const int mainBegin = (int)br.get(h.v1 ? 9 : 8);
        br.get(h.v1 ? (nch == 1 ? 5 : 3) : (nch == 1 ? 1 : 2));
        int scfsi[2][4] = {{0}};
        if (h.v1) for (int ch = 0; ch < nch; ++ch) for (int b = 0; b < 4; ++b) scfsi[ch][b] = (int)br.get(1);
        Granule g[2][2];
        for (int gr = 0; gr < ngr; ++gr)
            for (int ch = 0; ch < nch; ++ch)
            {
                Granule& x = g[gr][ch];
                x.part23 = (int)br.get(12); x.bigValues = (int)br.get(9); x.globalGain = (int)br.get(8);
                x.sfCompress = (int)br.get(h.v1 ? 4 : 9); x.winSwitch = (int)br.get(1);
                if (x.winSwitch)
                {
                    x.blockType = (int)br.get(2); x.mixed = (int)br.get(1);
                    x.table[0] = (int)br.get(5); x.table[1] = (int)br.get(5);
                    for (int w = 0; w < 3; ++w) x.subGain[w] = (int)br.get(3);
                    x.region0 = (x.blockType == 2 && !x.mixed) ? 8 : 7; x.region1 = 20 - x.region0;
                    if (x.blockType == 0) x.blockType = 1;       // invalid stream: treat as start block
                }
                else
                {
                    for (int r = 0; r < 3; ++r) x.table[r] = (int)br.get(5);
                    x.region0 = (int)br.get(4); x.region1 = (int)br.get(3);
                }
                if (h.v1) x.preflag = (int)br.get(1);
                x.sfScale = (int)br.get(1); x.count1Table = (int)br.get(1);
                if (x.bigValues > 288) x.bigValues = 288;
            }
        // bit reservoir
        const size_t dataStart = (size_t)(hdr + si);
        const size_t frameData = (size_t)h.frameLen > dataStart ? (size_t)h.frameLen - dataStart : 0;
        const int have = (int)res_.size();
        res_.insert(res_.end(), f + dataStart, f + dataStart + frameData);
        const bool ok = mainBegin <= have;
        const size_t base = (size_t)(have - (ok ? mainBegin : 0));
        BitReader md(res_.data() + base, res_.size() - base);

        float xr[2][576];
        for (int gr = 0; gr < ngr; ++gr)
        {
            int sf[2][40] = {{0}};          // long: [0..21]; short: [sfb*3 + win] up to 39
            int nz[2] = {0, 0};
            for (int ch = 0; ch < nch; ++ch)
            {
                Granule& x = g[gr][ch];
                const size_t start = md.pos();
                if (!ok) { std::memset(xr[ch], 0, sizeof xr[ch]); md.seek(start + (size_t)x.part23); continue; }
                if (h.v1) readScalefactorsV1(md, x, gr, ch, scfsi, sf[ch]);
                else readScalefactorsLsf(md, x, h, ch, sf[ch]);
                int is[576];
                nz[ch] = huffman(md, x, h, start + (size_t)x.part23, is);
                md.seek(start + (size_t)x.part23);
                requantize(x, h, sf[ch], is, xr[ch]);
                if (gr == 0 && h.v1) std::memcpy(sfPrev_[ch], sf[ch], sizeof sfPrev_[ch]);
            }
            if (nch == 2 && h.mode == 1) stereo(h, g[gr], sf[1], nz, xr);
            for (int ch = 0; ch < nch; ++ch)
            {
                Granule& x = g[gr][ch];
                if (x.winSwitch && x.blockType == 2) reorder(x, h, xr[ch]);
                antialias(x, xr[ch]);
                hybrid(x, ch, xr[ch]);
            }
            // synthesis: 18 slots x 32 samples
            float pcm[2][576];
            for (int ch = 0; ch < nch; ++ch)
                for (int t = 0; t < 18; ++t)
                {
                    float s[32];
                    for (int sb = 0; sb < 32; ++sb) s[sb] = xr[ch][sb * 18 + t];
                    synth(ch, s, pcm[ch] + t * 32);
                }
            const size_t o = out.size();
            out.resize(o + 576 * 2);
            for (int i = 0; i < 576; ++i)
            {
                const float l = pcm[0][i], r = nch == 2 ? pcm[1][i] : pcm[0][i];
                out[o + (size_t)i * 2] = toS16(l);
                out[o + (size_t)i * 2 + 1] = toS16(r);
            }
        }
        // keep only what a later frame could still reach (max main_data_begin 511 bytes)
        const size_t keepFrom = res_.size() > 2048 ? res_.size() - 2048 : 0;
        if (keepFrom > 0) res_.erase(res_.begin(), res_.begin() + (long)keepFrom);
    }
    static int16_t toS16(float v)
    {
        const float x = std::round(v * 32767.0f);
        return (int16_t)(x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : (int)x);
    }

    void readScalefactorsV1(BitReader& md, const Granule& x, int gr, int ch, int scfsi[2][4], int* sf)
    {
        static const int slen1t[16] = {0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4};
        static const int slen2t[16] = {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3};
        const int s1 = slen1t[x.sfCompress], s2 = slen2t[x.sfCompress];
        if (x.winSwitch && x.blockType == 2)
        {
            if (x.mixed)
            {
                for (int b = 0; b < 8; ++b) sf[b] = (int)md.get(s1);                 // long part
                for (int b = 3; b < 12; ++b) for (int w = 0; w < 3; ++w) shortSf_[b * 3 + w] = (int)md.get(b < 6 ? s1 : s2);
            }
            else
                for (int b = 0; b < 12; ++b) for (int w = 0; w < 3; ++w) shortSf_[b * 3 + w] = (int)md.get(b < 6 ? s1 : s2);
            for (int w = 0; w < 3; ++w) shortSf_[36 + w] = 0;
        }
        else
        {
            static const int edge[5] = {0, 6, 11, 16, 21};
            for (int gidx = 0; gidx < 4; ++gidx)
                for (int b = edge[gidx]; b < edge[gidx + 1]; ++b)
                    sf[b] = (gr == 1 && scfsi[ch][gidx]) ? sfPrev_[ch][b] : (int)md.get(gidx < 2 ? s1 : s2);
            sf[21] = 0;
        }
    }

    void readScalefactorsLsf(BitReader& md, Granule& x, const Header& h, int ch, int* sf)
    {
        static const int nr[6][3][4] = {
            {{6, 5, 5, 5}, {9, 9, 9, 9}, {6, 9, 9, 9}},
            {{6, 5, 7, 3}, {9, 9, 12, 6}, {6, 9, 12, 6}},
            {{11, 10, 0, 0}, {18, 18, 0, 0}, {15, 18, 0, 0}},
            {{7, 7, 7, 0}, {12, 12, 12, 0}, {6, 15, 12, 0}},
            {{6, 6, 6, 3}, {12, 9, 9, 6}, {6, 12, 9, 6}},
            {{8, 8, 5, 0}, {15, 12, 9, 0}, {6, 18, 9, 0}}};
        int slen[4] = {0, 0, 0, 0}, idx = 0;
        int c = x.sfCompress;
        const bool isRight = ch == 1 && (h.modeExt & 1) && h.mode == 1;
        x.preflag = 0;
        if (!isRight)
        {
            if (c < 400) { slen[0] = (c >> 4) / 5; slen[1] = (c >> 4) % 5; slen[2] = (c & 15) >> 2; slen[3] = c & 3; idx = 0; }
            else if (c < 500) { c -= 400; slen[0] = (c >> 2) / 5; slen[1] = (c >> 2) % 5; slen[2] = c & 3; idx = 1; }
            else { c -= 500; slen[0] = c / 3; slen[1] = c % 3; idx = 2; x.preflag = 1; }
        }
        else
        {
            c >>= 1;
            if (c < 180) { slen[0] = c / 36; slen[1] = (c % 36) / 6; slen[2] = (c % 36) % 6; idx = 3; }
            else if (c < 244) { c -= 180; slen[0] = (c & 63) >> 4; slen[1] = (c & 15) >> 2; slen[2] = c & 3; idx = 4; }
            else { c -= 244; slen[0] = c / 3; slen[1] = c % 3; idx = 5; }
        }
        const int bt = (x.winSwitch && x.blockType == 2) ? (x.mixed ? 2 : 1) : 0;
        int vals[45] = {0}, n = 0;
        for (int i = 0; i < 4; ++i) for (int j = 0; j < nr[idx][bt][i]; ++j) vals[n++] = slen[i] ? (int)md.get(slen[i]) : 0;
        if (bt == 0) { for (int b = 0; b < 21; ++b) sf[b] = vals[b]; sf[21] = 0; }
        else
        {
            int k = 0;
            if (bt == 2) { for (int b = 0; b < 6; ++b) sf[b] = vals[k++]; }
            for (int b = (bt == 2 ? 3 : 0); b < 12; ++b) for (int w = 0; w < 3; ++w) shortSf_[b * 3 + w] = k < n ? vals[k++] : 0;
            for (int w = 0; w < 3; ++w) shortSf_[36 + w] = 0;
        }
    }

    int huffman(BitReader& md, const Granule& x, const Header& h, size_t end, int* is)
    {
        const int* L = sfbLong(h);
        const int* S = sfbShort(h);
        int r1, r2;
        if (x.winSwitch)
        {
            r1 = x.blockType == 2 ? S[3] * 3 : L[8];
            if (!h.v1 && x.blockType == 2 && !x.mixed) r1 = S[3] * 3;
            r2 = 576;
        }
        else
        {
            r1 = L[std::min(x.region0 + 1, 22)];
            r2 = L[std::min(x.region0 + x.region1 + 2, 22)];
        }
        const int bv = x.bigValues * 2;
        int i = 0;
        for (; i < bv && i < 576; i += 2)
        {
            const int t = x.table[i < r1 ? 0 : (i < r2 ? 1 : 2)];
            if (t == 0 || t == 4 || t == 14 || !kBig[t].code) { is[i] = is[i + 1] = 0; continue; }
            const int v = big_[t].decode(md);
            const int xl = kBig[t].xlen;
            int a = v / xl, b = v % xl;
            const int lb = kBig[t].linbits;
            if (lb && a == 15) a += (int)md.get(lb);
            if (a && md.bit()) a = -a;
            if (lb && b == 15) b += (int)md.get(lb);
            if (b && md.bit()) b = -b;
            is[i] = a; is[i + 1] = b;
        }
        while (i + 4 <= 576 && md.pos() < end)
        {
            int v;
            if (x.count1Table) v = 15 - (int)md.get(4);
            else v = quadA_.decode(md);
            int q[4] = {(v >> 3) & 1, (v >> 2) & 1, (v >> 1) & 1, v & 1};
            for (int k = 0; k < 4; ++k) if (q[k] && md.bit()) q[k] = -q[k];
            if (md.pos() > end) break;                  // overran: drop this quad
            for (int k = 0; k < 4; ++k) is[i + k] = q[k];
            i += 4;
        }
        const int last = i;
        for (; i < 576; ++i) is[i] = 0;
        return last;
    }

    void requantize(const Granule& x, const Header& h, const int* sf, const int* is, float* xr)
    {
        static const int pretab[22] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0};
        const double gain = std::pow(2.0, 0.25 * (x.globalGain - 210));
        const double mult = x.sfScale ? 1.0 : 0.5;
        const int* L = sfbLong(h);
        const int* S = sfbShort(h);
        auto q = [&](int v) { const int a = v < 0 ? -v : v; const float m = a < 8207 ? pow43_[a] : (float)std::pow((double)a, 4.0 / 3.0); return v < 0 ? -m : m; };
        const bool shortB = x.winSwitch && x.blockType == 2;
        const int longEnd = shortB ? (x.mixed ? L[h.v1 ? 8 : 6] : 0) : 576;
        int sfb = 0;
        for (int i = 0; i < longEnd; ++i)
        {
            while (sfb < 21 && i >= L[sfb + 1]) ++sfb;
            const int p = x.preflag ? pretab[sfb] : 0;
            xr[i] = (float)(q(is[i]) * gain * std::pow(2.0, -mult * (sf[sfb] + p)));
        }
        if (shortB)
        {
            const int* ss = shortSf_;
            for (int b = x.mixed ? 3 : 0; b < 13; ++b)
            {
                const int w0 = S[b], width = S[b + 1] - S[b];
                for (int w = 0; w < 3; ++w)
                {
                    const double g = gain * std::pow(2.0, -2.0 * x.subGain[w]) * std::pow(2.0, -mult * (b < 12 ? ss[b * 3 + w] : 0));
                    for (int j = 0; j < width; ++j)
                    {
                        const int i = w0 * 3 + w * width + j;
                        if (i < 576) xr[i] = (float)(q(is[i]) * g);
                    }
                }
            }
        }
    }

    void stereo(const Header& h, Granule* g, const int* sfR, const int* nz, float xr[2][576])
    {
        const bool ms = (h.modeExt & 2) != 0, isOn = (h.modeExt & 1) != 0 && h.v1;
        int isStart = 576;
        const int* L = sfbLong(h);
        if (isOn)
        {
            // intensity stereo (MPEG-1): above the last non-zero line of the right channel
            const Granule& r = g[1];
            const bool shortB = r.winSwitch && r.blockType == 2;
            if (!shortB)
            {
                int b = 0;
                while (b < 21 && L[b + 1] <= nz[1]) ++b;
                if (nz[1] > L[b]) ++b;
                isStart = L[std::min(b, 21)];
                for (int sb = b; sb < 22; ++sb)
                {
                    const int pos = sb < 21 ? sfR[sb] : sfR[20];
                    const int a = L[sb], e = L[sb + 1];
                    if (pos >= 7) { if (ms) for (int i = a; i < e; ++i) msPair(xr, i); continue; }
                    const double t = std::tan(pos * M_PI / 12), kl = t / (1 + t), kr = 1 / (1 + t);
                    for (int i = a; i < e; ++i) { const float v = xr[0][i]; xr[0][i] = (float)(v * kl); xr[1][i] = (float)(v * kr); }
                }
            }
            else isStart = 576;                       // rare: short-block intensity left as M/S
        }
        if (ms) for (int i = 0; i < isStart; ++i) msPair(xr, i);
    }
    static inline void msPair(float xr[2][576], int i)
    {
        const float m = xr[0][i], s = xr[1][i];
        xr[0][i] = (m + s) * 0.70710678f; xr[1][i] = (m - s) * 0.70710678f;
    }

    void reorder(const Granule& x, const Header& h, float* xr)
    {
        const int* S = sfbShort(h);
        float tmp[576];
        for (int b = x.mixed ? 3 : 0; b < 13; ++b)
        {
            const int start = S[b] * 3, width = S[b + 1] - S[b];
            for (int w = 0; w < 3; ++w)
                for (int j = 0; j < width; ++j) tmp[j * 3 + w] = xr[start + w * width + j];
            std::memcpy(xr + start, tmp, sizeof(float) * (size_t)(3 * width));
        }
    }

    void antialias(const Granule& x, float* xr)
    {
        if (x.winSwitch && x.blockType == 2 && !x.mixed) return;
        const int sbLimit = (x.winSwitch && x.blockType == 2 && x.mixed) ? 1 : 31;
        for (int sb = 0; sb < sbLimit; ++sb)
            for (int i = 0; i < 8; ++i)
            {
                const float a = xr[sb * 18 + 17 - i], b = xr[(sb + 1) * 18 + i];
                xr[sb * 18 + 17 - i] = a * cs_[i] - b * ca_[i];
                xr[(sb + 1) * 18 + i] = b * cs_[i] + a * ca_[i];
            }
    }

    void hybrid(const Granule& x, int ch, float* xr)
    {
        for (int sb = 0; sb < 32; ++sb)
        {
            const int bt = (x.winSwitch && x.mixed && sb < 2) ? 0 : (x.winSwitch ? x.blockType : 0);
            float out[36];
            float* X = xr + sb * 18;
            if (bt != 2)
            {
                for (int i = 0; i < 36; ++i)
                {
                    float s = 0;
                    for (int k = 0; k < 18; ++k) s += X[k] * cosL_[i][k];
                    out[i] = s * win_[bt][i];
                }
            }
            else
            {
                std::memset(out, 0, sizeof out);
                for (int w = 0; w < 3; ++w)
                    for (int i = 0; i < 12; ++i)
                    {
                        float s = 0;
                        for (int k = 0; k < 6; ++k) s += X[w + 3 * k] * cosS_[i][k];
                        out[6 + 6 * w + i] += s * win_[2][i];
                    }
            }
            for (int i = 0; i < 18; ++i)
            {
                float v = out[i] + prev_[ch][sb][i];
                prev_[ch][sb][i] = out[18 + i];
                if ((sb & 1) && (i & 1)) v = -v;          // frequency inversion
                X[i] = v;
            }
        }
    }

    void synth(int ch, const float* s, float* pcm)
    {
        vOff_[ch] = (vOff_[ch] - 64) & 1023;
        float* V = V_[ch];
        // V[i] = sum_k cos((16+i)(2k+1)pi/64) s[k]; only 33 distinct cosine rows are needed
        float X[33];
        for (int m = 0; m <= 32; ++m)
        {
            float acc = 0;
            const float* row = N_[m];
            for (int k = 0; k < 32; ++k) acc += row[k] * s[k];
            X[m] = acc;
        }
        for (int i = 0; i < 64; ++i)
        {
            const int m = 16 + i;
            const float v = m <= 32 ? X[m] : (m <= 64 ? -X[64 - m] : -X[m - 64]);
            V[(vOff_[ch] + i) & 1023] = v;
        }
        for (int j = 0; j < 32; ++j)
        {
            float acc = 0;
            for (int i = 0; i < 8; ++i)
            {
                acc += V[(vOff_[ch] + i * 128 + j) & 1023] * D_[i * 64 + j];
                acc += V[(vOff_[ch] + i * 128 + 96 + j) & 1023] * D_[i * 64 + 32 + j];
            }
            pcm[j] = acc;
        }
    }

    Tree big_[32], quadA_;
    float pow43_[8207];
    float D_[512];
    float N_[33][32];
    float win_[4][36];
    float cosL_[36][18], cosS_[12][6];
    float cs_[8], ca_[8];
    float prev_[2][32][18];
    float V_[2][1024];
    int vOff_[2] = {0, 0};
    std::vector<uint8_t> res_;
    int sfPrev_[2][40] = {{0}};
    int shortSf_[40] = {0};
};

} // namespace mdmp3
