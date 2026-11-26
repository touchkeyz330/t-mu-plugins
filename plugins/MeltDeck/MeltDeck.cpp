// Melt Deck - DJ decks for Akai MPC standalone.
//
// SPDX-License-Identifier: MIT
//
// The track library (Content/index.txt + Content/tracks/...) is written by
// make_meltdeck.py, which converts every track to 44.1 kHz 16-bit WAV and finds
// its BPM, beat grid, key and first sound.
//
// Mode:   1 Deck (play over your MPC beat: Sync follows the MPC tempo and bar)
//         or 2 Decks (A + B, crossfader, sync deck to deck).
// Decks:  Play, Cue (CDJ style: hold to preview), Sync, beat loops (1 beat to 16
//         bars), beat jump, nudge, jog, brake, 4 hot cues on screen / 8 on pads,
//         3-band isolator EQ with kills, filter (LP left, HP right), pitch fader.
//         A playing deck is locked: scrolling its track list only loads after you
//         pause it.
// Pads:   (bottom row first) CUE PLAY NUDGE- NUDGE+ / SYNC JUMP- JUMP+ BRAKE /
//         1 BAR 2 BARS 4 BARS 8 BARS / DECK A DECK B LOOP SHIFT.
//         SHIFT + bottom two rows = hot cues 1-8; SHIFT + LOOP, then a hot cue,
//         clears it.
// Cue:    decks with CUE on go to outputs 3-4 (headphones, if routed).
//
// Buttons on the screen are triggers: every tap acts once. The Status line under
// each deck shows what the deck is doing.
//
// Parameters (order and ids are permanent: append only)
//   0 Volume 1 Crossfader 2 A Track 3 B Track 4 A Level 5 B Level 6 A Tempo
//   7 B Tempo 8 A Filter 9 B Filter 10 A Low 11 A Mid 12 A High 13 B Low 14 B Mid
//   15 B High 16 Mode 17 Deck 18 Play 19 Cue 20 Sync 21 Loop 22 Loop Size
//   23 Jump Back 24 Jump Fwd 25 Brake 26 Jog 27 A Crate 28 B Crate 29 A Status
//   30 B Status 31 A Cue 32 B Cue 33 Cue Mix 34 Pad Start 35-38 Hot Cue 1-4
//   39 FX Type 40 FX Beats 41 FX Amount 42 A FX 43 B FX 44 Auto Gain 45 Sort
//
// Beat FX: Echo, Reverb, Flanger, Roll, Crush, Gate - timed to each deck's own
// beat. Turn FX on per deck. SHIFT + LOOP-size pads = loop rolls (1/4 to 2 beats)
// while held; the track keeps running underneath, so it drops back in on time.
// Auto Gain plays every track at the same loudness (measured by the builder).
//
//   46-63 Deck A: Play Cue Sync Loop LoopHalf LoopDouble LoopIn LoopOut Hot1-3
//         Gain Jog BPM Time Info Meter LoopSize     64-81 the same for Deck B
//   82 Phase 83 Shift 84 BPM Up 85 BPM Down 86 Tempo Reset 87 Browse 88-90 Loop
//   2/4/8 Bars 91-92 Nudge Down/Up 93 Sampler 94 Sampler Vol 95 Sampler Play
//   96 Live View 97-136 A Zoom 137-176 B Zoom 177-216 A Wave 217-256 B Wave
//
// Live screen: the plugin writes its own display parameters (time, BPM, meters,
// waveform columns, spinning platters, lit buttons) and tells the MPC they moved.
// Those writes happen on the audio thread as plain atomic stores; a separate
// notifier thread passes them on to the host.

#include "mpc_plugin.h"
#include "dsp/svf.h"
#include "dsp/delay_line.h"
#include "md_config.h"
#include "md_library.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#if defined(__linux__)
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#endif
#include <string>
#include <thread>
#include <vector>

namespace {

enum
{
    kVolume = 0, kXfade, kATrack, kBTrack, kALevel, kBLevel, kATempo, kBTempo, kAFilter, kBFilter,
    kALow, kAMid, kAHigh, kBLow, kBMid, kBHigh, kMode, kDeck, kPlay, kCue, kSync, kLoop, kLoopSize,
    kJumpBack, kJumpFwd, kBrake, kJog, kACrate, kBCrate, kAStatus, kBStatus, kACue, kBCue, kCueMix,
    kPadStart, kHot1, kHot2, kHot3, kHot4, kFxType, kFxBeats, kFxAmount, kAFx, kBFx, kAutoGain, kSort,
    kAPlay
};
enum DeckParam { oPlay, oCue, oSync, oLoop, oLoopHalf, oLoopDouble, oLoopIn, oLoopOut, oHot1, oHot2, oHot3,
                 oGain, oJog, oBpm, oTime, oInfo, oMeter, oLoopSize, kDeckParams };
constexpr int kCols = 40;
enum : int
{
    kBPlay = kAPlay + kDeckParams,
    kPhase = kBPlay + kDeckParams, kShift, kBpmUp, kBpmDown, kTempoReset, kBrowse, kLoop2, kLoop4, kLoop8,
    kNudgeDown, kNudgeUp, kSampler, kSamplerVol, kSamplerPlay, kLiveView,
    kAZoom, kBZoom = kAZoom + kCols, kAWave = kBZoom + kCols, kBWave = kAWave + kCols,
    kRescan = kBWave + kCols, kLibInfo, kPadMode, kFxPad1,
    kBrowseSel = kFxPad1 + 16, kBrowseRow, kBrowsePath, kBrowseLine1,
    kBrowseOpen = kBrowseLine1 + 8, kBrowseBack, kBrowseLoadA, kBrowseLoadB, kBrowseAdd, kBrowsePgUp, kBrowsePgDn, kBrowseDrives,
    kJogMode,
    kPadMap, kPadLearn, kPadInfo, kAddSet, kLoadNext, kLearn1,
    kNumParams = kLearn1 + 16
};
// FX pads (bottom row first)
enum { PF_LP, PF_HP, PF_CENSOR, PF_SPIN, PF_GATE2, PF_GATE4, PF_FLANGE, PF_CRUSH,
       PF_ROLL8, PF_ROLL4, PF_ROLL2, PF_ROLL1, PF_ECHO2, PF_ECHO1, PF_VERB, PF_BRAKE };
const char* kPadFxName[16] = { "LP SWEEP", "HP SWEEP", "CENSOR", "BACKSPIN", "GATE 1/2", "GATE 1/4", "FLANGER", "CRUSH",
                               "ROLL 1/8", "ROLL 1/4", "ROLL 1/2", "ROLL 1", "ECHO 1/2", "ECHO 1", "REVERB", "BRAKE" };
constexpr int kMaxTracks = 8192;
inline int dp(int deck, int o) { return (deck ? (int)kBPlay : (int)kAPlay) + o; }
const float kSizeBeats[] = { 0.125f, 0.25f, 0.5f, 1, 2, 4, 8, 16, 32 };
// the notes MPC pads send in a standard MPC program (Pad Bank A and B, pads 1-16)
const int kMpcBankA[16] = { 37, 36, 42, 82, 40, 38, 46, 44, 48, 47, 45, 43, 49, 55, 51, 53 };
const int kMpcBankB[16] = { 54, 69, 81, 80, 65, 66, 76, 77, 56, 62, 63, 64, 73, 74, 71, 39 };
const char* const kSetList = "SET LIST";
const char* kSizeText[] = { "1/8", "1/4", "1/2", "1", "2", "4", "8", "16", "32" };

const float kLoopBeats[] = { 1, 2, 4, 8, 16, 32, 64 };
const float kFxBeatsV[] = { 0.25f, 0.5f, 1, 2, 4 };
const float kRollBeats[] = { 0.25f, 0.5f, 1, 2 };
enum { FX_ECHO, FX_REVERB, FX_FLANGER, FX_ROLL, FX_CRUSH, FX_GATE };
const char* kFxShort[] = { "ECHO", "VERB", "FLANGE", "ROLL", "CRUSH", "GATE" };
constexpr float kPi = 3.14159265359f;

struct TrackInfo
{
    std::string name, file;            // file: inside Content/ (built on the Mac)
    std::string path, rel, crate;      // tracks found on the MPC's drive
    char key[12] = {0};
    float bpm = 120, firstBeat = 0, start = 0, length = 0, gain = 0;
    bool external = false, analyzed = true;
    int folder = -1;
    long fsize = 0, mtime = 0;
};
struct Crate { std::string name; std::vector<int> tracks; };

struct Audio
{
    std::vector<int16_t> data;     // interleaved stereo
    long frames = 0;
    float rate = 44100;
    int track = -1;
    std::vector<float> env;          // peak per 256 frames (for the live waveform)
    float overview[kCols] = {};      // whole-track waveform, 0..1
    void analyse()
    {
        const long nb = frames / 256 + 1;
        env.assign((size_t)nb + 1, 0.0f);
        float pk = 1e-6f;
        for (long b = 0; b < nb; ++b)
        {
            int m = 0;
            const long e = std::min(frames, (b + 1) * 256);
            for (long i = b * 256; i < e; ++i)
            {
                const int v = std::abs((int)data[(size_t)i * 2]) + std::abs((int)data[(size_t)i * 2 + 1]);
                if (v > m) m = v;
            }
            env[(size_t)b] = (float)m / 65536.0f;
            pk = std::max(pk, env[(size_t)b]);
        }
        for (float& v : env) v /= pk;                       // normalise to the track's own peak
        for (int c = 0; c < kCols; ++c)
        {
            const long a = (long)((double)c / kCols * nb), b = std::max(a + 1, (long)((double)(c + 1) / kCols * nb));
            double sum = 0;
            for (long i = a; i < b; ++i) sum += env[(size_t)i] * env[(size_t)i];
            overview[c] = (float)std::sqrt(sum / (double)(b - a));
        }
    }
};
struct Sample { std::string name, file; Audio* audio = nullptr; };

struct Comb
{
    std::vector<float> b; int p = 0; float st = 0;
    void init(int n) { b.assign((size_t)std::max(n, 1), 0.0f); p = 0; st = 0; }
    void clear() { std::fill(b.begin(), b.end(), 0.0f); st = 0; }
    inline float run(float in, float fb, float dmp)
    { float o = b[(size_t)p]; st = o * (1 - dmp) + st * dmp; b[(size_t)p] = in + st * fb; if (++p >= (int)b.size()) p = 0; return o; }
};
struct Allp
{
    std::vector<float> b; int p = 0;
    void init(int n) { b.assign((size_t)std::max(n, 1), 0.0f); p = 0; }
    void clear() { std::fill(b.begin(), b.end(), 0.0f); }
    inline float run(float in) { float v = b[(size_t)p]; float o = -in + v; b[(size_t)p] = in + v * 0.5f; if (++p >= (int)b.size()) p = 0; return o; }
};
struct Reverb
{
    Comb cl[8], cr[8]; Allp al[4], ar[4];
    void prepare(double sr)
    {
        static const int c[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617}, a[4] = {556, 441, 341, 225};
        const double k = sr / 44100.0;
        for (int i = 0; i < 8; ++i) { cl[i].init((int)(c[i] * k)); cr[i].init((int)((c[i] + 23) * k)); }
        for (int i = 0; i < 4; ++i) { al[i].init((int)(a[i] * k)); ar[i].init((int)((a[i] + 23) * k)); }
    }
    void clear() { for (auto& x : cl) x.clear(); for (auto& x : cr) x.clear(); for (auto& x : al) x.clear(); for (auto& x : ar) x.clear(); }
    inline void run(float inL, float inR, float& l, float& r)
    {
        l = r = 0; const float xl = inL * 0.015f, xr = inR * 0.015f;
        for (int i = 0; i < 8; ++i) { l += cl[i].run(xl, 0.86f, 0.3f); r += cr[i].run(xr, 0.86f, 0.3f); }
        for (int i = 0; i < 4; ++i) { l = al[i].run(l); r = ar[i].run(r); }
    }
};

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == sep) { out.push_back(cur); cur.clear(); } else if (c != '\r') cur += c; }
    out.push_back(cur);
    return out;
}

// Minimal 16-bit PCM WAV reader (the builder writes exactly this format).
Audio* readWav16(const std::string& path)
{
    std::FILE* f = mdlib::openFile(path, "rb");
    if (!f) return nullptr;
    auto u32 = [](const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); };
    uint8_t h[12];
    if (std::fread(h, 1, 12, f) != 12 || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4)) { std::fclose(f); return nullptr; }
    int ch = 0, bits = 0; uint32_t rate = 44100;
    Audio* a = nullptr;
    uint8_t c[8];
    while (std::fread(c, 1, 8, f) == 8)
    {
        const uint32_t size = u32(c + 4);
        if (!std::memcmp(c, "fmt ", 4))
        {
            uint8_t b[16];
            if (size < 16 || std::fread(b, 1, 16, f) != 16) break;
            ch = b[2] | (b[3] << 8); rate = u32(b + 4); bits = b[14] | (b[15] << 8);
            std::fseek(f, (long)(size - 16 + (size & 1)), SEEK_CUR);
        }
        else if (!std::memcmp(c, "data", 4))
        {
            if (bits != 16 || ch < 1 || ch > 2) break;
            const long frames = (long)(size / (uint32_t)(2 * ch));
            a = new Audio();
            a->frames = frames; a->rate = (float)rate;
            a->data.assign((size_t)(frames + 8) * 2, 0);
            if (ch == 2)
                a->frames = (long)(std::fread(a->data.data(), 4, (size_t)frames, f));
            else
            {
                std::vector<int16_t> m((size_t)frames);
                a->frames = (long)std::fread(m.data(), 2, (size_t)frames, f);
                for (long i = 0; i < a->frames; ++i) a->data[(size_t)i * 2] = a->data[(size_t)i * 2 + 1] = m[(size_t)i];
            }
            break;
        }
        else std::fseek(f, (long)(size + (size & 1)), SEEK_CUR);
    }
    std::fclose(f);
    if (a && a->frames <= 0) { delete a; a = nullptr; }
    return a;
}

struct Deck
{
    // loading (loader thread <-> audio thread)
    std::atomic<Audio*> pending{nullptr}, retired{nullptr};
    std::atomic<int> wantTrack{-1}, loadedTrack{-1};
    Audio* live = nullptr;
    int seenTrack = -2; float settle = 0;

    // transport
    double pos = 0, cuePos = 0;
    bool playing = false, cuePreview = false, synced = false, looping = false, braking = false;
    enum Src { MPC, OTHER, HOLD } src = MPC;
    float holdBpm = 120, brakeAmt = 1, jogVel = 0, rateNow = 1, bpmNow = 0;
    // scratch: the play head follows the platter while it is being turned
    bool scratching = false; double scratchTarget = 0; float scratchHold = 0, scratchVel = 0;
    int nudge = 0;
    double loopStart = 0, loopEnd = 0;
    float jumpBeats = 16;
    double hot[8];
    float amp = 0;

