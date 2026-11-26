// Melt Stutter (T-MU) - vocal stutters on the MPC, as an insert effect.
//
// SPDX-License-Identifier: MIT
//
// CAPTURE   arms the plugin. CAPTURE length WORD: the next word is recorded on its own (it
//           starts when the sound starts and stops when the word ends; press again to stop).
//           1 BEAT .. 2 BARS: records exactly that long, starting on the next beat while the
//           sequencer plays (or on the first sound when it's stopped).
// GRAB      takes what just went past: the plugin always keeps the last 10 seconds. WORD grabs
//           the last word; the beat lengths grab exactly that much, ending on the last beat.
// PLAY WORD plays the captured word as it is.
// STUTTER   plays the stutter: the start of the word repeated (REPEATS x RATE, timed to the MPC
//           tempo), then the whole word. FADE IN ramps the repeats up, PAN throws them left and
//           right, PITCH makes them rise into the word, GATE sets how much of each slice sounds.
// EFFECTS   CHORUS, PHONE (telephone band + grit) and BACKVERB (a reversed reverb that swells in
//           before the stutter) are printed into the stutter.
// AUTO      fires the stutter on the downbeat every 1, 2 or 4 bars while the sequencer plays.
// SAVE WAV  writes the stutter to "Melt Stutter" on the MPC drive, ready to load on a pad.
//
// The stutter is rendered on a worker thread whenever a setting or the tempo changes, so the
// audio thread only plays back a finished buffer.
//
// MPC notes: the host nudges parameter 0 on every prepare, so parameter 0 is the status line
// (display only). Every button is momentary (pressed = 1, released = 0) and acts on the press;
// the plugin never writes its own buttons. Screen feedback (status, lights, waveform) is
// published from the worker thread, so it updates even while the MPC isn't sending audio.

#include "mpc_plugin.h"
#include "dsp/biquad.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#if defined(_WIN32)
  #include <filesystem>
  #include <system_error>
#else
  #include <sys/stat.h>
