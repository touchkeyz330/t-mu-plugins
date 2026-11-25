// T-MU desktop bridge: runs an MPC-SDK plugin engine as a JUCE plugin (VST3 / AU / AAX).
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"
#include "mpc_plugin.h"
#include <memory>
#include <vector>

namespace tmu {

class Processor;

// One host-visible parameter, backed by the engine's own parameter value.
class HostParam : public juce::AudioProcessorParameterWithID
{
public:
    HostParam(mpc::Plugin& p, int index);
    float getValue() const override { return plugin.getNormalised(index); }
    void setValue(float v) override { plugin.setNormalised(index, v); }
    float getDefaultValue() const override;
    juce::String getText(float v, int maxLen) const override;
    float getValueForText(const juce::String& text) const override;
    int getNumSteps() const override;
    bool isDiscrete() const override;
    bool isBoolean() const override;
    juce::String getLabel() const override { return plugin.param(index).unit; }
    juce::StringArray getAllValueStrings() const override;

    mpc::Plugin& plugin;
    const int index;
};

class Processor : public juce::AudioProcessor, private juce::Timer
{
public:
    Processor();
    ~Processor() override;

    // ---- juce::AudioProcessor
    const juce::String getName() const override { return JucePlugin_Name; }
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return engine->info().isInstrument; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& dest) override;
    void setStateInformation(const void* data, int size) override;

    // ---- for the editor
    mpc::Plugin& plugin() { return *engine; }
    const juce::var& skin() const { return tui; }
    // the screen changed a value: tell the host (with a gesture) or set it directly
    void setFromUi(int index, float norm, bool gesture = true);
    void beginUi(int index);
    void endUi(int index);
    float uiScale = 0.0f;   // remembered editor size (0 = default)

private:
    explicit Processor(std::unique_ptr<mpc::Plugin> engine);
    void timerCallback() override;
    static BusesProperties busesFor(bool instrument);

    std::unique_ptr<mpc::Plugin> engine;
    juce::var tui;
    std::vector<HostParam*> hostParams;                    // engine index -> host parameter (nullptr = display-only)
    std::unique_ptr<std::atomic<uint8_t>[]> written;       // engine wrote this parameter itself
    juce::AudioBuffer<float> inCopy, scratch;
    std::vector<mpc::MidiEvent> midiEvents;
    double sr = 44100.0;
    int maxBlock = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Processor)
};

} // namespace tmu