    // declick crossfade after a jump
    double xfPos = 0; int xfLeft = 0;

    // loop roll: a second play head looping a slice while pos keeps running
    bool rollPad = false, rollFx = false; float rollBeats = 1, rollMix = 0;
    double rollStart = 0, rollLen = 0, rollPos = 0;
    int rollNote = -1;

    // beat FX
    mpc::dsp::DelayLine echo[2], flange[2];
    Reverb verb;
    float fxSend = 0, fxAmt = 0, flPh = 0, crushHold[2] = { 0, 0 }, crushCnt = 0, gateG = 1, trim = 1;

    // tone
    mpc::dsp::SVF lo[2], hi[2], flt[2];
    float gLo = 1, gMid = 1, gHi = 1, level = 1, xg = 1;

    // FX pads: a held effect (with tail), filter sweep, reverse head (censor / backspin)
    int pfx = -1, pfxPad = -1; bool pfxHeld = false; float pfxAmt = 0.7f, pfxBeats = 1, pfxTail = 0;
    int sweepDir = 0; bool sweepHeld = false; float sweep = 0, sweepAmt = 0.8f;
    bool revOn = false, spin = false; float revMix = 0, revSpeed = 1; double revPos = 0;

    // live screen
    float nudgePulse = 0; int nudgePulseDir = 0;
    float meterPeak = 0;
    double loopInPos = -1;
    int infoCount = 0; long infoSig = -1; int statusFlip = 0; bool wasOn = false;

    // status for the screen
    std::atomic<int> stFlags{0};
    std::atomic<float> stBpm{0}, stLoop{0};

    Deck() { for (double& h : hot) h = -1; }
    const TrackInfo* info(const std::vector<TrackInfo>& t) const
    {
        return live && live->track >= 0 ? &t[(size_t)live->track] : nullptr;     // reserved vector: safe to index
    }
};

