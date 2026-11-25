#include "TmuProcessor.h"
#include "TmuEditor.h"
#include <set>

#if defined(_WIN32)
  #include <cstdio>
#endif

// fopen that takes UTF-8 paths on every system (declared in mpc_resources.h)
std::FILE* mpc::openFile(const std::string& path, const char* mode)
{
#if defined(_WIN32)
    return _wfopen(juce::String::fromUTF8(path.c_str()).toWideCharPointer(), juce::String(mode).toWideCharPointer());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

namespace tmu {

// ------------------------------------------------------------------ parameters
static juce::String idFor(const mpc::Plugin& p, int i) { return juce::String(p.param(i).id.c_str()); }

HostParam::HostParam(mpc::Plugin& p, int i)
    : juce::AudioProcessorParameterWithID(juce::ParameterID(idFor(p, i), 1), juce::String(p.param(i).name.c_str())),
      plugin(p), index(i) {}

float HostParam::getDefaultValue() const
{
    const mpc::Param& pr = plugin.param(index);
    return pr.toNormalised(pr.def);
}
juce::String HostParam::getText(float v, int maxLen) const
{
    const mpc::Param& pr = plugin.param(index);
    const float plain = pr.toPlain(v);
    std::string s;
    if (!plugin.formatValue(index, plain, s)) s = pr.display(plain);
    return juce::String::fromUTF8(s.c_str()).substring(0, maxLen);
}
float HostParam::getValueForText(const juce::String& text) const
{
    const mpc::Param& pr = plugin.param(index);
    if (pr.kind == mpc::Param::Kind::Choice)
        for (size_t k = 0; k < pr.choices.size(); ++k)
            if (text.equalsIgnoreCase(pr.choices[k].c_str())) return pr.toNormalised((float)k);
    if (pr.kind == mpc::Param::Kind::Bool) return (text.equalsIgnoreCase("on") || text.getIntValue() != 0) ? 1.0f : 0.0f;
    return pr.toNormalised(text.retainCharacters("-0123456789.").getFloatValue());
}
int HostParam::getNumSteps() const
{
    const mpc::Param& pr = plugin.param(index);
    switch (pr.kind)
    {
        case mpc::Param::Kind::Bool: return 2;
        case mpc::Param::Kind::Choice: return juce::jmax(2, (int)pr.choices.size());
        case mpc::Param::Kind::Int: return juce::jlimit(2, 0x7fffffff, (int)(pr.max - pr.min) + 1);
        default: return juce::AudioProcessor::getDefaultNumParameterSteps();
    }
}
bool HostParam::isDiscrete() const { return plugin.param(index).kind != mpc::Param::Kind::Float; }
bool HostParam::isBoolean() const { return plugin.param(index).kind == mpc::Param::Kind::Bool; }
juce::StringArray HostParam::getAllValueStrings() const
{
    juce::StringArray a;
    const mpc::Param& pr = plugin.param(index);
    if (pr.kind == mpc::Param::Kind::Choice) for (const auto& c : pr.choices) a.add(c.c_str());
    return a;
}

// ------------------------------------------------------------------ skin helpers
static juce::var loadSkin()
{
    int size = 0;
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        if (juce::String(BinaryData::originalFilenames[i]) == "TUI.json")
            if (const char* d = BinaryData::getNamedResource(BinaryData::namedResourceList[i], size))
                return juce::JSON::parse(juce::String::fromUTF8(d, size));
    return {};
}

static int handleParam(const juce::var& entry)
{
    const juce::var map = entry["handle remapping"]["map"];
    if (map.isArray() && map.size() > 0)
    {
        const juce::String v = map[0]["value"].toString();
        return v.fromLastOccurrenceOf(" ", false, false).getIntValue();
    }
    return -1;
}

// Parameters the screen only shows (meters, waveforms, status lines) are kept away from the
// host: they change all the time and must never be automated.
static void collectUse(const juce::var& entry, const juce::var& local, int handle, std::set<int>& control, std::set<int>& shown)
{
    const juce::var cd = entry["componentData"];
    const juce::String type = cd["type"].toString();
    const int h = handleParam(entry);
    if (h >= 0) handle = h;
    if (local.hasProperty(type))
    {
        const juce::var comp = local[juce::Identifier(type)];
        const juce::var kids = comp["componentsData"];
        bool interactive = false;
        for (int k = 0; k < kids.size(); ++k)
        {
            const juce::String t = kids[k]["componentData"]["type"].toString();
            if (t == "Knob" || t == "Button") interactive = true;
        }
        const juce::var acts = comp["actions"];
        for (int k = 0; k < acts.size(); ++k) if (acts[k]["handler"].toString() == "Show Overlay") interactive = true;
        if (handle >= 0) (interactive ? control : shown).insert(handle);
        return;
    }
    if (handle < 0) return;
    if (type == "Knob" || type == "Button") control.insert(handle);
    else shown.insert(handle);
}

// ------------------------------------------------------------------ processor
Processor::BusesProperties Processor::busesFor(bool instrument)
{
    if (instrument)
        return BusesProperties().withOutput("Main", juce::AudioChannelSet::stereo(), true)
                                .withOutput("Cue", juce::AudioChannelSet::stereo(), false);
    return BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                            .withOutput("Output", juce::AudioChannelSet::stereo(), true);
}

static std::unique_ptr<mpc::Plugin> makeEngine()
{
    // each plugin keeps its files in Music/T-MU/<name> (saved WAVs, caches, its music folder)
    const juce::File folder = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                                  .getChildFile("T-MU").getChildFile(JucePlugin_Name);
    folder.getChildFile("Content").createDirectory();
    mpc::setPluginFolder(folder.getFullPathName().replaceCharacter('\\', '/').toStdString());
    return std::unique_ptr<mpc::Plugin>(mpcCreatePlugin());
}

Processor::Processor()
    : Processor(makeEngine()) {}

Processor::Processor(std::unique_ptr<mpc::Plugin> e)
    : juce::AudioProcessor(busesFor(e->info().isInstrument)), engine(std::move(e))
{
    tui = loadSkin();
    std::set<int> control, shown;
    const juce::var local = [&] {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        const juce::var defs = tui["pageData"]["componentDefinitions"]["localComponentDefinitions"];
        for (int i = 0; i < defs.size(); ++i) o->setProperty(juce::Identifier(defs[i]["key"].toString()), defs[i]["value"]);
        return juce::var(o.get());
    }();
    const juce::var tabs = tui["pageData"]["tabs"];
    for (int t = 0; t < tabs.size(); ++t)
    {
        const juce::var comps = tabs[t]["componentDefinition"]["componentsData"];
        for (int k = 0; k < comps.size(); ++k) collectUse(comps[k], local, -1, control, shown);
    }
    const int n = engine->numParams();
    hostParams.assign((size_t)n, nullptr);
    written.reset(new std::atomic<uint8_t>[(size_t)n]);
    for (int i = 0; i < n; ++i)
    {
        written[(size_t)i].store(0);
        const bool displayOnly = shown.count(i) && !control.count(i);
        if (displayOnly) continue;
        auto* p = new HostParam(*engine, i);
        hostParams[(size_t)i] = p;
        addParameter(p);
    }
    engine->onParamWritten = [this, n](int i) { if (i >= 0 && i < n) written[(size_t)i].store(1, std::memory_order_relaxed); };
    startTimerHz(30);
}

Processor::~Processor()
{
    stopTimer();
    engine->onParamWritten = nullptr;
}

bool Processor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto out = l.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono()) return false;
    if (engine->info().isInstrument)
    {
        if (l.outputBuses.size() > 1)
        {
            const auto cue = l.outputBuses[1];
            if (!cue.isDisabled() && cue != juce::AudioChannelSet::stereo()) return false;
        }
        return true;
    }
    return l.getMainInputChannelSet() == out;
}