#endif
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// ---------------------------------------------------------------- files (MPC, macOS, Windows)
inline std::FILE* openOut(const std::string& path)
{
#if defined(MPC_DESKTOP)
    return mpc::openFile(path, "wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}
inline bool pathExists(const std::string& p)
{
#if defined(_WIN32)
    std::error_code ec; return std::filesystem::exists(std::filesystem::u8path(p), ec);
#else
    struct stat st; return ::stat(p.c_str(), &st) == 0;
#endif
}
inline void makeDir(const std::string& p)
{
#if defined(_WIN32)
    std::error_code ec; std::filesystem::create_directories(std::filesystem::u8path(p), ec);
#else
    ::mkdir(p.c_str(), 0775);
#endif
}

using mpc::dsp::Biquad;
using mpc::dsp::BiquadCoeffs;

enum
{
    kStatus = 0,                                   // parameter 0: display only (the MPC nudges it)
    kCapture, kGrab, kPlayWord, kStutter, kSave,
    kRepeats, kRate, kFade, kPan, kOutput, kPitch, kGate,
    kChoOn, kChoDepth, kChoMix, kPhoOn, kPhoTone, kPhoMix, kBvOn, kBvSize, kBvMix,
    kAuto, kMode, kSens, kCapLen,
    kBpm, kSig, kPlayhead, kSlice, kInMeter, kCapLight, kPlayLight, kWave1,
    kNumParams = kWave1 + 48
};
constexpr int kCols = 48;
const int kRepeatsV[] = { 2, 3, 4, 6, 8, 12, 16 };
const double kRateBeats[] = { 1.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125 };
const int kAutoBars[] = { 0, 1, 2, 4 };
const double kCapBeats[] = { 0, 1, 2, -1, -2 };     // 0 = word, >0 beats, <0 bars

struct Segment { long outStart, outLen; double src, step; };

// one finished render: the stutter phrase, the plain word, and a map for the playhead
struct Render
{
    std::vector<float> st[2], word[2];
    long stLen = 0, wordLen = 0, preroll = 0;
    int gen = 0;                       // which captured word this was made from
    std::vector<Segment> segs;
};

// ---------------------------------------------------------------- small offline effects
struct SimpleVerb
{
    std::vector<float> c[8], a[4]; int cp[8] = {}, ap[4] = {}; float cs[8] = {};
    void init(double sr, int spread)
    {
        static const int cl[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617}, al[4] = {556, 441, 341, 225};
        for (int i = 0; i < 8; ++i) { c[i].assign((size_t)((cl[i] + spread) * sr / 44100.0), 0.0f); cp[i] = 0; cs[i] = 0; }
        for (int i = 0; i < 4; ++i) { a[i].assign((size_t)((al[i] + spread) * sr / 44100.0), 0.0f); ap[i] = 0; }
    }
    float run(float x, float fb)
    {
        float o = 0;
        for (int i = 0; i < 8; ++i)
        {
            float& b = c[i][(size_t)cp[i]];
            const float y = b; cs[i] = y * 0.7f + cs[i] * 0.3f; b = x * 0.015f + cs[i] * fb;
            if (++cp[i] >= (int)c[i].size()) cp[i] = 0;
            o += y;
        }
        for (int i = 0; i < 4; ++i)
        {
            float& b = a[i][(size_t)ap[i]];
            const float v = b; const float y = -o + v; b = o + v * 0.5f; o = y;
            if (++ap[i] >= (int)a[i].size()) ap[i] = 0;
        }
        return o;
    }
};

inline float readLin(const std::vector<float>& v, long len, double p)
{
    if (p < 0 || p >= len - 1) return 0;
    const long i = (long)p; const float t = (float)(p - i);
    return v[(size_t)i] + (v[(size_t)i + 1] - v[(size_t)i]) * t;
}

class MeltStutter : public mpc::Plugin
{
public:
    MeltStutter() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::intParam("status", "Status", 0, 1000, 0));
        addParam(mpc::boolParam("capture", "Capture", false));
        addParam(mpc::boolParam("grab", "Grab", false));
        addParam(mpc::boolParam("playword", "Play Word", false));
        addParam(mpc::boolParam("stutter", "Stutter", false));
        addParam(mpc::boolParam("save", "Save WAV", false));
        addParam(mpc::choiceParam("repeats", "Repeats", { "2", "3", "4", "6", "8", "12", "16" }, 2));
        addParam(mpc::choiceParam("rate", "Rate", { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" }, 3));
        addParam(mpc::floatParam("fade", "Fade In", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(mpc::floatParam("pan", "Pan", 0.0f, 100.0f, 40.0f, "%", 0));
        addParam(mpc::floatParam("output", "Output", -24.0f, 6.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("pitch", "Pitch Rise", 0.0f, 12.0f, 0.0f, "st", 1));
        addParam(mpc::floatParam("gate", "Gate", 25.0f, 100.0f, 85.0f, "%", 0));
        addParam(mpc::boolParam("choon", "Chorus On", false));
        addParam(mpc::floatParam("chodepth", "Chorus Depth", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::floatParam("chomix", "Chorus Mix", 0.0f, 100.0f, 40.0f, "%", 0));
        addParam(mpc::boolParam("phoon", "Phone On", false));
        addParam(mpc::floatParam("photone", "Phone Tone", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::floatParam("phomix", "Phone Mix", 0.0f, 100.0f, 80.0f, "%", 0));
        addParam(mpc::boolParam("bvon", "Backverb On", false));
        addParam(mpc::floatParam("bvsize", "Backverb Size", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::floatParam("bvmix", "Backverb Mix", 0.0f, 100.0f, 60.0f, "%", 0));
        addParam(mpc::choiceParam("auto", "Auto", { "Off", "Every Bar", "Every 2 Bars", "Every 4 Bars" }, 0));
        addParam(mpc::choiceParam("mode", "Mode", { "Over", "Replace" }, 0));
        addParam(mpc::floatParam("sens", "Threshold", -60.0f, -20.0f, -45.0f, "dB", 0));
        addParam(mpc::choiceParam("caplen", "Capture", { "Word", "1 Beat", "2 Beats", "1 Bar", "2 Bars" }, 0));
        addParam(mpc::floatParam("bpm", "BPM", 0.0f, 300.0f, 120.0f, "", 1));
        addParam(mpc::intParam("sig", "Time Sig", 0, 1999, 404));
        addParam(mpc::floatParam("playhead", "Playhead", 0.0f, 1.0f, 0.0f, "", 2));
        addParam(mpc::floatParam("slice", "Slice", 0.0f, 1.0f, 0.0f, "", 2));
        addParam(mpc::floatParam("inmeter", "In Meter", 0.0f, 1.0f, 0.0f, "", 2));
        addParam(mpc::intParam("caplight", "Cap Light", 0, 2, 0));
        addParam(mpc::intParam("playlight", "Play Light", 0, 2, 0));
        for (int k = 0; k < kCols; ++k)
        {
            char id[16], nm[16];
            std::snprintf(id, sizeof id, "wave%d", k + 1); std::snprintf(nm, sizeof nm, "Wave %d", k + 1);
            addParam(mpc::intParam(id, nm, 0, 16, 0));
        }
        for (auto& f : notify_) f.store(0);
    }
    ~MeltStutter() override
    {
        running_.store(false);
        if (worker_.joinable()) worker_.join();
        delete cur_; delete pending_.load(); delete retired_.load();
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Stutter"; i.vendor = "T-MU";
        i.uniqueId = MPC_FOURCC('T', 'm', 's', 't');
        i.version = 1000; i.isInstrument = false; i.numInputs = 2; i.numOutputs = 2;
        i.tailSamples = 44100 * 30;
        return i;
    }

#if defined(MPC_DESKTOP)
    // drag the SAVE WAV button into your DAW: it saves the stutter and hands over the file
    int desktopDragParam() const override { return kSave; }
    std::string desktopDragFile() override
    {
        if (!word_) return {};
        const int before = saveCount_.load();
        saveReq_.store(true);
        for (int k = 0; k < 150 && saveCount_.load() == before; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (saveFail_.load()) return {};
        std::lock_guard<std::mutex> g(pathMu_);
        return lastPath_;
    }
#endif

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[64];
        switch (index)
        {
            case kStatus: out = statusText(); return true;
            case kBpm: std::snprintf(b, sizeof b, "%.1f", plain); out = b; return true;
            case kSig: std::snprintf(b, sizeof b, "%d/%d", (int)plain / 100, (int)plain % 100); out = b; return true;
            case kPitch: std::snprintf(b, sizeof b, "+%.1f st", plain); out = b; return true;
            case kOutput: case kSens: std::snprintf(b, sizeof b, "%+.1f dB", plain); out = b; return true;
            case kFade: case kPan: case kGate: case kChoDepth: case kChoMix: case kPhoTone: case kPhoMix: case kBvSize: case kBvMix:
                std::snprintf(b, sizeof b, "%d%%", (int)std::lround(plain)); out = b; return true;
            case kPlayhead: case kSlice: case kInMeter: case kCapLight: case kPlayLight: out = ""; return true;
            default:
                if (index >= kWave1) { out = ""; return true; }
                return false;
        }
    }

    void prepare(double sr, int) override
    {
        if (running_.exchange(false) && worker_.joinable()) worker_.join();   // prepare can repeat: stop the worker first
        sr_ = (float)sr;
        ringN_ = (long)(sr * 10.0);                                            // the last 10 seconds, always
        ring_[0].assign((size_t)ringN_, 0.0f); ring_[1].assign((size_t)ringN_, 0.0f);
        capMax_ = (long)(sr * 8.0);
        preN_ = (long)(sr * 0.04);
        total_ = 0;
        for (int i = kCapture; i <= kSave; ++i) last_[i] = getBool(i);
        capState_.store(word_ ? kReady : kIdle);
        running_.store(true);
        worker_ = std::thread([this] { workerLoop(); });
        needRender_.store(true);
    }

    void reset() override
    {
        playing_.store(0); playPos_ = 0; env_ = 0; duck_ = 1; pendingPlay_ = 0;
        const int st = capState_.load();
        if (st == kArmed || st == kWaitBeat || st == kRec) capState_.store(word_ ? kReady : kIdle);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        const mpc::Transport& tr = ctx.transport;
        const float bpm = (tr.valid && tr.tempoValid && tr.bpm > 20) ? (float)tr.bpm : 120.0f;
        hostBpm_.store(bpm);
        const bool seq = tr.valid && tr.playing && tr.ppqValid;
        const int num = tr.valid ? std::max(1, tr.timeSigNumerator) : 4, den = tr.valid ? std::max(1, tr.timeSigDenominator) : 4;
        sig_.store(mpc::clamp(num, 1, 19) * 100 + mpc::clamp(den, 1, 99));
        const double spq = 60.0 / bpm * sr_;                                  // samples per beat

        // swap in a fresh render (lock-free: the worker deletes the old one)
        if (Render* p = pending_.load(std::memory_order_acquire))
            if (!retired_.load(std::memory_order_acquire))
            {
                pending_.store(nullptr, std::memory_order_release);
                retired_.store(cur_, std::memory_order_release);
                cur_ = p;
            }

        // buttons: act on the press only
        bool down[kSave + 1] = {};
        for (int i = kCapture; i <= kSave; ++i)
        {
            const bool v = getBool(i);
            down[i] = v && !last_[i];
            last_[i] = v;
        }
        const double capBeatsSet = kCapBeats[mpc::clamp(getInt(kCapLen), 0, 4)];
        const double capBeats = capBeatsSet < 0 ? -capBeatsSet * num * 4.0 / den : capBeatsSet;
        if (down[kCapture])
        {
            const int st = capState_.load();
            if (st == kArmed || st == kWaitBeat) capState_.store(word_ ? kReady : kIdle);   // press again: cancel
            else if (st == kRec) finishCapture();                                            // press again: stop now
            else if (st != kDone)
            {
                capFixed_ = capBeats > 0 ? (long)(capBeats * spq) : 0;
                capState_.store(capFixed_ && seq ? kWaitBeat : kArmed);
            }
            bump();
        }
        if (down[kGrab] && capState_.load() != kDone && capState_.load() != kRec)
        {
            // what just went past: WORD = the last sound in the last 3 s; beats = exactly that, ending on the last beat
            long end = 0, len;
            if (capBeats > 0)
            {
                len = std::min(capMax_, (long)(capBeats * spq));
                if (seq) end = (long)((tr.ppqPos - std::floor(tr.ppqPos)) * spq);
            }
            else len = std::min(capMax_, (long)(3.0f * sr_));
            capStart_ = total_ - end - len;
            capLen_ = len;
            if (capStart_ < 0) { capLen_ += capStart_; capStart_ = 0; }
            capWord_ = capBeats <= 0; capTrim_ = capBeats <= 0;
            capState_.store(capLen_ > 0 ? kDone : capState_.load());
            bump();
        }
        if (down[kPlayWord]) { pendingPlay_ = 1; bump(); }
        if (down[kStutter]) { pendingPlay_ = 2; bump(); }
        if (down[kSave]) saveReq_.store(true);
        // play as soon as the sound is ready (right after a capture the stutter is still being made)
        if (pendingPlay_ && !word_ && capState_.load() != kDone) pendingPlay_ = 0;     // nothing captured yet
        if (pendingPlay_ && cur_ && cur_->gen == wordGen_.load() && (pendingPlay_ == 1 ? cur_->wordLen : cur_->stLen) > 0)
        { playing_.store(pendingPlay_); playPos_ = 0; pendingPlay_ = 0; bump(); }

        // AUTO: start so the stutter (after any backverb swell) lands on the downbeat
        long autoStart = -1;
        const int bars = kAutoBars[mpc::clamp(getInt(kAuto), 0, 3)];
        if (bars && seq && cur_ && cur_->stLen)
        {
            const double period = bars * num * 4.0 / den;
            const double pre = cur_->preroll / spq;
            const double p0 = tr.ppqPos, p1 = p0 + n / spq;
            const double next = std::ceil((p0 + pre - 1e-9) / period) * period - pre;
            if (next >= p0 && next < p1 && std::fabs(next - lastAuto_) > 1e-6)
            {
                autoStart = (long)((next - p0) * spq); lastAuto_ = next;
            }
        }
        // CAPTURE on the beat: where in this block the next beat falls
        long beatAt = -1;
        if (capState_.load() == kWaitBeat)
        {
            if (!seq) capState_.store(kArmed);
            else
            {
                const double nb = std::ceil(tr.ppqPos - 1e-9);
                const long off = (long)((nb - tr.ppqPos) * spq);
                if (off < n) beatAt = std::max(0L, off);
            }
        }

        const float outG = std::pow(10.0f, get(kOutput) / 20.0f);
        const float thr = std::pow(10.0f, get(kSens) / 20.0f);
        const bool replace = getInt(kMode) == 1;
        const float aRel = 1.0f - std::exp(-1.0f / (0.05f * sr_));
        const float* inL = ctx.inputs[0];
        const float* inR = ctx.numInputs > 1 ? ctx.inputs[1] : ctx.inputs[0];
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        const Render* r = cur_;
        float pk = 0;
        for (int i = 0; i < n; ++i)
        {
            const float l = inL[i], rr = inR[i];
            if (i == autoStart && r) { playing_.store(2); playPos_ = 0; bump(); }
            const float a = std::max(std::fabs(l), std::fabs(rr));
            pk = std::max(pk, a);
            env_ = a > env_ ? a : env_ + (a - env_) * aRel;
            // every input sample goes into the 10 second memory
            const long w = (long)(total_ % ringN_);
            ring_[0][(size_t)w] = l; ring_[1][(size_t)w] = rr;
            const int st = capState_.load(std::memory_order_relaxed);
            if (st == kWaitBeat && i == beatAt)
            {
                capStart_ = total_; capLen_ = 0; capWord_ = false; capTrim_ = false; capState_.store(kRec); bump();
            }
            else if (st == kArmed && env_ > thr)
            {
                // start 40 ms early so the attack isn't cut
                capStart_ = std::max(0LL, total_ - preN_); capLen_ = (long)(total_ - capStart_);
                capWord_ = capFixed_ == 0; capTrim_ = capFixed_ == 0;
                quiet_ = 0; capState_.store(kRec); bump();
            }
            if (capState_.load(std::memory_order_relaxed) == kRec)
            {
                ++capLen_;
                quiet_ = env_ < thr ? quiet_ + 1 : 0;
                const bool wordEnd = capWord_ && quiet_ > (long)(0.3f * sr_) && capLen_ > (long)(0.12f * sr_);
                const bool full = capFixed_ ? capLen_ >= capFixed_ + (capTrim_ ? preN_ : 0) : capLen_ >= capMax_;
                if (wordEnd || full || capLen_ >= capMax_) finishCapture();
            }
            ++total_;

            // playback
            float pl = 0, pr = 0;
            const int pm = playing_.load(std::memory_order_relaxed);
            if (pm && r)
            {
                const std::vector<float>* b = pm == 1 ? r->word : r->st;
                const long len = pm == 1 ? r->wordLen : r->stLen;
                if (playPos_ < len) { pl = b[0][(size_t)playPos_]; pr = b[1][(size_t)playPos_]; ++playPos_; }
                else { playing_.store(0); bump(); }
            }
            const float dt = (replace && pm) ? 0.0f : 1.0f;
            duck_ += (dt - duck_) * 0.003f;
            outL[i] = softClip(l * duck_ + pl * outG);
            outR[i] = softClip(rr * duck_ + pr * outG);
        }
        inPeak_.store(std::max(inPeak_.load(std::memory_order_relaxed), pk), std::memory_order_relaxed);
        // playhead
        float ph = 0;
        const int pm = playing_.load();
        if (pm && r && r->wordLen)
        {
            if (pm == 1) ph = (float)playPos_ / r->wordLen;
            else for (const Segment& sg : r->segs)
                if (playPos_ >= sg.outStart && playPos_ < sg.outStart + sg.outLen)
                { ph = (float)((sg.src + (playPos_ - sg.outStart) * sg.step) / r->wordLen); break; }
        }
        playhead_.store(ph);
    }

private:
    enum { kIdle, kArmed, kWaitBeat, kRec, kDone, kReady };

    static inline float softClip(float x)
    {
        const float a = std::fabs(x);
        if (a < 0.9f) return x;
        const float y = 0.9f + 0.1f * std::tanh((a - 0.9f) / 0.1f);
        return x < 0 ? -y : y;
    }
    void bump() { statusDirty_.store(true); }
    void finishCapture() { capState_.store(kDone); bump(); }
    void writeParam(int i, float v)
    {
        v = mpc::clamp(v, 0.0f, 1.0f);
        if (std::fabs(getNormalised(i) - v) < 1e-5f) return;
        setNormalised(i, v);
        notify_[i].store(1, std::memory_order_relaxed);
    }

    std::string statusText() const
    {
        char b[48];
        switch (capState_.load())
        {
            case kArmed: return "LISTENING - PLAY THE VOCAL";
            case kWaitBeat: return "RECORDS ON THE NEXT BEAT";
            case kRec: return "RECORDING...";
            case kDone: return "GOT IT";
            default: break;
        }
        if (playing_.load() == 2) return "STUTTER";
        if (playing_.load() == 1) return "PLAY WORD";
        if (pendingPlay_) return "GETTING IT READY...";
        if (nothing_.load()) return "NOTHING HEARD - TURN THRESHOLD LEFT";
        if (saved_.load() > 0 && savedShow_.load())
        { std::snprintf(b, sizeof b, "SAVED  Stutter %03d.wav", saved_.load()); return b; }
        if (saveFail_.load()) return "COULDN'T SAVE - CHECK DRIVE";
        if (!word_) return "PRESS CAPTURE OR GRAB";
        std::snprintf(b, sizeof b, "WORD %.2f s  -  READY", wordSecs_.load());
        return b;
    }

    // ------------------------------------------------------------ worker
    struct Settings
    {
        int repeats, rate; float fade, pan, pitch, gate, bpm; bool cho, pho, bv; float chD, chM, phT, phM, bvS, bvM;
        bool operator!=(const Settings& o) const { return std::memcmp(this, &o, sizeof *this) != 0; }
    };
    Settings snap() const
    {
        Settings s; std::memset(&s, 0, sizeof s);
        s.repeats = kRepeatsV[mpc::clamp(getInt(kRepeats), 0, 6)]; s.rate = mpc::clamp(getInt(kRate), 0, 5);
        s.fade = get(kFade) / 100.0f; s.pan = get(kPan) / 100.0f; s.pitch = get(kPitch); s.gate = get(kGate) / 100.0f;
        s.bpm = std::round(hostBpm_.load() * 100.0f) / 100.0f;
        s.cho = getBool(kChoOn); s.pho = getBool(kPhoOn); s.bv = getBool(kBvOn);
        s.chD = get(kChoDepth) / 100.0f; s.chM = get(kChoMix) / 100.0f; s.phT = get(kPhoTone) / 100.0f; s.phM = get(kPhoMix) / 100.0f;
        s.bvS = get(kBvSize) / 100.0f; s.bvM = get(kBvMix) / 100.0f;
        return s;
    }

    void workerLoop()
    {
        Settings last; std::memset(&last, 0xff, sizeof last);
        while (running_.load())
        {
            if (Render* old = retired_.exchange(nullptr)) delete old;
            if (capState_.load() == kDone) takeCapture();
            const Settings s = snap();
            if (word_ && (s != last || needRender_.exchange(false)))
            {
                last = s;
                if (!pending_.load()) { Render* r = render(s); if (Render* p = pending_.exchange(r)) delete p; }
                else needRender_.store(true);
            }
            if (saveReq_.exchange(false)) saveWav();
            if (savedShow_.load() && --savedTicks_ <= 0) { savedShow_.store(false); bump(); }
            publishScreen();
            for (int i = 0; i < kNumParams; ++i)
                if (notify_[i].load(std::memory_order_relaxed) && notify_[i].exchange(0)) notifyHostParameterChanged(i);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // the screen: written from this thread, so it updates even if the MPC isn't running audio
    void publishScreen()
    {
        writeParam(kBpm, hostBpm_.load() / 300.0f);
        writeParam(kSig, (float)sig_.load() / 1999.0f);
        writeParam(kPlayhead, std::round(mpc::clamp(playhead_.load(), 0.0f, 1.0f) * 63.0f) / 63.0f);
        const float pk = inPeak_.exchange(0.0f);
        const float db = pk > 1e-6f ? 20.0f * std::log10(pk) : -100.0f;
        meter_ = std::max(mpc::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f), meter_ - 0.06f);
        writeParam(kInMeter, std::round(meter_ * 64.0f) / 64.0f);
        const int st = capState_.load();
        writeParam(kCapLight, (st == kRec || st == kDone) ? 1.0f : (st == kArmed || st == kWaitBeat) ? 0.5f : 0.0f);
        const int pm = playing_.load();
        writeParam(kPlayLight, pm == 2 ? 1.0f : pm == 1 ? 0.5f : 0.0f);
        if (statusDirty_.exchange(false)) { statusCount_ = (statusCount_ + 1) % 1000; writeParam(kStatus, statusCount_ / 1000.0f); }
    }

    void takeCapture()
    {
        const float thr = std::pow(10.0f, get(kSens) / 20.0f);
        // copy out of the 10 second memory (the audio thread keeps writing ahead of this, never over it)
        long len = std::min(capLen_, capMax_);
        cap_[0].resize((size_t)std::max(1L, len)); cap_[1].resize((size_t)std::max(1L, len));
        for (long k = 0; k < len; ++k)
        {
            const long j = (long)((capStart_ + k) % ringN_);
            cap_[0][(size_t)k] = ring_[0][(size_t)j]; cap_[1][(size_t)k] = ring_[1][(size_t)j];
        }
        if (capWord_ && capTrim_ && capStart_ + len <= total_ && capLen_ >= (long)(2.9f * sr_))
        {
            // GRAB in WORD mode: keep only the last sound (go back from the end to the gap before it)
            long e = len;
            while (e > 0 && std::max(std::fabs(cap_[0][(size_t)e - 1]), std::fabs(cap_[1][(size_t)e - 1])) < thr) --e;
            long b = e, gap = 0;
            while (b > 0 && gap < (long)(0.25f * sr_)) { --b; gap = std::max(std::fabs(cap_[0][(size_t)b]), std::fabs(cap_[1][(size_t)b])) < thr * 0.5f ? gap + 1 : 0; }
            if (e > b) { cap_[0].assign(cap_[0].begin() + b, cap_[0].begin() + std::min(len, e + (long)(0.3f * sr_)));
                         cap_[1].assign(cap_[1].begin() + b, cap_[1].begin() + std::min(len, e + (long)(0.3f * sr_)));
                         len = (long)cap_[0].size(); }
        }
        // WORD: trim the quiet ends (leave 60 ms so the word can ring). Beat lengths stay exact.
        long end = len, start = 0;
        if (capTrim_)
        {
            while (end > 0 && std::max(std::fabs(cap_[0][(size_t)end - 1]), std::fabs(cap_[1][(size_t)end - 1])) < thr * 0.5f) --end;
            end = std::min(len, end + (long)(0.06f * sr_));
            while (start < end && std::max(std::fabs(cap_[0][(size_t)start]), std::fabs(cap_[1][(size_t)start])) < thr * 0.5f) ++start;
            start = std::max(0L, start - (long)(0.004f * sr_));      // a few ms before the first sound
        }
        const long wl = std::max(0L, end - start);
        if (wl < (long)(0.03f * sr_))
        {   // nothing worth keeping: keep the old word, say so
            nothing_.store(true);
            capState_.store(word_ ? kReady : kIdle);
            bump();
            return;
        }
        nothing_.store(false);
        wordL_.assign(cap_[0].begin() + start, cap_[0].begin() + start + wl);
        wordR_.assign(cap_[1].begin() + start, cap_[1].begin() + start + wl);
        // 3 ms fades so it never clicks
        const long f = std::min(wl / 2, (long)(0.003f * sr_));
        for (long k = 0; k < f; ++k)
        {
            const float g = (float)k / f;
            wordL_[(size_t)k] *= g; wordR_[(size_t)k] *= g;
            wordL_[(size_t)(wl - 1 - k)] *= g; wordR_[(size_t)(wl - 1 - k)] *= g;
        }
        word_ = wl > 0;
        wordSecs_.store((float)wl / sr_);
        // waveform display
        for (int c = 0; c < kCols; ++c)
        {
            const long a = wl * c / kCols, b = std::max(a + 1, wl * (c + 1) / kCols);
            float pk = 0;
            for (long k = a; k < b && k < wl; ++k) pk = std::max(pk, std::max(std::fabs(wordL_[(size_t)k]), std::fabs(wordR_[(size_t)k])));
            wavePk_[c] = pk;
        }
        float mx = 1e-6f; for (float v : wavePk_) mx = std::max(mx, v);
        for (int c = 0; c < kCols; ++c) writeParam(kWave1 + c, std::round(std::sqrt(wavePk_[c] / mx) * 16.0f) / 16.0f);
        wordGen_.fetch_add(1);
        needRender_.store(true);
        capState_.store(word_ ? kReady : kIdle);
        bump();
    }

    Render* render(const Settings& s)
    {
        Render* r = new Render();
        r->gen = wordGen_.load();
        const long wl = (long)wordL_.size();
        r->word[0] = wordL_; r->word[1] = wordR_; r->wordLen = wl;
        const double beat = 60.0 / std::max(20.0f, s.bpm) * sr_;
        const long slice = std::max(64L, (long)(kRateBeats[s.rate] * beat));
        const long sound = std::max(32L, (long)(slice * s.gate));
        writeParam(kSlice, std::round(mpc::clamp((float)sound / std::max(1L, wl), 0.0f, 1.0f) * 63.0f) / 63.0f);

        // BACKVERB swell: reverb of the first slice, reversed, ending where the stutter starts
        std::vector<float> bv[2];
        long pre = 0;
        if (s.bv)
        {
            pre = (long)((0.35f + 1.65f * s.bvS) * sr_);
            for (int c = 0; c < 2; ++c)
            {
                SimpleVerb v; v.init(sr_, c * 23);
                bv[c].assign((size_t)pre, 0.0f);
                const float fb = 0.8f + 0.17f * s.bvS;
                for (long k = 0; k < pre; ++k)
                {
                    const float x = k < std::min(sound, wl) ? (c ? wordR_ : wordL_)[(size_t)k] : 0.0f;
                    bv[c][(size_t)k] = v.run(x, fb);
                }
                std::reverse(bv[c].begin(), bv[c].end());
            }
            float pk = 1e-6f, wp = 1e-6f;
            for (int c = 0; c < 2; ++c) for (float x : bv[c]) pk = std::max(pk, std::fabs(x));
            for (long k = 0; k < std::min(sound, wl); ++k) wp = std::max(wp, std::fabs(wordL_[(size_t)k]));
            const float g = wp / pk * s.bvM;
            for (int c = 0; c < 2; ++c)
                for (long k = 0; k < pre; ++k) bv[c][(size_t)k] *= g * std::min(1.0f, (float)k / (0.1f * pre + 1));
        }

        const long body = slice * s.repeats + wl;
        const long total = pre + body;
        for (int c = 0; c < 2; ++c) r->st[c].assign((size_t)total + 1, 0.0f);
        for (int c = 0; c < 2; ++c) for (long k = 0; k < pre; ++k) r->st[c][(size_t)k] = bv[c][(size_t)k];
        r->preroll = pre; r->stLen = total;

        // the repeats
        const long fadeOut = std::min(sound / 3, (long)(0.004f * sr_));
        for (int k = 0; k < s.repeats; ++k)
        {
            const float t = (float)k / s.repeats;
            const float g = 1.0f - s.fade * (1.0f - t);
            const float semis = -s.pitch * (1.0f - t);
            const double step = std::pow(2.0, semis / 12.0);
            const float p = (k % 2 ? 1.0f : -1.0f) * s.pan;                       // throw left, right, left...
            const float gl = g * std::cos((p + 1) * 0.25f * 3.14159265f) * 1.41421356f;
            const float gr = g * std::sin((p + 1) * 0.25f * 3.14159265f) * 1.41421356f;
            const long o = pre + k * slice;
            const long len = std::min(sound, (long)((wl - 1) / step));
            for (long j = 0; j < len; ++j)
            {
                float env = 1.0f;
                if (j > len - fadeOut) env = (float)(len - j) / fadeOut;
                const double sp = j * step;
                r->st[0][(size_t)(o + j)] += readLin(wordL_, wl, sp) * gl * env;
                r->st[1][(size_t)(o + j)] += readLin(wordR_, wl, sp) * gr * env;
            }
            r->segs.push_back({ o, slice, 0.0, step });
        }
        // then the whole word
        const long wo = pre + slice * s.repeats;
        for (long j = 0; j < wl; ++j) { r->st[0][(size_t)(wo + j)] += wordL_[(size_t)j]; r->st[1][(size_t)(wo + j)] += wordR_[(size_t)j]; }
        r->segs.push_back({ wo, wl, 0.0, 1.0 });

        // PHONE: telephone band, tone moves it, a little grit
        if (s.pho)
        {
            const float centre = 600.0f * std::pow(2200.0f / 600.0f, s.phT);
            for (int c = 0; c < 2; ++c)
            {
                Biquad h1, h2, l1, l2;
                h1.setCoeffs(BiquadCoeffs::highpass(sr_, centre / 2.4f, 0.8f)); h2.setCoeffs(BiquadCoeffs::highpass(sr_, centre / 2.4f, 0.8f));
                l1.setCoeffs(BiquadCoeffs::lowpass(sr_, centre * 2.4f, 0.8f)); l2.setCoeffs(BiquadCoeffs::lowpass(sr_, centre * 2.4f, 0.8f));
                for (long k = 0; k < total; ++k)
                {
                    float& x = r->st[c][(size_t)k];
                    const float y = std::tanh(l2.process(l1.process(h2.process(h1.process(x)))) * 3.0f) * 0.6f;
                    x += (y - x) * s.phM;
                }
            }
        }
        // CHORUS: two moving voices
        if (s.cho)
        {
            const long dmax = (long)(0.04f * sr_);
            std::vector<float> d[2];
            for (int c = 0; c < 2; ++c) d[c] = r->st[c];
            for (int c = 0; c < 2; ++c)
                for (long k = 0; k < total; ++k)
                {
                    const double t = (double)k / sr_;
                    const double m1 = (0.012 + 0.006 * s.chD * (1 + std::sin(2 * M_PI * 0.7 * t + c * 1.6))) * sr_;
                    const double m2 = (0.019 + 0.008 * s.chD * (1 + std::sin(2 * M_PI * 1.1 * t + 2.1 + c * 1.1))) * sr_;
                    const float w = 0.5f * (readLin(d[c], total, k - std::min((double)dmax, m1)) + readLin(d[1 - c], total, k - std::min((double)dmax, m2)));
                    r->st[c][(size_t)k] = d[c][(size_t)k] * (1.0f - 0.4f * s.chM) + w * s.chM;
                }
        }
        return r;
    }

    // SAVE WAV -> <MPC drive>/Melt Stutter/Stutter 001.wav
    void saveWav()
    {
        const Render* r = pending_.load() ? pending_.load() : cur_;
        if (!r || !r->stLen) { saveFail_.store(true); saveCount_.fetch_add(1); bump(); return; }
#if defined(MPC_DESKTOP)
        const std::string dir = mpc::pluginFolder();                       // Music/T-MU/Melt Stutter
#else
        std::string root = mpc::pluginFolder();
        for (std::string p = root; p.size() > 1;)
        {
            const size_t s = p.find_last_of('/');
            if (s == std::string::npos || s == 0) break;
            if (p.substr(s + 1) == "VST2 Plugins") { root = p.substr(0, s); break; }
            p = p.substr(0, s);
        }
        const std::string dir = root + "/Melt Stutter";
#endif
        makeDir(dir);
        std::string path; int idx = 1;
        for (; idx < 1000; ++idx)
        {
            char nm[64]; std::snprintf(nm, sizeof nm, "/Stutter %03d.wav", idx);
            path = dir + nm;
            if (!pathExists(path)) break;
        }
        FILE* f = openOut(path);
        if (!f) { saveFail_.store(true); saveCount_.fetch_add(1); bump(); return; }
        const uint32_t frames = (uint32_t)r->stLen, rate = (uint32_t)sr_, bytes = frames * 6;
        auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
        w32(16); w16(1); w16(2); w32(rate); w32(rate * 6); w16(6); w16(24);
        std::fwrite("data", 1, 4, f); w32(bytes);
        std::vector<uint8_t> buf; buf.reserve(6 * 4096);
        for (uint32_t k = 0; k < frames; ++k)
        {
            for (int c = 0; c < 2; ++c)
            {
                const int32_t v = (int32_t)std::lround(mpc::clamp(r->st[c][k], -1.0f, 1.0f) * 8388607.0f);
                buf.push_back((uint8_t)(v & 0xff)); buf.push_back((uint8_t)((v >> 8) & 0xff)); buf.push_back((uint8_t)((v >> 16) & 0xff));
            }
            if (buf.size() >= 6 * 4096) { std::fwrite(buf.data(), 1, buf.size(), f); buf.clear(); }
        }
        std::fwrite(buf.data(), 1, buf.size(), f);
        const bool ok = std::fclose(f) == 0;
        saveFail_.store(!ok);
        if (ok) { saved_.store(idx); savedShow_.store(true); savedTicks_ = 200; { std::lock_guard<std::mutex> g(pathMu_); lastPath_ = path; } }
        saveCount_.fetch_add(1);
        bump();
    }

    // state
    float sr_ = 44100, env_ = 0, duck_ = 1;
    std::vector<float> ring_[2], cap_[2];
    long ringN_ = 1, capMax_ = 1, capLen_ = 0, capFixed_ = 0, preN_ = 1, quiet_ = 0;
    long long total_ = 0, capStart_ = 0;
    bool capWord_ = true, capTrim_ = true;
    std::atomic<int> capState_{kIdle};
    bool last_[kSave + 1] = {};
    std::atomic<int> playing_{0};
    int pendingPlay_ = 0;
    long playPos_ = 0;
    double lastAuto_ = -1e9;
    Render* cur_ = nullptr;
    std::atomic<Render*> pending_{nullptr}, retired_{nullptr};
    std::atomic<float> inPeak_{0}, playhead_{0};
    std::atomic<int> sig_{404}, wordGen_{0};
    std::atomic<bool> nothing_{false};
    float meter_ = 0;
    // worker-owned
    std::vector<float> wordL_, wordR_;
    bool word_ = false;
    float wavePk_[kCols] = {};
    int savedTicks_ = 0;
    std::atomic<float> wordSecs_{0}, hostBpm_{120};
    std::atomic<int> saved_{0};
    std::atomic<bool> savedShow_{false}, saveFail_{false}, saveReq_{false}, needRender_{true}, statusDirty_{true};
    int statusCount_ = 0;
    std::atomic<int> saveCount_{0};
    std::mutex pathMu_;
    std::string lastPath_;
    std::atomic<uint8_t> notify_[kNumParams];
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace

MPC_PLUGIN_ENTRY(MeltStutter)