class MeltDeck : public mpc::Plugin
{
public:
    MeltDeck() : mpc::Plugin(makeInfo())
    {
        tracks_.reserve(kMaxTracks);
        claim_.reset(new std::atomic<uint8_t>[kMaxTracks]);
        for (int i = 0; i < kMaxTracks; ++i) claim_[i].store(0);
        loadIndex();
        buildView();
        const int maxN = 4000;          // crates can grow while running (songs from the MPC drive)

        addParam(mpc::floatParam("volume", "Volume", -48.0f, 6.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("xfade", "Crossfader", -100.0f, 100.0f, 0.0f, "", 0));
        addParam(mpc::intParam("atrack", "A Track", 1, maxN, 1));
        addParam(mpc::intParam("btrack", "B Track", 1, maxN, std::min(2, maxN)));
        addParam(mpc::floatParam("alevel", "A Level", 0.0f, 100.0f, 85.0f, "%", 0));
        addParam(mpc::floatParam("blevel", "B Level", 0.0f, 100.0f, 85.0f, "%", 0));
        addParam(mpc::floatParam("atempo", "A Tempo", -16.0f, 16.0f, 0.0f, "%", 2));
        addParam(mpc::floatParam("btempo", "B Tempo", -16.0f, 16.0f, 0.0f, "%", 2));
        addParam(mpc::floatParam("afilter", "A Filter", -100.0f, 100.0f, 0.0f, "", 0));
        addParam(mpc::floatParam("bfilter", "B Filter", -100.0f, 100.0f, 0.0f, "", 0));
        for (const char* n : { "A Low", "A Mid", "A High", "B Low", "B Mid", "B High" })
        {
            std::string id = n; id.erase(std::remove(id.begin(), id.end(), ' '), id.end());
            for (char& ch : id) ch = (char)std::tolower((unsigned char)ch);
            addParam(mpc::floatParam(id.c_str(), n, -30.0f, 6.0f, 0.0f, "dB", 1));
        }
        addParam(mpc::choiceParam("mode", "Mode", { "1 Deck", "2 Decks" }, 1));
        addParam(mpc::choiceParam("deck", "Deck", { "Deck A", "Deck B" }, 0));
        addParam(mpc::boolParam("play", "Play", false));
        addParam(mpc::boolParam("cue", "Cue", false));
        addParam(mpc::boolParam("sync", "Sync", false));
        addParam(mpc::boolParam("loop", "Loop", false));
        addParam(mpc::choiceParam("loopsize", "Loop Size", { "1 Beat", "2 Beats", "1 Bar", "2 Bars", "4 Bars", "8 Bars", "16 Bars" }, 4));
        addParam(mpc::boolParam("jumpback", "Jump Back", false));
        addParam(mpc::boolParam("jumpfwd", "Jump Fwd", false));
        addParam(mpc::boolParam("brake", "Brake", false));
        addParam(mpc::floatParam("jog", "Jog", -1000.0f, 1000.0f, 0.0f, "", 0));
        addParam(mpc::intParam("acrate", "A Crate", 0, 63, 0));
        addParam(mpc::intParam("bcrate", "B Crate", 0, 63, 0));
        addParam(mpc::intParam("astatus", "A Status", 0, 1, 0));
        addParam(mpc::intParam("bstatus", "B Status", 0, 1, 0));
        addParam(mpc::boolParam("acue", "A Cue", false));
        addParam(mpc::boolParam("bcue", "B Cue", false));
        addParam(mpc::floatParam("cuemix", "Cue Mix", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::intParam("padstart", "Pad Start", 0, 112, 36));
        addParam(mpc::boolParam("hot1", "Hot Cue 1", false));
        addParam(mpc::boolParam("hot2", "Hot Cue 2", false));
        addParam(mpc::boolParam("hot3", "Hot Cue 3", false));
        addParam(mpc::boolParam("hot4", "Hot Cue 4", false));
        addParam(mpc::choiceParam("fxtype", "FX Type", { "Echo", "Reverb", "Flanger", "Roll", "Crush", "Gate" }, 0));
        addParam(mpc::choiceParam("fxbeats", "FX Beats", { "1/4", "1/2", "1", "2", "4" }, 2));
        addParam(mpc::floatParam("fxamount", "FX Amount", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::boolParam("afx", "A FX", false));
        addParam(mpc::boolParam("bfx", "B FX", false));
        addParam(mpc::boolParam("autogain", "Auto Gain", true));
        addParam(mpc::choiceParam("sort", "Sort", { "Name", "BPM", "Key" }, 0));
        for (int d = 0; d < 2; ++d)
        {
            const std::string L = d ? "B" : "A", l = d ? "b" : "a";
            auto id = [&](const char* x) { return l + "_" + x; };
            auto nm = [&](const char* x) { return L + " " + x; };
            addParam(mpc::boolParam(id("play").c_str(), nm("Play").c_str(), false));
            addParam(mpc::boolParam(id("cue").c_str(), nm("Cue Btn").c_str(), false));
            addParam(mpc::boolParam(id("sync").c_str(), nm("Sync").c_str(), false));
            addParam(mpc::boolParam(id("loop").c_str(), nm("Loop").c_str(), false));
            addParam(mpc::boolParam(id("loophalf").c_str(), nm("Loop /2").c_str(), false));
            addParam(mpc::boolParam(id("loopdouble").c_str(), nm("Loop x2").c_str(), false));
            addParam(mpc::boolParam(id("loopin").c_str(), nm("Loop In").c_str(), false));
            addParam(mpc::boolParam(id("loopout").c_str(), nm("Loop Out").c_str(), false));
            addParam(mpc::boolParam(id("hot1").c_str(), nm("Hot 1").c_str(), false));
            addParam(mpc::boolParam(id("hot2").c_str(), nm("Hot 2").c_str(), false));
            addParam(mpc::boolParam(id("hot3").c_str(), nm("Hot 3").c_str(), false));
            addParam(mpc::floatParam(id("gain").c_str(), nm("Gain").c_str(), -12.0f, 12.0f, 0.0f, "dB", 1));
            addParam(mpc::floatParam(id("jog").c_str(), nm("Jog").c_str(), 0.0f, 1.0f, 0.0f, "", 2));
            addParam(mpc::floatParam(id("bpm").c_str(), nm("BPM").c_str(), 0.0f, 300.0f, 0.0f, "", 2));
            addParam(mpc::floatParam(id("time").c_str(), nm("Time").c_str(), 0.0f, 1.0f, 0.0f, "", 3));
            addParam(mpc::intParam(id("info").c_str(), nm("Info").c_str(), 0, 1000, 0));
            addParam(mpc::floatParam(id("meter").c_str(), nm("Meter").c_str(), 0.0f, 1.0f, 0.0f, "", 2));
            addParam(mpc::intParam(id("loopsize").c_str(), nm("Loop Size").c_str(), 0, 8, 5));
        }
        addParam(mpc::floatParam("phase", "Phase", -0.5f, 0.5f, 0.0f, "", 2));
        addParam(mpc::boolParam("shift", "Shift", false));
        addParam(mpc::boolParam("bpmup", "BPM +", false));
        addParam(mpc::boolParam("bpmdown", "BPM -", false));
        addParam(mpc::boolParam("tempreset", "Tempo Reset", false));
        addParam(mpc::boolParam("browse", "Browse", false));
        addParam(mpc::boolParam("loop2", "Loop 2 Bars", false));
        addParam(mpc::boolParam("loop4", "Loop 4 Bars", false));
        addParam(mpc::boolParam("loop8", "Loop 8 Bars", false));
        addParam(mpc::boolParam("nudgedown", "Nudge -", false));
        addParam(mpc::boolParam("nudgeup", "Nudge +", false));
        std::vector<std::string> sn;
        for (auto& x : samples_) sn.push_back(x.name);
        if (sn.empty()) sn.push_back("No Samples");
        if (sn.size() < 2) sn.push_back("-");
        addParam(mpc::choiceParam("sampler", "Sampler", sn, 0));
        addParam(mpc::floatParam("samplervol", "Sampler Vol", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(mpc::boolParam("samplerplay", "Sampler Play", false));
        addParam(mpc::choiceParam("liveview", "Live View", { "Off", "Low", "High" }, 2));
        for (const char* g : { "azoom", "bzoom", "awave", "bwave" })
            for (int c = 0; c < kCols; ++c)
            {
                char idb[32], nmb[32];
                std::snprintf(idb, sizeof idb, "%s%d", g, c + 1);
                std::snprintf(nmb, sizeof nmb, "%c %s %d", g[0] == 'a' ? 'A' : 'B', g[1] == 'z' ? "Zoom" : "Wave", c + 1);
                addParam(mpc::floatParam(idb, nmb, 0.0f, 1.0f, 0.0f, "", 2));
            }
        addParam(mpc::boolParam("rescan", "Rescan Drive", false));
        addParam(mpc::intParam("libinfo", "Library", 0, 1000, 0));
        addParam(mpc::choiceParam("padmode", "Pad Mode", { "Deck", "FX" }, 0));
        for (int k = 0; k < 16; ++k)
        {
            char idb[16], nmb[32];
            std::snprintf(idb, sizeof idb, "fxpad%d", k + 1);
            std::snprintf(nmb, sizeof nmb, "FX Pad %d", k + 1);
            addParam(mpc::boolParam(idb, nmb, false));
        }
        addParam(mpc::intParam("bsel", "Browse Pos", 0, 9999, 0));
        addParam(mpc::intParam("brow", "Browse Row", 0, 7, 0));
        addParam(mpc::intParam("bpath", "Folder", 0, 1000, 0));
        for (int k = 0; k < 8; ++k)
        {
            char idb[16], nmb[24];
            std::snprintf(idb, sizeof idb, "bline%d", k + 1);
            std::snprintf(nmb, sizeof nmb, "Browse Line %d", k + 1);
            addParam(mpc::intParam(idb, nmb, 0, 1000, 0));
        }
        for (const char* n : { "Open", "Back", "Load A", "Load B", "Add Folder", "Page Up", "Page Down", "Drives" })
        {
            std::string id = "b"; for (const char* c = n; *c; ++c) if (*c != ' ') id += (char)std::tolower((unsigned char)*c);
            addParam(mpc::boolParam(id.c_str(), n, false));
        }
        addParam(mpc::choiceParam("jogmode", "Jog Mode", { "Nudge", "Scratch" }, 1));
        addParam(mpc::choiceParam("padmap", "Pad Map", { "MPC Pads", "Chromatic", "Learned" }, 0));
        addParam(mpc::boolParam("padlearn", "Pad Learn", false));
        addParam(mpc::intParam("padinfo", "Pad Info", 0, 1000, 0));
        addParam(mpc::boolParam("addset", "Add To Set", false));
        addParam(mpc::boolParam("loadnext", "Load Next", false));
        for (int k = 0; k < 16; ++k)
        {
            char id[16], nm[24];
            std::snprintf(id, sizeof id, "learn%d", k + 1); std::snprintf(nm, sizeof nm, "Pad %d Note", k + 1);
            addParam(mpc::intParam(id, nm, 0, 127, kMpcBankA[k]));
        }
        notify_.reset(new std::atomic<uint8_t>[kNumParams]);
        for (int i = 0; i < kNumParams; ++i) notify_[i].store(0);
        lastSeen_.assign(kNumParams, 0.0f);
    }
    ~MeltDeck() override
    {
        stopLoader();
        for (Deck& d : deck_) { delete d.pending.exchange(nullptr); delete d.retired.exchange(nullptr); delete d.live; }
        for (Sample& x : samples_) delete x.audio;
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = MD_NAME; i.vendor = MD_VENDOR;
        i.uniqueId = MPC_FOURCC(MD_UID_A, MD_UID_B, MD_UID_C, MD_UID_D);
        i.version = 1000; i.isInstrument = true; i.numInputs = 0; i.numOutputs = 4;
        return i;
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[160];
        switch (index)
        {
            case kATrack: case kBTrack:
            {
                const int d = index == kATrack ? 0 : 1;
                const int t = trackAt(getInt(d ? kBCrate : kACrate), (int)std::lround(plain));
                if (t < 0) { out = "-"; return true; }
                out = tracks_[(size_t)t].name; return true;
            }
            case kACrate: case kBCrate:
            {
                const View* v = view();
                if (!v || v->crates.empty()) { out = scanning_.load() ? "Looking..." : "No Tracks"; return true; }
                const Crate& c = v->crates[(size_t)mpc::clamp((int)std::lround(plain), 0, (int)v->crates.size() - 1)];
                out = c.name + " (" + std::to_string(c.tracks.size()) + ")";
                return true;
            }
            case kLibInfo: out = libraryText(); return true;
            case kPadInfo: out = padInfoText(); return true;
            case kPadLearn: case kAddSet: case kLoadNext: out = "TAP"; return true;
            case kRescan: out = "TAP"; return true;
            case kATempo: case kBTempo:
            {
                std::snprintf(b, sizeof b, "%+.2f%%", plain); out = b; return true;
            }
            case kAFilter: case kBFilter:
                if (std::fabs(plain) < 3) { out = "OFF"; return true; }
                std::snprintf(b, sizeof b, "%s %d", plain < 0 ? "LP" : "HP", (int)std::lround(std::fabs(plain))); out = b; return true;
            case kALow: case kAMid: case kAHigh: case kBLow: case kBMid: case kBHigh:
                if (plain <= -29.5f) { out = "KILL"; return true; }
                std::snprintf(b, sizeof b, "%+.1f dB", plain); out = b; return true;
            case kXfade:
                if (std::fabs(plain) < 2) { out = "CENTER"; return true; }
                std::snprintf(b, sizeof b, "%s %d", plain < 0 ? "A" : "B", (int)std::lround(std::fabs(plain))); out = b; return true;
            case kJog: out = "JOG"; return true;
            case kAStatus: case kBStatus: out = status(index == kAStatus ? 0 : 1); return true;
            case kPadStart:
            {
                static const char* nn[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
                const int n = (int)std::lround(plain);
                std::snprintf(b, sizeof b, "%s%d", nn[n % 12], n / 12 - 2); out = b; return true;
            }
            case kPlay: case kCue: case kSync: case kLoop: case kJumpBack: case kJumpFwd: case kBrake:
            case kHot1: case kHot2: case kHot3: case kHot4:
                out = "TAP"; return true;
            case kPhase: out = "PHASE"; return true;
            default: break;
        }
        if (index >= kFxPad1 && index < kFxPad1 + 16) { out = kPadFxName[index - kFxPad1]; return true; }
        if (index >= kBrowseLine1 && index < kBrowseLine1 + 8) { out = browseLineText(index - kBrowseLine1); return true; }
        if (index == kBrowsePath) { out = browsePathText(); return true; }
        if (index == kBrowseSel || index == kBrowseRow) { out = ""; return true; }
        if (index >= kBrowseOpen && index <= kBrowseDrives) { out = "TAP"; return true; }
        if (index >= kAZoom && index < kRescan) { out = ""; return true; }
        if (index >= kAPlay && index < kAPlay + 2 * kDeckParams)
        {
            const int d = index >= kBPlay ? 1 : 0, o = index - dp(d, 0);
            const Deck& k = deck_[d];
            const TrackInfo* t = trackPtr(k.loadedTrack.load());
            switch (o)
            {
                case oGain: std::snprintf(b, sizeof b, "%+.1f dB", plain); out = b; return true;
                case oBpm: if (plain < 1 || !t) out = "--.--"; else { std::snprintf(b, sizeof b, "%.2f", plain); out = b; } return true;
                case oTime:
                {
                    if (!t) { out = "NO TRACK LOADED"; return true; }
                    const double el = plain * t->length, re = std::max(0.0, (double)t->length - el);
                    std::snprintf(b, sizeof b, "ELAPSED %02d:%04.1f  REMAIN %02d:%04.1f", (int)(el / 60), std::fmod(el, 60.0), (int)(re / 60), std::fmod(re, 60.0));
                    out = b; return true;
                }
                case oInfo:
                {
                    if (!t) { out = ""; return true; }
                    std::string key(t->key); key = key.substr(0, key.find(' '));
                    const float g = (getBool(kAutoGain) ? t->gain : 0.0f) + get(dp(d, oGain));
                    const float pitch = t->bpm > 0 && k.bpmNow > 0 ? (k.bpmNow / t->bpm - 1.0f) * 100.0f : 0.0f;
                    std::snprintf(b, sizeof b, "KEY %s GAIN %+.1fdB PITCH %+.1f", key.empty() ? "-" : key.c_str(), g, pitch);
                    out = b; return true;
                }
                case oLoopSize: out = kSizeText[mpc::clamp((int)std::lround(plain), 0, 8)]; return true;
                case oJog: case oMeter: out = ""; return true;
                default: out = plain >= 0.5f ? "On" : "Off"; return true;
            }
        }
        return false;
    }

    void prepare(double sr, int) override
    {
        sr_ = (float)sr;
        for (Deck& d : deck_)
        {
            for (int c = 0; c < 2; ++c)
            {
                d.lo[c].setSampleRate(sr); d.hi[c].setSampleRate(sr); d.flt[c].setSampleRate(sr);
                d.echo[c].prepare((int)(sr * 6.5)); d.flange[c].prepare((int)(sr * 0.02));
            }
            d.verb.prepare(sr);
        }
        syncTriggers();
        startLoader();
    }

    void reset() override
    {
        for (Deck& d : deck_)
        {
            for (int c = 0; c < 2; ++c) { d.lo[c].reset(); d.hi[c].reset(); d.flt[c].reset(); d.echo[c].reset(); d.flange[c].reset(); }
            d.verb.clear(); d.rollPad = d.rollFx = false; d.rollMix = 0;
        }
        // a restored state: take over the saved deck choice and loop sizes
        focus_ = getInt(kMode) == 1 ? mpc::clamp(getInt(kDeck), 0, 1) : 0;
        for (int i = 0; i < 2; ++i) deck_[i].jumpBeats = kSizeBeats[mpc::clamp(getInt(dp(i, oLoopSize)), 0, 8)];
        syncTriggers();
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        const mpc::Transport& tr = ctx.transport;
        mpcBpm_ = (tr.valid && tr.tempoValid && tr.bpm > 20) ? (float)tr.bpm : 120.0f;
        mpcRunning_ = tr.valid && tr.playing && tr.ppqValid;
        ppq_ = tr.ppqPos;
        twoDecks_ = getInt(kMode) == 1;
        if (!twoDecks_) focus_ = 0;

        readTriggers(n);
        applySelection();
        for (int d = 0; d < 2; ++d) { takePending(d); browse(d, n); }

        int ev = 0;
        for (int f = 0; f < n;)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= f) midi(ctx.midi[ev++]);
            int end = std::min(n, f + 32);
            if (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset > f && ctx.midi[ev].sampleOffset < end) end = ctx.midi[ev].sampleOffset;
            render(ctx, f, end - f);
            f = end;
        }
        while (ev < ctx.numMidiEvents) midi(ctx.midi[ev++]);
        publish(n);
    }

private:
    // ------------------------------------------------------------- library
    void loadIndex()
    {
        std::string text;
        if (!mpc::readTextFile(mpc::contentPath("index.txt"), text)) return;
        for (const std::string& line : split(text, '\n'))
        {
            const auto f = split(line, '\t');
            if (f.size() >= 2 && f[0] == "CRATE") builtCrates_.push_back({ f[1], {} });
            else if (f.size() >= 8 && f[0] == "TRACK" && (int)tracks_.size() < kMaxTracks)
            {
                TrackInfo t;
                t.name = f[1]; t.file = f[2];
                t.bpm = (float)std::atof(f[3].c_str()); t.firstBeat = (float)std::atof(f[4].c_str());
                std::snprintf(t.key, sizeof t.key, "%s", f[5].c_str());
                t.start = (float)std::atof(f[6].c_str()); t.length = (float)std::atof(f[7].c_str());
                if (f.size() >= 9) t.gain = mpc::clamp((float)std::atof(f[8].c_str()), -18.0f, 18.0f);
                if (t.bpm < 40 || t.bpm > 250) t.bpm = 120;
                tracks_.push_back(t);
            }
            else if (f.size() >= 3 && f[0] == "SAMPLE") samples_.push_back({ f[1], f[2], nullptr });
            else if (f.size() >= 2 && f[0] == "IN" && !builtCrates_.empty())
            {
                const int i = std::atoi(f[1].c_str());
                if (i >= 0 && i < (int)tracks_.size()) builtCrates_.back().tracks.push_back(i);
            }
        }
        nTracks_.store((int)tracks_.size(), std::memory_order_release);
    }
    int nTracks() const { return nTracks_.load(std::memory_order_acquire); }
    const TrackInfo* trackPtr(int i) const { return i >= 0 && i < nTracks() ? &tracks_[(size_t)i] : nullptr; }

    // The crate lists the screen shows: Mac-built crates first, then "MPC Drive" and one
    // crate per sub-folder of "Melt Deck Music". Each in three orders (name, BPM, key).
    // A new view is published whole; old ones stay alive until the plugin closes.
    struct View { std::vector<Crate> crates; std::vector<std::array<std::vector<int>, 3>> order; };
    void buildView()
    {
        const int n = nTracks();
        std::unique_ptr<View> v(new View());
        v->crates = builtCrates_;
        std::vector<int> drive;
        std::vector<std::string> names;
        for (int i = 0; i < n; ++i)
            if (tracks_[(size_t)i].external && tracks_[(size_t)i].folder >= 0 && !(tracks_[(size_t)i].analyzed && tracks_[(size_t)i].length <= 0))   // skip unreadable files
            {
                drive.push_back(i);
                const std::string& c = tracks_[(size_t)i].crate;
                if (!c.empty() && std::find(names.begin(), names.end(), c) == names.end()) names.push_back(c);
            }
        auto byName = [this](int a, int b) { return mdlib::lower(tracks_[(size_t)a].name) < mdlib::lower(tracks_[(size_t)b].name); };
        if (!drive.empty())
        {
            std::stable_sort(drive.begin(), drive.end(), byName);
#if defined(MPC_DESKTOP)
            v->crates.push_back({ "My Music", drive });
#else
            v->crates.push_back({ "MPC Drive", drive });
#endif
            std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return mdlib::lower(a) < mdlib::lower(b); });
            for (const std::string& c : names)
            {
                Crate cr{ c, {} };
                for (int i : drive) if (tracks_[(size_t)i].crate == c) cr.tracks.push_back(i);
                v->crates.push_back(cr);
            }
        }
        if (!browsed_.empty()) v->crates.push_back({ "Browsed Songs", browsed_ });
        for (const auto& c : addedCrates_) v->crates.push_back(c);
        if (v->crates.size() > 64) v->crates.resize(64);
        auto camelot = [this](int t) {
            const TrackInfo& x = tracks_[(size_t)t];
            if (!x.analyzed || !x.key[0]) return 999;
            const char* sp = std::strrchr(x.key, ' ');
            if (!sp) return 999;
            const size_t L = std::strlen(x.key);
            return std::atoi(sp + 1) * 2 + (x.key[L - 1] == 'B' ? 1 : 0);
        };
        for (auto& c : v->crates)
        {
            std::vector<int> byBpm = c.tracks, byKey = c.tracks;
            std::stable_sort(byBpm.begin(), byBpm.end(), [this](int a, int b) {
                const TrackInfo &x = tracks_[(size_t)a], &y = tracks_[(size_t)b];
                return (x.analyzed ? x.bpm : 999.0f) < (y.analyzed ? y.bpm : 999.0f); });
            std::stable_sort(byKey.begin(), byKey.end(), [&](int a, int b) { return camelot(a) < camelot(b); });
            if (c.name == kSetList) v->order.push_back({ c.tracks, c.tracks, c.tracks });     // a set keeps your order
            else v->order.push_back({ c.tracks, byBpm, byKey });
        }
        view_.store(v.get(), std::memory_order_release);
        views_.push_back(std::move(v));
    }
    const View* view() const { return view_.load(std::memory_order_acquire); }
    int crateCount() const { const View* v = view(); return v ? (int)v->crates.size() : 0; }
    int crateSize(int c) const
    {
        const View* v = view();
        if (!v || v->crates.empty()) return 0;
        return (int)v->crates[(size_t)mpc::clamp(c, 0, (int)v->crates.size() - 1)].tracks.size();
    }
    int trackAt(int crate, int n) const
    {
        const View* v = view();
        if (!v || v->crates.empty()) return -1;
        const int ci = mpc::clamp(crate, 0, (int)v->crates.size() - 1);
        const auto& o = v->order[(size_t)ci][(size_t)mpc::clamp(getInt(kSort), 0, 2)];
        if (n < 1 || n > (int)o.size()) return -1;
        return o[(size_t)(n - 1)];
    }

    // ------------------------------------------------------------- the MPC drive
    void scanDrive()
    {
        scanning_.store(true);
        const std::vector<std::string> folders = mdlib::findMusicFolders(mpc::pluginFolder());
        for (const std::string& folder : folders)
        {
            int fi = -1;
            for (size_t k = 0; k < folders_.size(); ++k) if (folders_[k] == folder) fi = (int)k;
            if (fi < 0)
            {
                std::lock_guard<std::mutex> lk(infoMu_);
                folders_.push_back(folder); fi = (int)folders_.size() - 1;
                caches_.push_back(mdlib::loadCache(folder));
            }
            std::vector<mdlib::FileEntry> files;
            mdlib::listAudio(folder, "", 0, files);
            for (const mdlib::FileEntry& e : files)
            {
                if ((int)tracks_.size() >= kMaxTracks) break;
                if (knownPaths_.count(e.path)) continue;
                knownPaths_.insert(e.path);
                TrackInfo t;
                t.name = mdlib::cleanName(e.stem);
                t.path = e.path; t.rel = e.rel; t.crate = e.crate.empty() ? std::string() : mdlib::cleanName(e.crate);
                t.external = true; t.analyzed = false; t.folder = fi; t.fsize = e.size; t.mtime = e.mtime;
                auto it = caches_[(size_t)fi].find(e.rel);
                if (it != caches_[(size_t)fi].end() && it->second.size == e.size && it->second.mtime == e.mtime)
                {
                    const mdlib::Analysis& a = it->second.a;
                    t.bpm = a.bpm; t.firstBeat = a.first; t.start = a.start; t.length = a.length; t.gain = a.gain;
                    std::snprintf(t.key, sizeof t.key, "%s", a.key);
                    t.analyzed = true;
                }
                tracks_.push_back(t);
                knownIndex_[e.path] = (int)tracks_.size() - 1;
                nTracks_.store((int)tracks_.size(), std::memory_order_release);
            }
        }
        foldersFound_.store((int)folders_.size());
        scanning_.store(false);
        buildView();
    }
    void publishList(mdlib::BrowseList* L)
    {
        blists_.push_back(std::unique_ptr<mdlib::BrowseList>(L));       // kept alive: other threads may still read the old one
        blist_.store(L);
        listSerial_.fetch_add(1);
    }
    // loader thread: add one file as a track (or find it), returns its index
    int addFile(const std::string& path)
    {
        auto it = knownIndex_.find(path);
        if (it != knownIndex_.end()) return it->second;
        if ((int)tracks_.size() >= kMaxTracks) return -1;
        if (!browseCacheLoaded_) { browseCache_ = mdlib::loadCache(mpc::contentFolder()); browseCacheLoaded_ = true; }
        const mdlib::FileStat st = mdlib::statPath(path);
        if (!st.ok || st.dir) return -1;
        TrackInfo t;
        const size_t sl = path.find_last_of('/'), dot = path.find_last_of('.');
        t.name = mdlib::cleanName(path.substr(sl + 1, (dot != std::string::npos && dot > sl ? dot : path.size()) - sl - 1));
        t.path = path; t.rel = path; t.external = true; t.analyzed = false; t.folder = -2;
        t.fsize = st.size; t.mtime = st.mtime;
        auto c = browseCache_.find(path);
        if (c != browseCache_.end() && c->second.size == t.fsize && c->second.mtime == t.mtime)
        {
            const mdlib::Analysis& a = c->second.a;
            t.bpm = a.bpm; t.firstBeat = a.first; t.start = a.start; t.length = a.length; t.gain = a.gain;
            std::snprintf(t.key, sizeof t.key, "%s", a.key);
            t.analyzed = true;
        }
        tracks_.push_back(t);
        knownPaths_.insert(path);
        const int idx = (int)tracks_.size() - 1;
        knownIndex_[path] = idx;
        nTracks_.store((int)tracks_.size(), std::memory_order_release);
        return idx;
    }
    void handleAdd(int type)
    {
        const mdlib::BrowseList* L = bAddList_.load();
        const int idx = bAddIdx_.load(), deck = bAddDeck_.load();
        if (!L) return;
        if (type == 1 && idx >= 0 && idx < (int)L->items.size())
        {
            const int t = addFile(L->items[(size_t)idx].path);
            if (t < 0) return;
            if (std::find(browsed_.begin(), browsed_.end(), t) == browsed_.end()) browsed_.push_back(t);
            buildView();
            selCrate_ = "Browsed Songs"; selTrack_.store(t); selDeck_.store(deck);
        }
        else if (type == 3 && idx >= 0 && idx < (int)L->items.size())
        {   // ADD TO SET: append one song to the SET LIST crate, in the order you add them
            const int t = addFile(L->items[(size_t)idx].path);
            if (t < 0) { say("CAN'T READ THAT SONG"); return; }
            Crate* set = nullptr;
            for (auto& c : addedCrates_) if (c.name == kSetList) set = &c;
            if (!set) { addedCrates_.insert(addedCrates_.begin(), Crate{ kSetList, {} }); set = &addedCrates_.front(); }
            if (std::find(set->tracks.begin(), set->tracks.end(), t) != set->tracks.end()) { say("ALREADY IN THE SET"); return; }
            set->tracks.push_back(t);
            buildView();
            char b[64]; std::snprintf(b, sizeof b, "ADDED TO SET  (%d SONGS)", (int)set->tracks.size());
            say(b);
        }
        else if (type == 2)
        {
            std::vector<mdlib::FileEntry> files;
            mdlib::listAudio(L->path, "", 2, files);              // this folder and two levels below
            Crate cr{ mdlib::cleanName(L->title), {} };
            for (const auto& e : files) { const int t = addFile(e.path); if (t >= 0) cr.tracks.push_back(t); }
            if (cr.tracks.empty()) return;
            std::stable_sort(cr.tracks.begin(), cr.tracks.end(), [this](int a, int b) { return mdlib::lower(tracks_[(size_t)a].name) < mdlib::lower(tracks_[(size_t)b].name); });
            bool replaced = false;
            for (auto& c : addedCrates_) if (c.name == cr.name) { c = cr; replaced = true; }
            if (!replaced) addedCrates_.push_back(cr);
            buildView();
            selCrate_ = cr.name; selTrack_.store(-1); selDeck_.store(deck);
        }
    }

    // decode a track for playing: from Content/ (Mac-built) or from the drive
    Audio* openTrack(int idx)
    {
        TrackInfo& t = tracks_[(size_t)idx];
        if (!t.external) return readWav16(mpc::contentPath(t.file));
        mdlib::Decoded dec;
        if (!mdlib::decodeFile(t.path, dec)) return nullptr;
        if (!t.analyzed)
        {
            uint8_t expect = 0;
            if (claim_[idx].compare_exchange_strong(expect, 1)) { analyseTrack(t, dec); claim_[idx].store(2); }
            else for (int k = 0; k < 1000 && !t.analyzed && running_.load(); ++k)        // the analyser is on it: wait for it
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        Audio* a = new Audio();
        a->data.swap(dec.data); a->frames = dec.frames; a->rate = dec.rate;
        return a;
    }
    void analyseTrack(TrackInfo& t, const mdlib::Decoded& dec)
    {
        mdlib::Analysis an;
        mdlib::analyse(dec.data.data(), dec.frames, dec.rate, t.name, an);
        t.bpm = an.bpm; t.firstBeat = an.first; t.start = an.start; t.length = an.length; t.gain = an.gain;
        std::snprintf(t.key, sizeof t.key, "%s", an.key);
        t.analyzed = true;
        {
            std::lock_guard<std::mutex> lk(cacheMu_);
            if (t.folder >= 0) mdlib::appendCache(folders_[(size_t)t.folder], t.rel, t.fsize, t.mtime, an);
            else if (t.folder == -2) mdlib::appendCache(mpc::contentFolder(), t.path, t.fsize, t.mtime, an);
        }
        sinceView_.fetch_add(1);
    }
    // one not-yet-analysed drive track, in the background
    bool analyseNext()
    {
        const int n = nTracks();
        for (int k = 0; k < n; ++k)
        {
            const int i = (bgCursor_ + k) % n;
            TrackInfo& t = tracks_[(size_t)i];
            if (!t.external || t.analyzed) continue;
            uint8_t expect = 0;
            if (!claim_[i].compare_exchange_strong(expect, 1)) continue;
            bgCursor_ = i + 1;
            mdlib::Decoded dec;
            if (mdlib::decodeFile(t.path, dec)) analyseTrack(t, dec);
            else { t.length = 0; t.analyzed = true; sinceView_.fetch_add(1); }   // unreadable: stop retrying
            claim_[i].store(2);
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------- loading
    void startLoader()
    {
        if (running_.exchange(true)) return;
        notifier_ = std::thread([this] {
            while (running_.load())
            {
                for (int i = 0; i < kNumParams; ++i)
                    if (notify_[i].load(std::memory_order_relaxed) && notify_[i].exchange(0)) notifyHostParameterChanged(i);
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        });
        browser_ = std::thread([this] {
            publishList(new mdlib::BrowseList(mdlib::listDrives(mpc::pluginFolder())));
            while (running_.load())
            {
                if (const int t = bReq_.load())
                {
                    const mdlib::BrowseList* L = bReqList_.load();
                    const int idx = bReqIdx_.load();
                    if (t == 1 || !L) publishList(new mdlib::BrowseList(mdlib::listDrives(mpc::pluginFolder())));
                    else if (t == 2 && idx >= 0 && idx < (int)L->items.size())
                    {
                        const std::string& ip = L->items[(size_t)idx].path;
                        const size_t pl = std::strlen(mdlib::kAllPrefix);
                        if (ip.rfind(mdlib::kAllPrefix, 0) == 0)
                        {
                            say("FINDING EVERY SONG...");
                            mdlib::BrowseList* all = new mdlib::BrowseList(mdlib::listAll(ip.substr(pl)));
                            char b[64]; std::snprintf(b, sizeof b, "%d SONGS", (int)all->items.size());
                            publishList(all); say(b);
                        }
                        else publishList(new mdlib::BrowseList(mdlib::listFolder(ip)));
                    }
                    else if (t == 3)
                    {
                        // up one folder; from a drive's top level, back to the drive list
                        std::string p = L->path;
                        const size_t pl = std::strlen(mdlib::kAllPrefix);
                        if (p.rfind(mdlib::kAllPrefix, 0) == 0) { publishList(new mdlib::BrowseList(mdlib::listFolder(p.substr(pl)))); bReq_.store(0); continue; }
                        const size_t sl = p.find_last_of('/');
                        bool atDrive = L->drives || p.empty();
                        if (!atDrive)
                        {
                            const mdlib::BrowseList drives = mdlib::listDrives(mpc::pluginFolder());
                            for (const auto& e : drives.items) if (e.path == p) atDrive = true;
                        }
                        if (atDrive || sl == std::string::npos || sl == 0) publishList(new mdlib::BrowseList(mdlib::listDrives(mpc::pluginFolder())));
                        else publishList(new mdlib::BrowseList(mdlib::listFolder(p.substr(0, sl))));
                    }
                    bReq_.store(0);
                }
                // a drive plugged in or pulled out: refresh the drive list and look for music on it
                if (++driveTick_ >= 100)
                {
                    driveTick_ = 0;
                    const std::string sig = mdlib::driveSignature();
                    if (!driveSig_.empty() && sig != driveSig_)
                    {
                        say(sig.size() > driveSig_.size() ? "DRIVE CONNECTED" : "DRIVE REMOVED");
                        const mdlib::BrowseList* cur = blist_.load();
                        if (!cur || cur->drives) publishList(new mdlib::BrowseList(mdlib::listDrives(mpc::pluginFolder())));
                        rescan_.store(true);
                    }
                    driveSig_ = sig;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        });
        analyser_ = std::thread([this] {
#if defined(__linux__)
            ::setpriority(PRIO_PROCESS, (id_t)::syscall(SYS_gettid), 15);
#endif
            while (running_.load() && foldersFound_.load() < 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            while (running_.load())
            {
                if (!analyseNext()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
                else std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
        });
        loader_ = std::thread([this] {
#if defined(__linux__)
            ::setpriority(PRIO_PROCESS, (id_t)::syscall(SYS_gettid), 10);   // background work stays out of the audio's way
#endif
            for (Sample& x : samples_)
                if (!x.audio && running_.load()) { Audio* a = readWav16(mpc::contentPath(x.file)); if (a) a->analyse(); x.audio = a; }
            samplesReady_.store(true);
            scanDrive();
            while (running_.load())
            {
                bool busy = false;
                for (Deck& d : deck_)
                {
                    delete d.retired.exchange(nullptr);
                    const int want = d.wantTrack.load();
                    if (want >= 0 && want != d.loadedTrack.load() && want < nTracks())
                    {
                        Audio* a = openTrack(want);
                        if (a) { a->track = want; a->analyse(); delete d.pending.exchange(a); }
                        d.loadedTrack.store(want);
                        busy = true;
                    }
                }
                if (const int at = bAdd_.load()) { handleAdd(at); bAdd_.store(0); busy = true; }
                if (rescan_.exchange(false)) { scanDrive(); busy = true; }
                const int sv = sinceView_.load();
                if (sv >= 8 || (sv > 0 && !busy)) { sinceView_.fetch_sub(sv); buildView(); }
                std::this_thread::sleep_for(std::chrono::milliseconds(busy ? 2 : 20));
            }
        });
    }
    void stopLoader()
    {
        if (!running_.exchange(false)) return;
        if (loader_.joinable()) loader_.join();
        if (notifier_.joinable()) notifier_.join();
        if (browser_.joinable()) browser_.join();
        if (analyser_.joinable()) analyser_.join();
    }

    void takePending(int i)
    {
        Deck& d = deck_[i];
        if (!d.pending.load() || d.retired.load()) return;
        Audio* a = d.pending.exchange(nullptr);
        if (!a) return;
        d.retired.store(d.live);
        d.live = a;
        const TrackInfo& t = tracks_[(size_t)a->track];
        d.playing = d.cuePreview = d.looping = d.braking = false;
        d.synced = false; d.brakeAmt = 1; d.jogVel = 0; d.scratching = false; d.xfLeft = 0; d.amp = 0;
        d.rollPad = d.rollFx = false; d.rollMix = 0;
        d.pos = d.cuePos = std::max(0.0, (double)t.start * a->rate);
        for (double& h : d.hot) h = -1;
        dirtyNow_ = true;
    }

    // a paused deck follows its track list; a playing deck is locked
    std::string libraryText() const
    {
        if (scanning_.load()) return "SCANNING DRIVE...";
        const int ff = foldersFound_.load();
        if (ff < 0) return "STARTING...";
        if (ff == 0) return "LOAD SONGS FROM BROWSE";
        int total = 0, ready = 0;
        const int n = nTracks();
        for (int i = 0; i < n; ++i)
        {
            const TrackInfo& t = tracks_[(size_t)i];
            if (!t.external || (t.analyzed && t.length <= 0)) continue;
            ++total; if (t.analyzed) ++ready;
        }
        char b[48];
        if (ready < total) std::snprintf(b, sizeof b, "DRIVE: %d SONGS, %d READY", total, ready);
        else std::snprintf(b, sizeof b, "DRIVE: %d SONGS READY", total);
        return b;
    }
    void browse(int i, int n)
    {
        Deck& d = deck_[i];
        // keep the pickers inside the lists (crates can appear after a drive scan)
        const int nc = crateCount(), cp = i ? kBCrate : kACrate, tp = i ? kBTrack : kATrack;
        if (nc > 0 && getInt(cp) > nc - 1) writePlain(cp, (float)(nc - 1));
        const int ns = crateSize(getInt(cp));
        if (ns > 0 && getInt(tp) > ns) writePlain(tp, (float)ns);
        const int t = trackAt(getInt(i ? kBCrate : kACrate), getInt(i ? kBTrack : kATrack));
        if (t != d.seenTrack) { d.seenTrack = t; d.settle = 0.25f; dirtyNow_ = true; }
        if (d.settle > 0) { d.settle -= (float)n / sr_; return; }
        if (!d.playing && !d.cuePreview && t >= 0 && (!d.live || d.live->track != t)) d.wantTrack.store(t);
    }

    // ------------------------------------------------------------- triggers
    // Every parameter the plugin watches for taps. The plugin also writes some of them
    // itself (lit buttons, spinning platters); writeParam() keeps lastSeen_ in step so
    // its own writes are never mistaken for a tap.
    void snapTriggers() { for (int i = 0; i < kNumParams; ++i) lastSeen_[(size_t)i] = getNormalised(i); }
    void syncTriggers() { snapTriggers(); guard_ = 0.4f; }   // after a load/state restore: ignore the restored values
    void writeParam(int i, float norm)
    {
        norm = mpc::clamp(norm, 0.0f, 1.0f);
        if (std::fabs(getNormalised(i) - norm) < 1e-5f) return;
        setNormalised(i, norm);
        lastSeen_[(size_t)i] = norm;
        notify_[i].store(1, std::memory_order_relaxed);
    }
    void writePlain(int i, float plain) { writeParam(i, param(i).toNormalised(plain)); }
    static bool watched(int i)
    {
        if (i >= kPlay && i <= kHot4) return true;
        if (i == kDeck || i == kSort || i == kShift) return true;
        if (i >= kAPlay && i < kAPlay + 2 * kDeckParams)
        {
            const int o = (i - kAPlay) % kDeckParams;
            return o <= oHot3 || o == oJog || o == oLoopSize;
        }
        if (i == kRescan || (i >= kFxPad1 && i < kFxPad1 + 16)) return true;
        if (i == kPadLearn || i == kAddSet || i == kLoadNext) return true;
        if (i == kBrowseSel || i == kBrowseRow || (i >= kBrowseOpen && i <= kBrowseDrives)) return true;
        return i >= kBpmUp && i <= kNudgeUp ? true : i == kSamplerPlay;
    }
    void readTriggers(int n)
    {
        if (guard_ > 0) { guard_ -= (float)n / sr_; snapTriggers(); return; }
        for (int i = 0; i < kNumParams; ++i)
        {
            if (i == kAZoom) i = kRescan;                 // skip the waveform columns
            if (!watched(i)) continue;
            const float v = getNormalised(i), last = lastSeen_[(size_t)i];
            if (std::fabs(v - last) < 1e-6f) continue;
            lastSeen_[(size_t)i] = v;
            handle(i, v - last);
        }
    }
    void jogMove(int i, float dn)
    {
        if (dn > 0.5f) dn -= 1.0f; else if (dn < -0.5f) dn += 1.0f;   // the platter wraps around
        Deck& d = deck_[i];
        if (!d.live) return;
        if (getInt(kJogMode) == 1)                                       // scratch: one turn = 1.8 s, like a 33 rpm record
        {
            if (!d.scratching) { d.scratching = true; d.scratchTarget = d.pos; d.scratchVel = 0; }
            d.scratchTarget = std::max(0.0, d.scratchTarget + dn * d.live->rate * 1.8);
            d.scratchHold = 0.09f;
            return;
        }
        if (d.playing) d.jogVel = mpc::clamp(d.jogVel + dn * 40.0f, -0.9f, 0.9f);
        else { d.pos = std::max(0.0, d.pos + dn * 64.0 * beatLen(d)); dirtyNow_ = true; }
    }
    void handle(int i, float dn)
    {
        const int f = focus_;
        const bool sh = shift_ || shiftLatch_;
        if (i >= kAPlay && i < kAPlay + 2 * kDeckParams)
        {
            const int d = i >= kBPlay ? 1 : 0, o = i - dp(d, 0);
            if (d == 1 && !twoDecks_) return;
            switch (o)
            {
                case oPlay: playPress(d); break;
                case oCue: cueDown(d); cueUp(d); break;
                case oSync: syncPress(d); break;
                case oLoop: loopToggle(d, deck_[d].jumpBeats); break;
                case oLoopHalf: loopResize(d, 0.5f); break;
                case oLoopDouble: loopResize(d, 2.0f); break;
                case oLoopIn: loopIn(d); break;
                case oLoopOut: loopOut(d); break;
                case oHot1: case oHot2: case oHot3: hotCue(d, o - oHot1); break;
                case oJog: jogMove(d, dn); break;
                case oLoopSize:
                    deck_[d].jumpBeats = kSizeBeats[mpc::clamp(getInt(i), 0, 8)];
                    if (deck_[d].looping) setLoop(d, deck_[d].jumpBeats);
                    break;
                default: break;
            }
            return;
        }
        switch (i)
        {
            case kPlay: if (sh) hotCue(f, 1); else playPress(f); break;
            case kCue: if (sh) hotCue(f, 0); else { cueDown(f); cueUp(f); } break;
            case kSync: if (sh) brake(f); else syncPress(f); break;
            case kLoop: loopToggle(f, deck_[f].jumpBeats); break;
            case kJumpBack: if (sh) loopResize(f, 0.5f); else jump(f, -1); break;
            case kJumpFwd: if (sh) loopResize(f, 2.0f); else jump(f, +1); break;
            case kBrake: brake(f); break;
            case kHot1: case kHot2: case kHot3: case kHot4: hotCue(f, i - kHot1); break;
            case kDeck: if (twoDecks_) focus_ = getInt(kDeck); dirtyNow_ = true; break;
            case kLoopSize: deck_[f].jumpBeats = kLoopBeats[mpc::clamp(getInt(kLoopSize), 0, 6)]; if (deck_[f].looping) setLoop(f, deck_[f].jumpBeats); break;
            case kJog: jogMove(f, dn); break;
            case kShift: shiftLatch_ = getBool(kShift); break;
            case kBpmUp: bpmStep(f, +0.1f); break;
            case kBpmDown: bpmStep(f, -0.1f); break;
            case kTempoReset: writePlain(f ? kBTempo : kATempo, 0.0f); break;
            case kBrowse: browseNext(f, sh ? -1 : 1); break;
            case kLoop2: loopToggle(f, 8); break;
            case kLoop4: loopToggle(f, 16); break;
            case kLoop8: loopToggle(f, 32); break;
            case kNudgeDown: if (sh) hotCue(f, 2); else { deck_[f].nudgePulse = 0.25f; deck_[f].nudgePulseDir = -1; } break;
            case kNudgeUp: if (sh) hotCue(f, 3); else { deck_[f].nudgePulse = 0.25f; deck_[f].nudgePulseDir = +1; } break;
            case kSamplerPlay:
                if (samplesReady_.load()) { smpIdx_ = getInt(kSampler); smpPos_ = 0; }
                break;
            case kSort: keepSelection(); break;
            case kRescan: rescan_.store(true); break;
            case kBrowseSel: bsel_ = getInt(kBrowseSel); break;
            case kBrowseRow: bsel_ = (bsel_ / 8) * 8 + getInt(kBrowseRow); break;
            case kBrowsePgUp: bsel_ = std::max(0, bsel_ - 8); break;
            case kBrowsePgDn: bsel_ += 8; break;
            case kBrowseDrives: postBrowse(1, -1); break;
            case kPadLearn:
                learnStep_ = learnStep_ >= 0 ? -1 : 0;           // tap again to stop
                padInfoDirty_ = true;
                break;
            case kAddSet:
            {
                const mdlib::BrowseList* L = blist_.load();
                if (L && bsel_ >= 0 && bsel_ < (int)L->items.size())
                {
                    if (!L->items[(size_t)bsel_].dir) postAdd(3, bsel_, -1);
                    else if (L->items[(size_t)bsel_].path.rfind(mdlib::kAllPrefix, 0) != 0) say("OPEN THE FOLDER, THEN ADD SONGS");
                }
                break;
            }
            case kLoadNext: loadNext(); break;
            case kBrowseBack: postBrowse(3, -1); break;
            case kBrowseOpen:
            {
                const mdlib::BrowseList* L = blist_.load();
                if (L && bsel_ >= 0 && bsel_ < (int)L->items.size())
                {
                    if (L->items[(size_t)bsel_].dir) postBrowse(2, bsel_);
                    else postAdd(1, bsel_, focus_);
                }
                break;
            }
            case kBrowseLoadA: case kBrowseLoadB:
            {
                const mdlib::BrowseList* L = blist_.load();
                const int d = i == kBrowseLoadB ? 1 : 0;
                if (d == 1 && !twoDecks_) break;
                if (L && bsel_ >= 0 && bsel_ < (int)L->items.size() && !L->items[(size_t)bsel_].dir) postAdd(1, bsel_, d);
                break;
            }
            case kBrowseAdd:
            {
                const mdlib::BrowseList* L = blist_.load();
                if (L && !L->drives) postAdd(2, -1, focus_);
                break;
            }
            default:
                if (i >= kFxPad1 && i < kFxPad1 + 16)
                {
                    if (getNormalised(i) >= 0.5f) fxPadDown(i - kFxPad1, 100); else fxPadUp(i - kFxPad1);
                }
                break;
        }
    }
    // ---- file browser: the audio thread only posts requests; the browser and loader threads do the disk work
    void postBrowse(int type, int idx)
    {
        if (bReq_.load()) return;                      // one at a time
        bReqList_.store(blist_.load()); bReqIdx_.store(idx); bReq_.store(type);
    }
    void postAdd(int type, int idx, int deck)
    {
        if (bAdd_.load()) return;
        bAddList_.store(blist_.load()); bAddIdx_.store(idx); bAddDeck_.store(deck); bAdd_.store(type);
    }
    std::string browseLineText(int k) const
    {
        const mdlib::BrowseList* L = blist_.load();
        if (!L) return "";
        const int idx = (bselShown_.load() / 8) * 8 + k;
        if (idx < 0 || idx >= (int)L->items.size()) return (L->items.empty() && k == 0) ? (L->drives ? "No drives found" : "No songs or folders here") : "";
        const mdlib::BrowseEntry& e = L->items[(size_t)idx];
        return (e.dir ? std::string("> ") : std::string("   ")) + mdlib::cleanName(e.name);
    }
    std::string browsePathText() const
    {
        {
            std::lock_guard<std::mutex> g(msgMu_);
            if (msgOn_.load() && !msg_.empty()) return msg_;
        }
        const mdlib::BrowseList* L = blist_.load();
        if (!L) return "Looking for drives...";
        char b[64];
        const int n = (int)L->items.size(), sel = bselShown_.load();
        std::snprintf(b, sizeof b, "  %d/%d", n ? sel + 1 : 0, n);
        std::string t = L->drives ? std::string("DRIVES") : mdlib::cleanName(L->title);
        if (t.size() > 20) t = t.substr(0, 20);
        return t + b;
    }
    // a short message on the BROWSE page (any thread)
    void say(const std::string& m)
    {
        { std::lock_guard<std::mutex> g(msgMu_); msg_ = m; }
        msgUntil_.store(msgClock_.load() + 3.0);           // shown for 3 seconds
        msgOn_.store(true);
        msgSerial_.fetch_add(1);
    }
    // which MPC pad (0-31) a note comes from, under the PAD MAP setting
    int padOf(int note) const
    {
        const int map = getInt(kPadMap);
        if (map == 1) { const int r = note - getInt(kPadStart); return (r >= 0 && r < 32) ? r : -1; }
        for (int k = 0; k < 16; ++k) if ((map == 2 ? getInt(kLearn1 + k) : kMpcBankA[k]) == note) return k;
        for (int k = 0; k < 16; ++k) if (kMpcBankB[k] == note) return 16 + k;
        return -1;
    }
    std::string padInfoText() const
    {
        char b[48];
        if (learnStep_ >= 0) { std::snprintf(b, sizeof b, "HIT PAD %d", learnStep_ + 1); return b; }
        switch (getInt(kPadMap))
        {
            case 1: return "CHROMATIC";
            case 2: return "LEARNED";
            default: return "MPC PADS";
        }
    }
    // LOAD NEXT: the next song in the SET LIST goes on the deck that isn't playing
    void loadNext()
    {
        const View* v = view();
        int c = -1;
        if (v) for (size_t k = 0; k < v->crates.size(); ++k) if (v->crates[k].name == kSetList) c = (int)k;
        if (c < 0 || v->crates[(size_t)c].tracks.empty()) { say("SET LIST IS EMPTY - ADD SONGS IN BROWSE"); return; }
        const auto& o = v->order[(size_t)c][0];
        int target;
        if (!twoDecks_) target = 0;
        else if (deck_[0].playing && !deck_[1].playing) target = 1;
        else if (deck_[1].playing && !deck_[0].playing) target = 0;
        else if (!deck_[0].playing && !deck_[1].playing) target = deck_[0].live ? 1 : 0;
        else { say("BOTH DECKS ARE PLAYING"); return; }
        if (deck_[target].playing) { say("STOP THE DECK FIRST"); return; }
        // after whatever the other deck is playing, else after the last one loaded
        int after = setPos_;
        const Deck& other = deck_[1 - target];
        const int ot = twoDecks_ && other.live ? other.live->track : -1;
        for (size_t k = 0; k < o.size(); ++k) if (o[k] == ot) after = (int)k;
        const int next = after + 1;
        if (next >= (int)o.size()) { say("END OF THE SET LIST"); return; }
        writePlain(target ? kBCrate : kACrate, (float)c);
        writePlain(target ? kBTrack : kATrack, (float)(next + 1));
        setPos_ = next;
        std::string nm = tracks_[(size_t)o[(size_t)next]].name;
        if (nm.size() > 18) nm = nm.substr(0, 18);
        say(std::string(target ? "DECK B: " : "DECK A: ") + nm);
    }

    // audio thread: point a deck at a song the loader just added
    void applySelection()
    {
        const int d = selDeck_.load();
        if (d < 0) return;
        const int t = selTrack_.load();
        const std::string cname = selCrate_;
        selDeck_.store(-1);
        const View* v = view();
        if (!v) return;
        for (size_t c = 0; c < v->crates.size(); ++c)
        {
            if (v->crates[c].name != cname) continue;
            const auto& o = v->order[c][(size_t)mpc::clamp(getInt(kSort), 0, 2)];
            for (size_t k = 0; k < o.size(); ++k)
                if (t < 0 || o[k] == t)
                {
                    writePlain(d ? kBCrate : kACrate, (float)c);
                    writePlain(d ? kBTrack : kATrack, (float)(k + 1));
                    if (twoDecks_) focus_ = d;
                    return;
                }
        }
    }

    // ---- FX pads (hold to play; velocity = how strong)
    void setPadFx(int i, int type, float beats, float amt, int pad)
    {
        Deck& d = deck_[i];
        d.pfx = type; d.pfxBeats = beats; d.pfxAmt = amt; d.pfxHeld = true; d.pfxTail = 0; d.pfxPad = pad;
        dirtyNow_ = true;
    }
    void fxPadDown(int k, int vel)
    {
        const int f = focus_;
        Deck& d = deck_[f];
        const float amt = 0.35f + 0.65f * mpc::clamp(vel, 1, 127) / 127.0f;
        switch (k)
        {
            case PF_LP: case PF_HP: d.sweepDir = k == PF_LP ? -1 : 1; d.sweepHeld = true; d.sweepAmt = amt; break;
            case PF_CENSOR: if (d.playing) { d.revOn = true; d.spin = false; d.revSpeed = 1; d.revPos = d.pos; } break;
            case PF_SPIN: if (d.playing) { d.revOn = true; d.spin = true; d.revSpeed = 3.2f; d.revPos = d.pos; } break;
            case PF_GATE2: setPadFx(f, FX_GATE, 0.5f, amt, k); break;
            case PF_GATE4: setPadFx(f, FX_GATE, 0.25f, amt, k); break;
            case PF_FLANGE: setPadFx(f, FX_FLANGER, 1.0f, amt, k); break;
            case PF_CRUSH: setPadFx(f, FX_CRUSH, 1.0f, amt, k); break;
            case PF_ECHO2: setPadFx(f, FX_ECHO, 0.5f, amt, k); break;
            case PF_ECHO1: setPadFx(f, FX_ECHO, 1.0f, amt, k); break;
            case PF_VERB: setPadFx(f, FX_REVERB, 1.0f, amt, k); break;
            case PF_ROLL8: case PF_ROLL4: case PF_ROLL2: case PF_ROLL1:
            {
                static const float rb[4] = { 0.125f, 0.25f, 0.5f, 1.0f };
                startRoll(f, rb[k - PF_ROLL8]);
                d.rollPad = d.playing; d.rollNote = 100 + k;
                break;
            }
            case PF_BRAKE: brake(f); break;
        }
        dirtyNow_ = true;
    }
    void fxPadUp(int k)
    {
        for (Deck& d : deck_)
        {
            if ((k == PF_LP && d.sweepDir < 0) || (k == PF_HP && d.sweepDir > 0)) d.sweepHeld = false;
            if (k == PF_CENSOR && !d.spin) d.revOn = false;
            if (d.pfxPad == k && d.pfxHeld) { d.pfxHeld = false; d.pfxTail = (d.pfx == FX_ECHO || d.pfx == FX_REVERB) ? 4.0f : 0.05f; }
            if (d.rollPad && d.rollNote == 100 + k) d.rollPad = false;
        }
        dirtyNow_ = true;
    }
    void padPressure(int p)
    {
        Deck& d = deck_[focus_];
        const float a = 0.3f + 0.7f * mpc::clamp(p, 0, 127) / 127.0f;
        if (p <= 0) return;
        if (d.pfxHeld) d.pfxAmt = a;
        if (d.sweepHeld) d.sweepAmt = a;
    }
    void bpmStep(int i, float bpm)
    {
        const TrackInfo* t = deck_[i].info(tracks_);
        if (!t || deck_[i].synced) return;
        const int p = i ? kBTempo : kATempo;
        writePlain(p, mpc::clamp(get(p) + bpm / t->bpm * 100.0f, -16.0f, 16.0f));
    }
    void browseNext(int i, int dir)
    {
        const int p = i ? kBTrack : kATrack;
        const int n = std::max(1, crateSize(getInt(i ? kBCrate : kACrate)));
        int v = getInt(p) + dir;
        if (v > n) v = 1; else if (v < 1) v = n;
        writePlain(p, (float)v);
    }
    // after a re-sort, point each deck's list back at the same track
    void keepSelection()
    {
        for (int d = 0; d < 2; ++d)
        {
            const int want = deck_[d].live ? deck_[d].live->track : deck_[d].seenTrack;
            const View* vw = view();
            if (want < 0 || !vw || vw->crates.empty()) continue;
            const int c = mpc::clamp(getInt(d ? kBCrate : kACrate), 0, (int)vw->crates.size() - 1);
            const auto& v = vw->order[(size_t)c][(size_t)mpc::clamp(getInt(kSort), 0, 2)];
            for (size_t k = 0; k < v.size(); ++k)
                if (v[k] == want) { writePlain(d ? kBTrack : kATrack, (float)(k + 1)); deck_[d].seenTrack = want; break; }
        }
    }

    // ------------------------------------------------------------- pads
    // Pads, laid out like the screen grid (bottom row = pads 1-4):
    //   DECK A   DECK B   LOOP     BPM +
    //   2 BARS   4 BARS   8 BARS   BPM -
    //   SYNC     JUMP -   JUMP +   SHIFT
    //   CUE      PLAY     NUDGE -  NUDGE +
    // SHIFT +: bottom row = hot cues 1-4, JUMP -/+ = loop /2 x2, SYNC = brake,
    // 2/4/8 BARS = loop roll 1/2, 1, 2 beats (hold), LOOP then a hot cue = clear it,
    // BPM +/- = tempo reset.
    void midi(const mpc::MidiEvent& e)
    {
        if (e.isAllNotesOff()) { shift_ = false; for (Deck& d : deck_) { d.nudge = 0; d.rollPad = false; if (d.cuePreview) cueUp((int)(&d - deck_)); } return; }
        const uint8_t stt = e.status();
        if (stt == 0xA0 || stt == 0xD0) { padPressure(stt == 0xA0 ? e.data[2] : e.data[1]); return; }   // pad pressure
        const bool on = e.isNoteOn(), off = e.isNoteOff();
        if (!on && !off) return;
        if (learnStep_ >= 0)
        {   // LEARN: the next 16 pads hit, in order, become pads 1-16
            if (on)
            {
                for (int k = 0; k < learnStep_; ++k) if (getInt(kLearn1 + k) == e.note()) return;   // same pad twice: ignore
                writePlain(kLearn1 + learnStep_, (float)e.note());
                if (++learnStep_ >= 16) { learnStep_ = -1; writePlain(kPadMap, 2.0f); }
                padInfoDirty_ = true;
            }
            return;
        }
        const int rel = padOf(e.note());
        // FX pads: Pad Bank B always, or Bank A while PAD MODE is FX
        if ((rel >= 16 && rel < 32) || (rel >= 0 && rel < 16 && getInt(kPadMode) == 1))
        {
            if (on) fxPadDown(rel % 16, e.velocity()); else fxPadUp(rel % 16);
            return;
        }
        if (rel < 0 || rel > 15) return;
        const int f = focus_;
        if (rel == 7) { shift_ = on; return; }
        if (off)
        {
            for (Deck& d : deck_) if (d.rollPad && d.rollNote == rel) { d.rollPad = false; dirtyNow_ = true; }
            if (rel == 0) cueUp(f);
            if (rel == 2 || rel == 3) deck_[f].nudge = 0;
            return;
        }
        const bool sh = shift_ || shiftLatch_;
        if (sh)
        {
            if (rel <= 3)
            {
                if (deleteNext_) { deck_[f].hot[rel] = -1; deleteNext_ = false; dirtyNow_ = true; }
                else hotCue(f, rel);
                return;
            }
            switch (rel)
            {
                case 4: brake(f); return;
                case 5: loopResize(f, 0.5f); return;
                case 6: loopResize(f, 2.0f); return;
                case 8: case 9: case 10:
                    startRoll(f, rel == 8 ? 0.5f : rel == 9 ? 1.0f : 2.0f);
                    deck_[f].rollPad = deck_[f].playing; deck_[f].rollNote = rel; return;
                case 11: case 15: writePlain(f ? kBTempo : kATempo, 0.0f); return;
                case 14: deleteNext_ = true; return;
                default: break;
            }
        }
        switch (rel)
        {
            case 0: cueDown(f); break;
            case 1: playPress(f); break;
            case 2: deck_[f].nudge = -1; break;
            case 3: deck_[f].nudge = +1; break;
            case 4: syncPress(f); break;
            case 5: jump(f, -1); break;
            case 6: jump(f, +1); break;
            case 8: loopToggle(f, 8); break;
            case 9: loopToggle(f, 16); break;
            case 10: loopToggle(f, 32); break;
            case 11: bpmStep(f, -0.1f); break;
            case 12: focus_ = 0; dirtyNow_ = true; break;
            case 13: if (twoDecks_) { focus_ = 1; dirtyNow_ = true; } break;
            case 14: loopToggle(f, deck_[f].jumpBeats); break;
            case 15: bpmStep(f, +0.1f); break;
        }
    }

    // ------------------------------------------------------------- deck actions
    double beatLen(const Deck& d) const
    {
        const TrackInfo* t = d.info(tracks_);
        return t && d.live ? d.live->rate * 60.0 / t->bpm : 22050.0;
    }
    double beatPhase(const Deck& d) const
    {
        const TrackInfo* t = d.info(tracks_);
        if (!t) return 0;
        const double b = (d.pos - (double)t->firstBeat * d.live->rate) / beatLen(d);
        return b - std::floor(b);
    }
    void jumpTo(Deck& d, double p)
    {
        if (d.amp > 0.001f) { d.xfPos = d.pos; d.xfLeft = 256; }
        d.pos = std::max(0.0, p);
    }
    void alignPhase(int i)
    {
        Deck& d = deck_[i];
        if (!d.live) return;
        double target;
        if (d.src == Deck::OTHER) { const Deck& o = deck_[1 - i]; if (!o.live) return; target = beatPhase(o); }
        else if (d.src == Deck::MPC && mpcRunning_) target = ppq_ - std::floor(ppq_);
        else return;
        double diff = target - beatPhase(d);
        if (diff > 0.5) diff -= 1; else if (diff < -0.5) diff += 1;
        jumpTo(d, d.pos + diff * beatLen(d));
    }
    void playPress(int i)
    {
        Deck& d = deck_[i];
        if (!d.live) return;
        if (d.cuePreview) { d.cuePreview = false; dirtyNow_ = true; return; }   // CDJ: play while holding cue keeps playing
        if (d.playing && !d.braking) d.playing = false;
        else { d.playing = true; d.braking = false; d.brakeAmt = 1; if (d.synced) alignPhase(i); }
        dirtyNow_ = true;
    }
    void cueDown(int i)
    {
        Deck& d = deck_[i];
        if (!d.live) return;
        if (d.playing && !d.cuePreview) { d.playing = false; d.braking = false; d.brakeAmt = 1; jumpTo(d, d.cuePos); }
        else if (!d.playing)
        {
            if (std::fabs(d.pos - d.cuePos) > 64) d.cuePos = d.pos;
            else d.pos = d.cuePos;
            d.playing = true; d.cuePreview = true;
        }
        dirtyNow_ = true;
    }
    void cueUp(int i)
    {
        Deck& d = deck_[i];
        if (!d.cuePreview) return;
        d.cuePreview = false; d.playing = false; jumpTo(d, d.cuePos);
        dirtyNow_ = true;
    }
    void syncPress(int i)
    {
        Deck& d = deck_[i];
        if (!d.live) return;
        if (d.synced) { d.synced = false; dirtyNow_ = true; return; }
        const Deck& o = deck_[1 - i];
        d.synced = true;
        d.src = (twoDecks_ && o.live && o.playing && !(o.synced && o.src == Deck::OTHER)) ? Deck::OTHER : Deck::MPC;
        if (d.playing) alignPhase(i);
        dirtyNow_ = true;
    }
    void setLoop(int i, float beats)
    {
        Deck& d = deck_[i];
        const TrackInfo* t = d.info(tracks_);
        if (!t) return;
        const double bl = beatLen(d), first = (double)t->firstBeat * d.live->rate;
        if (!d.looping) d.loopStart = first + std::floor((d.pos - first) / bl + 1e-4) * bl;
        d.loopEnd = d.loopStart + beats * bl;
        d.looping = true; d.jumpBeats = beats;
        if (d.pos >= d.loopEnd) jumpTo(d, d.loopStart + std::fmod(d.pos - d.loopStart, d.loopEnd - d.loopStart));
        dirtyNow_ = true;
    }
    void loopToggle(int i, float beats)
    {
        Deck& d = deck_[i];
        if (!d.live) return;
        const float cur = (float)((d.loopEnd - d.loopStart) / beatLen(d));
        if (d.looping && std::fabs(cur - beats) < 0.01f) { d.looping = false; dirtyNow_ = true; return; }
        setLoop(i, beats);
    }
    void loopResize(int i, float k)
    {
        Deck& d = deck_[i];
        d.jumpBeats = mpc::clamp(d.jumpBeats * k, 0.125f, 64.0f);
        if (d.looping) setLoop(i, d.jumpBeats);
        dirtyNow_ = true;
    }
    void loopIn(int i)
    {
        Deck& d = deck_[i];
        if (!d.live) return;
        d.loopInPos = d.pos; d.looping = false;
        dirtyNow_ = true;
    }
    void loopOut(int i)
    {
        Deck& d = deck_[i];
        if (!d.live) return;
        if (d.looping) { d.looping = false; dirtyNow_ = true; return; }
        if (d.loopInPos < 0 || d.pos <= d.loopInPos + 64) return;
        d.loopStart = d.loopInPos; d.loopEnd = d.pos; d.looping = true;
        d.jumpBeats = (float)((d.loopEnd - d.loopStart) / beatLen(d));
        jumpTo(d, d.loopStart);
        dirtyNow_ = true;
    }
    void jump(int i, int dir)
    {
        Deck& d = deck_[i];
        if (!d.live) return;
        const double s = dir * d.jumpBeats * beatLen(d);
        if (d.looping) { d.loopStart += s; d.loopEnd += s; }
        jumpTo(d, d.pos + s);
        if (!d.playing) d.cuePos = d.pos;
        dirtyNow_ = true;
    }
    void brake(int i)
    {
        Deck& d = deck_[i];
        if (!d.live || !d.playing) return;
        if (d.braking) { d.braking = false; d.brakeAmt = 1; } else d.braking = true;
        dirtyNow_ = true;
    }
    void startRoll(int i, float beats)
    {
        Deck& d = deck_[i];
        const TrackInfo* t = d.info(tracks_);
        if (!t || !d.playing) return;
        const double bl = beatLen(d), first = (double)t->firstBeat * d.live->rate;
        const double g = bl * std::min(1.0f, beats);                   // snap to the roll grid (max 1 beat)
        d.rollStart = first + std::floor((d.pos - first) / g + 1e-4) * g;
        d.rollLen = beats * bl;
        d.rollPos = d.pos;
        d.rollBeats = beats;
        dirtyNow_ = true;
    }
    void hotCue(int i, int h)
    {
        Deck& d = deck_[i];
        if (!d.live || h < 0 || h > 7) return;
        if (d.hot[h] < 0) d.hot[h] = d.pos;
        else { d.looping = false; jumpTo(d, d.hot[h]); if (!d.playing) { d.playing = true; d.braking = false; d.brakeAmt = 1; } d.cuePreview = false; }
        dirtyNow_ = true;
    }

    // ------------------------------------------------------------- audio
    static inline float hermite(float a, float b, float c, float d, float t)
    {
        const float c1 = 0.5f * (c - a), c2 = a - 2.5f * b + 2.0f * c - 0.5f * d, c3 = 0.5f * (d - a) + 1.5f * (b - c);
        return ((c3 * t + c2) * t + c1) * t + b;
    }
    static inline void readAt(const Audio* a, double p, float& l, float& r)
    {
        const long i = (long)std::floor(p);
        if (i < 1 || i + 2 >= a->frames) { l = r = 0; return; }
        const float t = (float)(p - (double)i);
        const int16_t* s = a->data.data() + (size_t)(i - 1) * 2;
        constexpr float k = 1.0f / 32768.0f;
        l = hermite(s[0], s[2], s[4], s[6], t) * k;
        r = hermite(s[1], s[3], s[5], s[7], t) * k;
    }

    float deckBpm(int i)
    {
        Deck& d = deck_[i];
        const TrackInfo* t = d.info(tracks_);
        if (!t) return 0;
        if (d.synced)
        {
            if (d.src == Deck::OTHER)
            {
                const Deck& o = deck_[1 - i];
                if (twoDecks_ && o.live && o.playing && !(o.synced && o.src == Deck::OTHER)) d.holdBpm = o.bpmNow > 0 ? o.bpmNow : d.holdBpm;
                else { d.src = Deck::HOLD; dirtyNow_ = true; }
                return d.holdBpm;
            }
            if (d.src == Deck::HOLD) return d.holdBpm;
            return d.holdBpm = mpcBpm_;
        }
        return t->bpm * (1.0f + get(i ? kBTempo : kATempo) / 100.0f);
    }

    void render(mpc::ProcessContext& ctx, int off, int n)
    {
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        float* CL = ctx.numOutputs > 3 ? ctx.outputs[2] : nullptr;
        float* CR = ctx.numOutputs > 3 ? ctx.outputs[3] : nullptr;
        const float dt = (float)n / sr_;

        // crossfader (constant power); 1 Deck mode plays A only
        const float x = (get(kXfade) + 100.0f) / 200.0f;
        float xa = std::cos(x * kPi * 0.5f), xb = std::sin(x * kPi * 0.5f);
        xa = std::min(1.0f, xa * 1.41421f); xb = std::min(1.0f, xb * 1.41421f);
        if (!twoDecks_) { xa = 1; xb = 0; }
        const float vol = std::pow(10.0f, get(kVolume) / 20.0f);
        const float cueMix = get(kCueMix) / 100.0f;

        // tempos: an unsynced deck first, so a deck synced to it sees its bpm
        const int order[2] = { deck_[0].synced && !deck_[1].synced ? 1 : 0, deck_[0].synced && !deck_[1].synced ? 0 : 1 };
        for (int k = 0; k < 2; ++k) { const int i = order[k]; deck_[i].bpmNow = deckBpm(i); }

        for (int i = 0; i < 2; ++i)
        {
            Deck& d = deck_[i];
            const TrackInfo* t = d.info(tracks_);
            const bool active = d.live && t && (i == 0 || twoDecks_);
            if (!active) { d.amp = 0; if (i == 1 && !twoDecks_) d.playing = d.cuePreview = false; continue; }

            // tone settings for this sub-block
            auto db = [](float v) { return v <= -29.5f ? 0.0f : std::pow(10.0f, v / 20.0f); };
            const float tLo = db(get(i ? kBLow : kALow)), tMid = db(get(i ? kBMid : kAMid)), tHi = db(get(i ? kBHigh : kAHigh));
            const float lev = get(i ? kBLevel : kALevel) / 100.0f, tLev = lev * lev * (i ? xb : xa);
            float fv = get(i ? kBFilter : kAFilter);
            {   // FX pad filter sweep: closes over 2 beats while held, springs back on release
                const float beatSec = 60.0f / std::max(20.0f, d.bpmNow);
                const float tgt = d.sweepHeld ? d.sweepDir * d.sweepAmt : 0.0f;
                const float stepS = d.sweepHeld ? dt / (2.0f * beatSec) : dt / 0.15f;
                d.sweep = d.sweep < tgt ? std::min(tgt, d.sweep + stepS) : std::max(tgt, d.sweep - stepS);
                if (std::fabs(d.sweep * 100.0f) > std::fabs(fv)) fv = d.sweep * 100.0f;
            }
            for (int c = 0; c < 2; ++c) { d.lo[c].set(250.0f, 0.0f); d.hi[c].set(2500.0f, 0.0f); }
            const bool fOn = std::fabs(fv) >= 3;
            if (fOn)
            {
                const float a = (std::fabs(fv) - 3) / 97.0f;
                const float fc = fv < 0 ? 20000.0f * std::pow(150.0f / 20000.0f, a) : 20.0f * std::pow(6000.0f / 20.0f, a);
                for (int c = 0; c < 2; ++c) d.flt[c].set(fc, 0.35f);
            }
            const bool pfl = getBool(i ? kBCue : kACue);

            if (d.braking) { d.brakeAmt -= dt / 0.9f; if (d.brakeAmt <= 0) { d.brakeAmt = 1; d.braking = false; d.playing = false; dirtyNow_ = true; } }
            d.jogVel *= std::exp(-dt / 0.12f);
            if (d.scratching) { d.scratchHold -= dt; if (d.scratchHold <= 0) { d.scratching = false; dirtyNow_ = true; } }
            float rate = (d.bpmNow / t->bpm) * (d.live->rate / sr_);
            if (d.nudgePulse > 0) d.nudgePulse -= dt;
            rate *= 1.0f + 0.04f * (float)(d.nudge + (d.nudgePulse > 0 ? d.nudgePulseDir : 0));
            rate += d.jogVel * (d.live->rate / sr_);
            if (d.braking) rate *= std::max(0.0f, d.brakeAmt * d.brakeAmt);
            d.rateNow = rate;

            // beat FX settings (timed to this deck's own beat)
            if (d.pfx >= 0 && !d.pfxHeld) { d.pfxTail -= dt; if (d.pfxTail <= 0) { d.pfx = -1; d.pfxPad = -1; } }
            const bool padFx = d.pfx >= 0;                       // an FX pad overrides the screen FX while it plays
            const int fxType = padFx ? d.pfx : mpc::clamp(getInt(kFxType), 0, 5);
            const float fxBeats = padFx ? d.pfxBeats : kFxBeatsV[mpc::clamp(getInt(kFxBeats), 0, 4)];
            const bool fxOn = padFx ? d.pfxHeld : getBool(i ? kBFx : kAFx);
            const float amtT = padFx ? d.pfxAmt : get(kFxAmount) / 100.0f;
            if (d.spin) { d.revSpeed *= std::exp(-dt / 0.22f); if (d.revSpeed < 0.08f) { d.revOn = false; d.spin = false; } }
            if (!d.playing) { d.revOn = false; d.spin = false; }
            const float beatOut = 60.0f / std::max(20.0f, d.bpmNow) * sr_;
            const float echoD = std::min(sr_ * 6.4f, fxBeats * beatOut);
            const float flStep = 1.0f / std::max(64.0f, fxBeats * 4.0f * beatOut);
            const bool wantRoll = fxOn && fxType == FX_ROLL && d.playing;
            if (wantRoll && !d.rollFx && !d.rollPad) { startRoll(i, fxBeats); d.rollFx = true; }
            else if (wantRoll && d.rollFx && !d.rollPad && d.rollBeats != fxBeats) startRoll(i, fxBeats);
            else if (!wantRoll && d.rollFx) { d.rollFx = false; dirtyNow_ = true; }
            if (!d.playing) d.rollPad = false;
            const float rollT = d.rollPad ? 1.0f : (d.rollFx ? amtT : 0.0f);
            const float sendT = (fxOn && fxType != FX_ROLL) ? 1.0f : 0.0f;
            const float trimT = (getBool(kAutoGain) ? std::pow(10.0f, t->gain / 20.0f) : 1.0f) * std::pow(10.0f, get(dp(i, oGain)) / 20.0f);
            const double gateLen = beatLen(d) * fxBeats, gateFirst = (double)t->firstBeat * d.live->rate;

            const float target = (d.playing || d.scratching) ? 1.0f : 0.0f;
            const double follow = 1.0 / (0.018 * sr_), maxScr = 5.0 * d.live->rate / sr_;
            const float ampStep = 1.0f / (0.004f * sr_);
            for (int s = off; s < off + n; ++s)
            {
                if (d.amp <= 0.0f && target == 0.0f) break;
                float l, r;
                readAt(d.live, d.pos, l, r);
                if (d.xfLeft > 0)
                {
                    float ol, orr; readAt(d.live, d.xfPos, ol, orr);
                    const float w = (float)d.xfLeft / 256.0f;
                    l = l * (1 - w) + ol * w; r = r * (1 - w) + orr * w;
                    d.xfPos += rate; --d.xfLeft;
                }
                if (d.rollMix > 0.0005f || rollT > 0)
                {
                    float rl, rr; readAt(d.live, d.rollPos, rl, rr);
                    l += (rl - l) * d.rollMix; r += (rr - r) * d.rollMix;
                    d.rollPos += rate;
                    if (d.rollPos >= d.rollStart + d.rollLen) d.rollPos -= d.rollLen;
                }
                d.rollMix += (rollT - d.rollMix) * 0.01f;
                if (d.revMix > 0.0005f || d.revOn)               // censor / backspin: a reverse head, the track runs on underneath
                {
                    float rl, rr; readAt(d.live, d.revPos, rl, rr);
                    l += (rl - l) * d.revMix; r += (rr - r) * d.revMix;
                    d.revPos = std::max(2.0, d.revPos - rate * d.revSpeed);
                }
                d.revMix += ((d.revOn ? 1.0f : 0.0f) - d.revMix) * 0.01f;
                d.amp = target > d.amp ? std::min(target, d.amp + ampStep) : std::max(target, d.amp - ampStep);
                d.trim += (trimT - d.trim) * 0.001f;
                l *= d.amp * d.trim; r *= d.amp * d.trim;
                if (d.scratching)
                {   // the head chases the hand: speed and direction come from how far the platter is ahead
                    const double v = mpc::clamp((d.scratchTarget - d.pos) * follow, -maxScr, maxScr);
                    d.scratchVel += (float)((v - d.scratchVel) * 0.03);
                    d.pos = std::max(0.0, d.pos + d.scratchVel);
                }
                else d.pos += rate;
                if (d.looping && d.pos >= d.loopEnd) { d.xfPos = d.pos; d.xfLeft = 128; d.pos -= (d.loopEnd - d.loopStart); }
                if (d.pos >= (double)d.live->frames - 4) { d.pos = (double)d.live->frames - 4; d.playing = false; d.cuePreview = false; dirtyNow_ = true; }

                // isolator EQ + filter
                float y[2] = { l, r };
                d.gLo += (tLo - d.gLo) * 0.002f; d.gMid += (tMid - d.gMid) * 0.002f; d.gHi += (tHi - d.gHi) * 0.002f;
                d.xg += (tLev - d.xg) * 0.002f;
                for (int c = 0; c < 2; ++c)
                {
                    float lp, bp, hp, lp2, bp2, hp2;
                    d.lo[c].processAll(y[c], lp, bp, hp);
                    d.hi[c].processAll(y[c], lp2, bp2, hp2);
                    const float mid = y[c] - lp - hp2;
                    float v = lp * d.gLo + mid * d.gMid + hp2 * d.gHi;
                    if (fOn) { float a, bb, cc; d.flt[c].processAll(v, a, bb, cc); v = fv < 0 ? a : cc; }
                    y[c] = v;
                }

                // beat FX
                d.fxSend += (sendT - d.fxSend) * 0.003f;
                d.fxAmt += (amtT - d.fxAmt) * 0.003f;
                if (d.fxSend > 0.0005f || fxType == FX_ECHO || fxType == FX_REVERB)
                {
                    switch (fxType)
                    {
                        case FX_ECHO:
                            for (int c = 0; c < 2; ++c)
                            {
                                const float e = d.echo[c].readLinear(echoD);
                                d.echo[c].write(y[c] * d.fxSend + e * 0.5f);
                                y[c] += e * d.fxAmt * 0.7f;
                            }
                            break;
                        case FX_REVERB:
                        {
                            float a, b; d.verb.run(y[0] * d.fxSend, y[1] * d.fxSend, a, b);
                            y[0] += a * d.fxAmt * 1.6f; y[1] += b * d.fxAmt * 1.6f;
                            break;
                        }
                        case FX_FLANGER:
                        {
                            d.flPh += flStep; if (d.flPh >= 1) d.flPh -= 1;
                            const float mix = d.fxSend * d.fxAmt;
                            for (int c = 0; c < 2; ++c)
                            {
                                const float ph = d.flPh + (c ? 0.25f : 0.0f);
                                const float dl = (0.0006f + 0.0044f * (0.5f + 0.5f * std::sin(6.2831853f * ph))) * sr_;
                                const float fo = d.flange[c].readLinear(dl);
                                d.flange[c].write(y[c] + fo * 0.6f);
                                y[c] = y[c] * (1 - 0.45f * mix) + fo * 0.75f * mix;
                            }
                            break;
                        }
                        case FX_CRUSH:
                        {
                            const float depth = d.fxSend * d.fxAmt;
                            if (d.crushCnt <= 0)
                            {
                                const float lv = std::pow(2.0f, 15.0f - depth * 11.5f);
                                for (int c = 0; c < 2; ++c) d.crushHold[c] = std::round(y[c] * lv) / lv;
                                d.crushCnt = 1 + depth * 22;
                            }
                            d.crushCnt -= 1;
                            for (int c = 0; c < 2; ++c) y[c] += (d.crushHold[c] * 1.1f - y[c]) * d.fxSend;
                            break;
                        }
                        case FX_GATE:
                        {
                            double ph = (d.pos - gateFirst) / gateLen; ph -= std::floor(ph);
                            const float g = ph < 0.5 ? 1.0f : 1.0f - d.fxSend * d.fxAmt;
                            d.gateG += (g - d.gateG) * 0.03f;
                            y[0] *= d.gateG; y[1] *= d.gateG;
                            break;
                        }
                        default: break;
                    }
                }
                L[s] += y[0] * d.xg * vol; R[s] += y[1] * d.xg * vol;
                const float pk = std::max(std::fabs(y[0]), std::fabs(y[1])) * std::sqrt(std::max(d.xg, 0.0f) / std::max(1e-3f, i ? xb : xa) + 1e-9f);
                if (pk > d.meterPeak) d.meterPeak = pk;
                if (CL && pfl) { CL[s] += y[0] * 0.8f; CR[s] += y[1] * 0.8f; }
            }
        }

        // sampler (one-shots from the Samples folder) on top of the mix
        if (smpIdx_ >= 0 && samplesReady_.load())
        {
            const Audio* a = (smpIdx_ < (int)samples_.size()) ? samples_[(size_t)smpIdx_].audio : nullptr;
            const float sv = get(kSamplerVol) / 100.0f;
            const double step = a ? a->rate / sr_ : 1.0;
            for (int s = off; s < off + n && a; ++s)
            {
                float l, r; readAt(a, smpPos_ + 1, l, r);
                L[s] += l * sv * vol; R[s] += r * sv * vol;
                smpPos_ += step;
                if (smpPos_ + 4 >= (double)a->frames) { smpIdx_ = -1; break; }
            }
            if (!a) smpIdx_ = -1;
        }

        // master soft limit; cue bus = cue decks blended with master
        for (int s = off; s < off + n; ++s)
        {
            L[s] = softLimit(L[s]); R[s] = softLimit(R[s]);
            if (CL) { CL[s] = softLimit(CL[s] * (1 - cueMix) + L[s] * cueMix); CR[s] = softLimit(CR[s] * (1 - cueMix) + R[s] * cueMix); }
        }
    }
    static inline float softLimit(float x)
    {
        const float a = std::fabs(x);
        if (a <= 0.8f) return x;
        const float y = 0.8f + 0.2f * std::tanh((a - 0.8f) / 0.2f);
        return x < 0 ? -y : y;
    }

    // ------------------------------------------------------------- screen status
    // max 31 characters on the MPC: most important first
    std::string status(int i) const
    {
        const Deck& d = deck_[i];
        if (i == 1 && getInt(kMode) == 0) return "1 DECK MODE";
        const int lt = d.loadedTrack.load();
        const int fl = d.stFlags.load();
        if (!(fl & 32)) return lt >= 0 ? "LOADING..." : "NO TRACK";
        std::vector<std::string> parts;
        char b[32];
        parts.push_back(std::string((fl & 8) ? "> " : "") + ((fl & 1) ? "PLAY" : "PAUSE"));
        std::snprintf(b, sizeof b, "%.1f", d.stBpm.load()); parts.push_back(b);
        if (fl & 2) parts.push_back((fl & 16) ? "SYNC MPC" : "SYNC");
        if (fl & 4)
        {
            const float lb = d.stLoop.load();
            if (lb < 4) std::snprintf(b, sizeof b, "LOOP %gBT", lb); else std::snprintf(b, sizeof b, "LOOP %gBAR", lb / 4);
            parts.push_back(b);
        }
        if (fl & 1024) parts.push_back("FX PAD");
        if (fl & 512) parts.push_back("ROLL");
        else if (fl & 256) parts.push_back(kFxShort[mpc::clamp(getInt(kFxType), 0, 5)]);
        if (fl & 128) parts.push_back("LOCKED");
        const TrackInfo* t = trackPtr(lt);
        if (t && t->key[0]) parts.push_back(t->key);
        std::string s;
        for (const std::string& p : parts)
        {
            if (s.size() + (s.empty() ? 0 : 2) + p.size() > 31) continue;
            if (!s.empty()) s += "  ";
            s += p;
        }
        return s;
    }
    static int sizeIndex(float beats)
    {
        int best = 0;
        for (int k = 1; k < 9; ++k) if (std::fabs(std::log2(kSizeBeats[k] / beats)) < std::fabs(std::log2(kSizeBeats[best] / beats))) best = k;
        return best;
    }
    // Keep the screen in step with the decks: lit buttons every block, live
    // displays (time, BPM, meters, waveforms, platters) a few times a second.
    void publishBrowser()
    {
        const mdlib::BrowseList* L = blist_.load();
        const int serial = listSerial_.load();
        if (serial != lastSerial_) { lastSerial_ = serial; bsel_ = 0; }
        const int count = L ? (int)L->items.size() : 0;
        bsel_ = mpc::clamp(bsel_, 0, std::max(0, count - 1));
        bselShown_.store(bsel_);
        writePlain(kBrowseSel, (float)bsel_);
        writePlain(kBrowseRow, (float)(bsel_ % 8));
        const int page = bsel_ / 8;
        for (int k = 0; k < 8; ++k)
        {
            const long sig = (long)serial * 100000L + page * 8 + k;
            if (sig != bLineSig_[k]) { bLineSig_[k] = sig; bLineCount_[k] = (bLineCount_[k] + 1) % 1000; writePlain(kBrowseLine1 + k, (float)bLineCount_[k]); }
        }
        if (msgOn_.load() && msgClock_.load() > msgUntil_.load()) { msgOn_.store(false); msgSerial_.fetch_add(1); }
        if (padInfoDirty_) { padInfoDirty_ = false; padInfoCount_ = (padInfoCount_ + 1) % 1000; writePlain(kPadInfo, (float)padInfoCount_); }
        const long ps = (long)serial * 100000L + bsel_ + (long)msgSerial_.load() * 7919L;
        if (ps != bPathSig_) { bPathSig_ = ps; bPathCount_ = (bPathCount_ + 1) % 1000; writePlain(kBrowsePath, (float)bPathCount_); }
    }
    void publish(int n)
    {
        const float dt = (float)n / sr_;
        msgClock_.store(msgClock_.load() + dt);
        if (guard_ > 0) return;              // just after a project/state load: leave the restored values alone
        publishBrowser();
        for (int i = 0; i < 2; ++i)
        {
            Deck& d = deck_[i];
            int fl = 0;
            if (d.playing) fl |= 1;
            if (d.synced) fl |= 2;
            if (d.looping) fl |= 4;
            if (focus_ == i && twoDecks_) fl |= 8;
            if (d.synced && d.src == Deck::MPC) fl |= 16;
            if (d.live) fl |= 32 | 64;
            if (d.live && d.seenTrack >= 0 && d.live->track != d.seenTrack && (d.playing || d.cuePreview)) fl |= 128;
            if (getBool(i ? kBFx : kAFx)) fl |= 256;
            if (d.rollPad || d.rollFx) fl |= 512;
            if ((d.pfx >= 0 && d.pfxHeld) || d.sweepHeld || d.revOn) fl |= 1024;
            d.stLoop.store(d.looping ? (float)((d.loopEnd - d.loopStart) / beatLen(d)) : 0.0f);
            d.stBpm.store(std::round(d.bpmNow * 10.0f) / 10.0f);
            const int sig = fl * 31 + (d.live ? d.live->track : -1) * 7 + getInt(kFxType) + (int)std::lround(d.stLoop.load() * 8) * 1000 + (int)std::lround(d.stBpm.load() * 10) * 100000;
            if (sig != lastStat_[i] || fl != d.stFlags.load())
            {
                d.stFlags.store(fl);
                lastStat_[i] = sig;
                d.statusFlip ^= 1;
                writeParam(i ? kBStatus : kAStatus, (float)d.statusFlip);
            }
            // lit buttons
            writeParam(dp(i, oPlay), d.playing ? 1.0f : 0.0f);
            writeParam(dp(i, oSync), d.synced ? 1.0f : 0.0f);
            writeParam(dp(i, oLoop), d.looping ? 1.0f : 0.0f);
            for (int h = 0; h < 3; ++h) writeParam(dp(i, oHot1 + h), d.hot[h] >= 0 ? 1.0f : 0.0f);
            writePlain(dp(i, oLoopSize), (float)sizeIndex(d.jumpBeats));
        }
        const Deck& fd = deck_[focus_];
        const float fl = fd.looping ? (float)((fd.loopEnd - fd.loopStart) / beatLen(fd)) : 0.0f;
        writeParam(kPlay, fd.playing ? 1.0f : 0.0f);
        writeParam(kSync, fd.synced ? 1.0f : 0.0f);
        writeParam(kLoop, fd.looping ? 1.0f : 0.0f);
        writeParam(kLoop2, fd.looping && std::fabs(fl - 8) < 0.01f ? 1.0f : 0.0f);
        writeParam(kLoop4, fd.looping && std::fabs(fl - 16) < 0.01f ? 1.0f : 0.0f);
        writeParam(kLoop8, fd.looping && std::fabs(fl - 32) < 0.01f ? 1.0f : 0.0f);
        if (twoDecks_) writeParam(kDeck, (float)focus_);
        for (int k = 0; k < 4; ++k) writeParam(kHot1 + k, fd.hot[k] >= 0 ? 1.0f : 0.0f);

        // live displays
        const int lv = getInt(kLiveView);
        liveTimer_ += dt; textTimer_ += dt;
        const bool live = lv > 0 && liveTimer_ >= (lv == 2 ? 1.0f / 12.0f : 1.0f / 5.0f);
        const bool text = live || textTimer_ >= 0.5f;
        if (live) liveTimer_ = 0;
        if (text) textTimer_ = 0;
        if (!text) return;
        {
            long ready = 0;
            const int nt = nTracks();
            for (int k = 0; k < nt; ++k) if (tracks_[(size_t)k].analyzed) ++ready;
            const long sig = (scanning_.load() ? 1L : 0L) + (foldersFound_.load() + 2) * 10L + (long)nt * 1000L + ready * 10000000L;
            if (sig != libSig_) { libSig_ = sig; libCount_ = (libCount_ + 1) % 1000; writePlain(kLibInfo, (float)libCount_); }
        }
        for (int i = 0; i < 2; ++i)
        {
            Deck& d = deck_[i];
            const TrackInfo* t = d.info(tracks_);
            const bool on = t && (i == 0 || twoDecks_);
            if (!on)
            {   // clear the displays once when a deck goes empty; otherwise leave them alone
                if (d.wasOn)
                {
                    d.wasOn = false;
                    writePlain(dp(i, oBpm), 0.0f); writeParam(dp(i, oTime), 0.0f); writeParam(dp(i, oMeter), 0.0f);
                    for (int c = 0; c < kCols; ++c) { writeParam((i ? kBZoom : kAZoom) + c, 0); writeParam((i ? kBWave : kAWave) + c, 0); }
                }
                continue;
            }
            d.wasOn = true;
            writeParam(dp(i, oTime), (float)(d.pos / std::max(1.0, (double)d.live->frames)));
            writePlain(dp(i, oBpm), d.bpmNow);
            const long sig = on ? (long)(t - tracks_.data()) * 100000L + std::lround(((getBool(kAutoGain) ? t->gain : 0.0f) + get(dp(i, oGain))) * 10) * 1000 + std::lround((d.bpmNow / t->bpm - 1) * 1000) : -1;
            if (sig != d.infoSig) { d.infoSig = sig; d.infoCount = (d.infoCount + 1) % 1000; writePlain(dp(i, oInfo), (float)d.infoCount); }
            if (!live) continue;
            const float m = d.meterPeak; d.meterPeak *= 0.0f;
            writeParam(dp(i, oMeter), m > 1e-4f ? std::round(mpc::clamp((20.0f * std::log10(m) + 42.0f) / 44.0f, 0.0f, 1.0f) * 16.0f) / 16.0f : 0.0f);
            const double rev = d.live->rate * 1.8;                      // 33 1/3 rpm
            const float turn = (float)(d.pos / rev - std::floor(d.pos / rev));
            const float spin = (float)((int)(turn * 64.0f) % 64) / 63.0f;   // platter art: 64 frames per turn
            if (!d.scratching)                                          // leave the platter alone while a hand is on it
            {
                writeParam(dp(i, oJog), spin);
                if (i == focus_) writeParam(kJog, spin);
            }
            // zoomed waveform: 5 beats around the play position (1/8 beat per column)
            const double col = beatLen(d) / 8.0;
            const std::vector<float>& env = d.live->env;
            for (int c = 0; c < kCols; ++c)
            {
                const double a0 = d.pos + (c - kCols / 2) * col;
                float pk = 0;
                if (a0 >= 0 && a0 < d.live->frames)
                {
                    const long b0 = (long)(a0 / 256), b1 = std::min((long)env.size() - 1, (long)((a0 + col) / 256));
                    for (long b = b0; b <= b1; ++b) pk = std::max(pk, env[(size_t)b]);
                }
                const float h = std::round(std::min(1.0f, pk) * 24.0f) / 24.0f;     // 24 heights: skip tiny changes
                writeParam((i ? kBZoom : kAZoom) + c, c < kCols / 2 ? 0.5f + 0.5f * h : 0.49f * h);
            }
            // overview: the whole track, played part lit
            const double frac = d.pos / std::max(1.0, (double)d.live->frames);
            for (int c = 0; c < kCols; ++c)
            {
                const float h = std::round(std::min(1.0f, d.live->overview[c] * 1.6f) * 24.0f) / 24.0f;
                writeParam((i ? kBWave : kAWave) + c, (c + 0.5) / kCols <= frac ? 0.5f + 0.5f * h : 0.49f * h);
            }
        }
        if (live && twoDecks_ && deck_[0].live && deck_[1].live)
        {
            double x = beatPhase(deck_[0]) - beatPhase(deck_[1]);
            if (x > 0.5) x -= 1; else if (x < -0.5) x += 1;
            writePlain(kPhase, (float)x);
        }
    }

    // ------------------------------------------------------------- state
    std::vector<TrackInfo> tracks_;                     // reserved up front: never reallocates
    std::atomic<int> nTracks_{0};
    std::vector<Crate> builtCrates_;
    std::atomic<View*> view_{nullptr};
    std::vector<std::unique_ptr<View>> views_;
    std::vector<std::string> folders_;
    std::vector<std::map<std::string, mdlib::CacheLine>> caches_;
    std::set<std::string> knownPaths_;
    std::map<std::string, int> knownIndex_;
    std::map<std::string, mdlib::CacheLine> browseCache_;
    bool browseCacheLoaded_ = false;
    std::vector<int> browsed_;
    mutable std::mutex msgMu_;
    std::string msg_;
    std::atomic<double> msgClock_{0.0}, msgUntil_{0.0};
    std::atomic<bool> msgOn_{false};
    int learnStep_ = -1, setPos_ = -1, padInfoCount_ = 0, driveTick_ = 0;
    bool padInfoDirty_ = true;
    std::atomic<int> msgSerial_{0};
    std::string driveSig_;
    std::vector<Crate> addedCrates_;
    std::thread browser_;
    std::atomic<mdlib::BrowseList*> blist_{nullptr}, bReqList_{nullptr}, bAddList_{nullptr};
    std::vector<std::unique_ptr<mdlib::BrowseList>> blists_;
    std::atomic<int> bReq_{0}, bReqIdx_{0}, bAdd_{0}, bAddIdx_{0}, bAddDeck_{0}, listSerial_{0}, bselShown_{0};
    std::atomic<int> selDeck_{-1}, selTrack_{-1};
    std::string selCrate_;
    int bsel_ = 0, lastSerial_ = -1, bLineCount_[8] = {0}, bPathCount_ = 0; long bLineSig_[8] = {-1, -1, -1, -1, -1, -1, -1, -1}; long bPathSig_ = -1;
    std::mutex infoMu_;
    std::atomic<bool> rescan_{false}, scanning_{false};
    std::atomic<int> foldersFound_{-1};
    int bgCursor_ = 0, libCount_ = 0; long libSig_ = -1;
    std::atomic<int> sinceView_{0};
    std::unique_ptr<std::atomic<uint8_t>[]> claim_;
    std::mutex cacheMu_;
    std::thread analyser_;
    Deck deck_[2];
    std::atomic<bool> running_{false}, samplesReady_{false};
    std::thread loader_, notifier_;
    std::vector<Sample> samples_;
    std::unique_ptr<std::atomic<uint8_t>[]> notify_;
    std::vector<float> lastSeen_;
    bool shiftLatch_ = false;
    int smpIdx_ = -1; double smpPos_ = 0; float liveTimer_ = 0, textTimer_ = 0;
    float sr_ = 44100, mpcBpm_ = 120, guard_ = 0.4f;
    double ppq_ = 0, clock_ = 0, lastUpdate_ = -1, bpmChangedAt_ = 0;
    bool mpcRunning_ = false, twoDecks_ = true, shift_ = false, deleteNext_ = false;
    bool dirtyNow_ = true, bpmDirty_ = false;
    int focus_ = 0, lastSig_ = 0, lastStat_[2] = { -1, -1 };
};

} // namespace

MPC_PLUGIN_ENTRY(MeltDeck)