void Processor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    maxBlock = juce::jlimit(32, 2048, samplesPerBlock);
    inCopy.setSize(2, maxBlock, false, true, true);
    scratch.setSize(4, maxBlock, false, true, true);
    midiEvents.reserve(1024);
    engine->bridgePrepare(sampleRate, maxBlock);
}

double Processor::getTailLengthSeconds() const
{
    const int t = engine->info().tailSamples;
    return t > 1 ? t / juce::jmax(1.0, sr) : 0.0;
}

void Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    if (engine->bridgeTakeStateLoad()) engine->reset();

    const mpc::PluginInfo& info = engine->info();
    const int total = buffer.getNumSamples();
    const int nch = buffer.getNumChannels();

    mpc::Transport tr;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            tr.valid = true;
            tr.playing = pos->getIsPlaying();
            tr.recording = pos->getIsRecording();
            tr.cycling = pos->getIsLooping();
            if (auto b = pos->getBpm()) { tr.bpm = *b; tr.tempoValid = true; }
            if (auto q = pos->getPpqPosition()) { tr.ppqPos = *q; tr.ppqValid = true; }
            if (auto q = pos->getPpqPositionOfLastBarStart()) tr.barStartPpq = *q;
            if (auto lp = pos->getLoopPoints()) { tr.cycleStartPpq = lp->ppqStart; tr.cycleEndPpq = lp->ppqEnd; }
            if (auto s = pos->getTimeInSamples()) tr.samplePos = (double)*s;
            if (auto ts = pos->getTimeSignature()) { tr.timeSigNumerator = ts->numerator; tr.timeSigDenominator = ts->denominator; }
        }

    // the engine was prepared for maxBlock samples: hosts may send more, so work in chunks
    for (int start = 0; start < total; start += maxBlock)
    {
        const int len = juce::jmin(maxBlock, total - start);
        midiEvents.clear();
        for (const auto m : midi)
        {
            if (m.samplePosition < start || m.samplePosition >= start + len) continue;
            const auto msg = m.getMessage();
            if (msg.getRawDataSize() > 3 || midiEvents.size() >= 1024) continue;
            mpc::MidiEvent e;
            e.sampleOffset = m.samplePosition - start;
            e.size = (uint8_t)msg.getRawDataSize();
            for (int k = 0; k < e.size; ++k) e.data[k] = msg.getRawData()[k];
            midiEvents.push_back(e);
        }
        // inputs: copied first, because the engine writes its outputs into the same buffer
        const float* in[2] = { nullptr, nullptr };
        if (info.numInputs > 0)
            for (int c = 0; c < 2; ++c)
            {
                inCopy.copyFrom(c, 0, buffer, juce::jmin(c, nch - 1), start, len);
                in[c] = inCopy.getReadPointer(c);
            }
        float* out[4];
        for (int c = 0; c < 4; ++c)
        {
            if (c < info.numOutputs && c < nch) out[c] = buffer.getWritePointer(c, start);
            else out[c] = scratch.getWritePointer(c);
            juce::FloatVectorOperations::clear(out[c], len);
        }
        mpc::ProcessContext ctx;
        ctx.inputs = info.numInputs > 0 ? in : nullptr;
        ctx.outputs = out;
        ctx.numInputs = info.numInputs > 0 ? 2 : 0;
        ctx.numOutputs = juce::jmin(4, juce::jmax(2, info.numOutputs));
        ctx.numFrames = len;
        ctx.midi = midiEvents.data();
        ctx.numMidiEvents = (int)midiEvents.size();
        ctx.sampleRate = sr;
        ctx.transport = tr;
        engine->process(ctx);
        // a mono track gets both sides mixed
        if (nch == 1)
        {
            juce::FloatVectorOperations::add(buffer.getWritePointer(0, start), scratch.getReadPointer(1), len);
            juce::FloatVectorOperations::multiply(buffer.getWritePointer(0, start), 0.5f, len);
        }
        // move the clock on for the next chunk
        if (tr.ppqValid) tr.ppqPos += len / sr * tr.bpm / 60.0;
        tr.samplePos += len;
    }
    for (int c = juce::jmin(info.numOutputs, 4); c < nch; ++c) buffer.clear(c, 0, total);
    midi.clear();
}

