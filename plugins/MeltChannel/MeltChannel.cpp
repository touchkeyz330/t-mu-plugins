// Melt Channel - one channel strip for the MPC: tube preamp, EQ, opto leveler and master.
//
// SPDX-License-Identifier: MIT
//
// Signal path:  in -> TUBE PREAMP -> EQUALIZER -> OPTO LEVELER -> MASTER -> out
//   Preamp  LOW / HIGH gain range, LEVEL drives a 2x oversampled tube-style stage
//           (asymmetric, so it adds the even harmonics a tube does). Loudness is
//           mostly compensated, so turning it up adds grit more than volume.
//   EQ      HIGH shelf 10 kHz, MID bell 300 Hz - 7.2 kHz, LOW shelf 110 Hz, +/-12 dB.
//   Opto    one COMPRESSION knob. Optical-style: soft knee, gentle ratio, fast-then-slow
//           release that depends on how hard it has been working. Gain is made up for you.
//   Master  OUTPUT (-inf .. +12 dB), VU meter, and ON/OFF true bypass of the whole strip.
//
// Parameters (permanent order)
//   0 Preamp In 1 Gain Range 2 Level 3 EQ In 4 High 5 Mid Freq 6 Mid 7 Low
//   8 Opto In 9 Compression 10 Master On 11 Output 12 GR Meter 13 VU Meter

#include "mpc_plugin.h"
#include "dsp/biquad.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