void Processor::timerCallback()
{
    // the engine set some of its own controls (a button popping back up, a track picked from the
    // browser...): show it in the host too
    for (size_t i = 0; i < hostParams.size(); ++i)
        if (written[i].load(std::memory_order_relaxed) && written[i].exchange(0))
            if (HostParam* p = hostParams[i])
                p->sendValueChangedMessageToListeners(p->getValue());
}

void Processor::setFromUi(int index, float norm, bool gesture)
{
    if (index < 0 || index >= engine->numParams()) return;
    if (HostParam* p = hostParams[(size_t)index])
    {
        if (gesture) p->beginChangeGesture();
        p->setValueNotifyingHost(norm);
        if (gesture) p->endChangeGesture();
    }
    else engine->setNormalised(index, norm);
}
void Processor::beginUi(int index) { if (index >= 0 && index < (int)hostParams.size() && hostParams[(size_t)index]) hostParams[(size_t)index]->beginChangeGesture(); }
void Processor::endUi(int index) { if (index >= 0 && index < (int)hostParams.size() && hostParams[(size_t)index]) hostParams[(size_t)index]->endChangeGesture(); }

void Processor::getStateInformation(juce::MemoryBlock& dest)
{
    const std::vector<uint8_t>& s = engine->internalSaveState();
    dest.append(s.data(), s.size());
    const float sc = uiScale;
    dest.append("TMUS", 4);
    dest.append(&sc, sizeof sc);
}

void Processor::setStateInformation(const void* data, int size)
{
    const auto* b = static_cast<const uint8_t*>(data);
    if (size > 8 && std::memcmp(b + size - 8, "TMUS", 4) == 0)
    {
        std::memcpy(&uiScale, b + size - 4, sizeof uiScale);
        size -= 8;
    }
    engine->internalLoadState(b, (size_t)size);
    updateHostDisplay(ChangeDetails().withParameterInfoChanged(true));
}

juce::AudioProcessorEditor* Processor::createEditor() { return new Editor(*this); }

} // namespace tmu

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new tmu::Processor(); }