namespace {

using mpc::dsp::BiquadCoeffs;
using mpc::dsp::Biquad;

enum
{
    kPreIn = 0, kRange, kLevel, kEqIn, kHigh, kFreq, kMid, kLow, kOptoIn, kComp, kOn, kOutput, kGrMeter, kVuMeter,
    kNumParams
};

inline float dbg(float db) { return std::pow(10.0f, db * 0.05f); }
inline float freqOf(float t) { return 300.0f * std::pow(7200.0f / 300.0f, t); }   // MID FREQ knob -> Hz

// meters: VU-style scale, linear in voltage. 0 VU sits at about 70% of the swing, +3 at the end.
inline float vuPos(float db) { return mpc::clamp(std::pow(10.0f, (db - 3.0f) / 20.0f), 0.0f, 1.0f); }

class MeltChannel : public mpc::Plugin
{
public:
    MeltChannel() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::boolParam("prein", "Preamp In", true));
        addParam(mpc::choiceParam("range", "Gain Range", { "Low", "High" }, 0));
        addParam(mpc::floatParam("level", "Level", 0.0f, 10.0f, 3.0f, "", 1));
        addParam(mpc::boolParam("eqin", "EQ In", true));
        addParam(mpc::floatParam("high", "High", -12.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("freq", "Mid Freq", 0.0f, 1.0f, 0.62f, "", 2));
        addParam(mpc::floatParam("mid", "Mid", -12.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("low", "Low", -12.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::boolParam("optoin", "Opto In", true));
        addParam(mpc::floatParam("comp", "Compression", 0.0f, 10.0f, 0.0f, "", 1));
        addParam(mpc::boolParam("on", "Master On", true));
        addParam(mpc::floatParam("output", "Output", -30.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("grmeter", "GR Meter", 0.0f, 1.0f, 1.0f, "", 2));
        addParam(mpc::floatParam("vumeter", "VU Meter", 0.0f, 1.0f, 0.0f, "", 2));
        for (auto& n : notify_) n.store(0);
    }
    ~MeltChannel() override
    {
        running_.store(false);
        if (notifier_.joinable()) notifier_.join();
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Channel"; i.vendor = "T-MU";
        i.uniqueId = MPC_FOURCC('T', 'm', 'c', 'h');
        i.version = 1000; i.isInstrument = false; i.numInputs = 2; i.numOutputs = 2;
        return i;
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[48];
        switch (index)
        {
            case kFreq:
            {
                const float f = freqOf(plain);
                if (f >= 1000) std::snprintf(b, sizeof b, "%.1f kHz", f / 1000.0f); else std::snprintf(b, sizeof b, "%d Hz", (int)std::lround(f));
                out = b; return true;
            }
            case kHigh: case kMid: case kLow:
                std::snprintf(b, sizeof b, "%+.1f dB", plain); out = b; return true;
            case kOutput:
                if (plain <= -29.5f) { out = "-inf dB"; return true; }
                std::snprintf(b, sizeof b, "%+.1f dB", plain); out = b; return true;
            case kGrMeter:
                std::snprintf(b, sizeof b, "GR %.1f dB", grShown_); out = b; return true;
            case kVuMeter: out = ""; return true;
            default: return false;
        }
    }

    void prepare(double sr, int) override
    {
        sr_ = (float)sr;
        // 2x oversampling filters for the tube stage (run at 2x rate)
        const BiquadCoeffs aa = BiquadCoeffs::lowpass(sr * 2.0, (float)(sr * 0.43), 0.54f);
        const BiquadCoeffs aa2 = BiquadCoeffs::lowpass(sr * 2.0, (float)(sr * 0.43), 1.31f);
        for (int c = 0; c < 2; ++c)
        {
            up1_[c].setCoeffs(aa); up2_[c].setCoeffs(aa2); dn1_[c].setCoeffs(aa); dn2_[c].setCoeffs(aa2);
        }
        lastEq_[0] = lastEq_[1] = lastEq_[2] = lastEq_[3] = 1e9f;
        updateEq();
        aVu_ = 1.0f - std::exp(-1.0f / (0.3f * sr_ / 2.2f));       // VU: about 300 ms to settle
        reset();
        if (!running_.exchange(true))
            notifier_ = std::thread([this] {
                while (running_.load())
                {
                    for (int i = kGrMeter; i <= kVuMeter; ++i)
                        if (notify_[i - kGrMeter].exchange(0)) notifyHostParameterChanged(i);
                    std::this_thread::sleep_for(std::chrono::milliseconds(33));
                }
            });
    }

    void reset() override
    {
        for (int c = 0; c < 2; ++c)
        {
            up1_[c].reset(); up2_[c].reset(); dn1_[c].reset(); dn2_[c].reset();
            hs_[c].reset(); pk_[c].reset(); ls_[c].reset();
            prevIn_[c] = 0; dcX_[c] = dcY_[c] = 0;
        }
        env_ = 0; grFast_ = grSlow_ = 0; vu_ = 0;
        mixPre_ = getBool(kPreIn) ? 1.0f : 0.0f; mixEq_ = getBool(kEqIn) ? 1.0f : 0.0f;
        mixOpto_ = getBool(kOptoIn) ? 1.0f : 0.0f; mixOn_ = getBool(kOn) ? 1.0f : 0.0f;
        outG_ = outTarget(); timer_ = 0;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        updateEq();

        // preamp settings
        const bool high = getInt(kRange) == 1;
        const float level = get(kLevel);
        const float bias = high ? 0.45f : 0.2f;
        const float driveDb = high ? 6.0f + level * 2.4f : level * 1.8f;        // LOW 0..18 dB, HIGH 6..30 dB
        const float drive = dbg(driveDb) * kHead;
        const float tb = std::tanh(bias), slope = 1.0f - tb * tb;
        // loudness: small signals come out a little louder as LEVEL goes up, loud ones get squashed
        const float post = dbg(-driveDb * (high ? 0.62f : 0.7f)) / (slope * kHead);
        // opto settings
        const float comp = get(kComp);
        const float thr = -3.0f - comp * 2.7f;                                    // 0 -> -3 dBFS, 10 -> -30 dBFS
        const float ratio = 2.5f + comp * 0.25f;                                  // gentle, like an opto
        const float knee = 10.0f;
        const float makeup = comp * 0.85f;
        const float aAtk = 1.0f - std::exp(-1.0f / (0.010f * sr_));
        const float aRelFast = 1.0f - std::exp(-1.0f / (0.07f * sr_));
        const float aSlowAtk = 1.0f - std::exp(-1.0f / (0.4f * sr_));
        const float aRelSlow = 1.0f - std::exp(-1.0f / (2.2f * sr_));
        const float aEnvR = 1.0f - std::exp(-1.0f / (0.05f * sr_));
        const float aMix = 1.0f - std::exp(-1.0f / (0.01f * sr_));
        const float tPre = getBool(kPreIn) ? 1.0f : 0.0f, tEq = getBool(kEqIn) ? 1.0f : 0.0f;
        const float tOpto = getBool(kOptoIn) ? 1.0f : 0.0f, tOn = getBool(kOn) ? 1.0f : 0.0f;
        const float tOut = outTarget();

        const float* inL = ctx.inputs[0];
        const float* inR = ctx.numInputs > 1 ? ctx.inputs[1] : ctx.inputs[0];
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        float grMax = 0;
        for (int i = 0; i < n; ++i)
        {
            const float dryL = inL[i], dryR = inR[i];      // read both before writing: buffers may be shared
            float x[2] = { dryL, dryR };
            mixPre_ += (tPre - mixPre_) * aMix; mixEq_ += (tEq - mixEq_) * aMix;
            mixOpto_ += (tOpto - mixOpto_) * aMix; mixOn_ += (tOn - mixOn_) * aMix;
            outG_ += (tOut - outG_) * aMix;
            if (std::fabs(mixOn_ - tOn) < 1e-5f) mixOn_ = tOn;      // settle exactly, so OFF is bit-for-bit the input
            if (std::fabs(mixPre_ - tPre) < 1e-5f) mixPre_ = tPre;
            if (std::fabs(mixEq_ - tEq) < 1e-5f) mixEq_ = tEq;
            if (std::fabs(mixOpto_ - tOpto) < 1e-5f) mixOpto_ = tOpto;

            // TUBE PREAMP (2x oversampled)
            if (mixPre_ > 0.0001f)
                for (int c = 0; c < 2; ++c)
                {
                    const float s0 = 0.5f * (prevIn_[c] + x[c]), s1 = x[c];
                    prevIn_[c] = x[c];
                    float y0 = up2_[c].process(up1_[c].process(s0 * 2.0f));
                    float y1 = up2_[c].process(up1_[c].process(s1 * 2.0f));
                    y0 = tube(y0 * 0.5f, drive, bias, tb);
                    y1 = tube(y1 * 0.5f, drive, bias, tb);
                    dn2_[c].process(dn1_[c].process(y0));
                    float y = dn2_[c].process(dn1_[c].process(y1)) * post;
                    // DC blocker (the asymmetry makes a little DC)
                    const float d = y - dcX_[c] + 0.9995f * dcY_[c]; dcX_[c] = y; dcY_[c] = d;
                    x[c] += (d - x[c]) * mixPre_;
                }

            // EQUALIZER
            if (mixEq_ > 0.0001f)
                for (int c = 0; c < 2; ++c)
                {
                    const float y = ls_[c].process(pk_[c].process(hs_[c].process(x[c])));
                    x[c] += (y - x[c]) * mixEq_;
                }

            // OPTO LEVELER
            {
                const float lv = std::max(std::fabs(x[0]), std::fabs(x[1]));
                env_ += (lv - env_) * (lv > env_ ? aAtk * 3.0f : aEnvR);
                const float e = 20.0f * std::log10(env_ + 1e-9f);
                const float over = e - thr;
                float gr = 0;
                if (over > knee * 0.5f) gr = over * (1 - 1 / ratio);
                else if (over > -knee * 0.5f) { const float k = over + knee * 0.5f; gr = (1 - 1 / ratio) * k * k / (2 * knee); }
                if (comp < 0.05f) gr = 0;
                // fast cell + slow "memory" cell: the more it works, the slower it lets go
                grFast_ += (gr - grFast_) * (gr > grFast_ ? aAtk : aRelFast);
                grSlow_ += (gr - grSlow_) * (gr > grSlow_ ? aSlowAtk : (comp < 0.05f ? aRelFast : aRelSlow));
                const float g = std::max(grFast_ * 0.6f + grSlow_ * 0.4f, 0.0f);
                grMax = std::max(grMax, g * mixOpto_);
                if (mixOpto_ > 0.0001f)
                {
                    const float k = dbg(makeup - g);
                    for (int c = 0; c < 2; ++c) x[c] += (x[c] * k - x[c]) * mixOpto_;
                }
            }

            // MASTER
            float yl = x[0] * outG_, yr = x[1] * outG_;
            yl = dryL + (yl - dryL) * mixOn_;                // OFF = true bypass, faded so it never clicks
            yr = dryR + (yr - dryR) * mixOn_;
            outL[i] = yl; outR[i] = yr;
            const float m = 0.5f * (yl * yl + yr * yr);
            vu_ += (m - vu_) * aVu_;
        }
        grHold_ = std::max(grHold_, grMax);
        timer_ += (float)n / sr_;
        if (timer_ >= 1.0f / 30.0f)
        {
            timer_ = 0;
            grShown_ = grHold_; grHold_ = 0;
            needleGr_ += (vuPos(3.0f - grShown_) - needleGr_) * 0.5f;          // GR needle: 0 dB at the right
            const float vdb = 10.0f * std::log10(vu_ + 1e-12f) + 18.0f;         // 0 VU = -18 dBFS
            writeMeter(kGrMeter, needleGr_);
            writeMeter(kVuMeter, vuPos(vdb));
        }
    }

private:
    static constexpr float kHead = 0.35f;     // headroom: at LEVEL 0 a normal mix barely touches the curve
    static inline float tube(float x, float drive, float bias, float tb)
    {
        // asymmetric soft clip: tanh with a bias, so the two halves of the wave clip differently
        return std::tanh(x * drive + bias) - tb;
    }
    float outTarget() const
    {
        const float o = get(kOutput);
        return o <= -29.5f ? 0.0f : dbg(o);
    }
    void updateEq()
    {
        const float h = get(kHigh), f = get(kFreq), m = get(kMid), l = get(kLow);
        if (h == lastEq_[0] && f == lastEq_[1] && m == lastEq_[2] && l == lastEq_[3]) return;
        lastEq_[0] = h; lastEq_[1] = f; lastEq_[2] = m; lastEq_[3] = l;
        const BiquadCoeffs ch = BiquadCoeffs::highShelf(sr_, 10000.0f, 0.7f, h);
        const BiquadCoeffs cm = BiquadCoeffs::peak(sr_, freqOf(f), 0.9f, m);
        const BiquadCoeffs cl = BiquadCoeffs::lowShelf(sr_, 110.0f, 0.7f, l);
        for (int c = 0; c < 2; ++c) { hs_[c].setCoeffs(ch); pk_[c].setCoeffs(cm); ls_[c].setCoeffs(cl); }
    }
    void writeMeter(int i, float v)
    {
        v = std::round(mpc::clamp(v, 0.0f, 1.0f) * 95.0f) / 95.0f;            // 96 needle positions
        if (std::fabs(getNormalised(i) - v) < 1e-4f) return;
        setNormalised(i, v);
        notify_[i - kGrMeter].store(1);
    }

    float sr_ = 44100;
    Biquad up1_[2], up2_[2], dn1_[2], dn2_[2], hs_[2], pk_[2], ls_[2];
    float prevIn_[2] = {0, 0}, dcX_[2] = {0, 0}, dcY_[2] = {0, 0};
    float lastEq_[4] = {1e9f, 1e9f, 1e9f, 1e9f};
    float env_ = 0, grFast_ = 0, grSlow_ = 0, grHold_ = 0, grShown_ = 0, needleGr_ = 1, vu_ = 0, aVu_ = 0.01f;
    float mixPre_ = 1, mixEq_ = 1, mixOpto_ = 1, mixOn_ = 1, outG_ = 1, timer_ = 0;
    std::atomic<uint8_t> notify_[2];
    std::atomic<bool> running_{false};
    std::thread notifier_;
};

} // namespace

MPC_PLUGIN_ENTRY(MeltChannel)
