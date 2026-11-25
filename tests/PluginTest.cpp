// Test harness (not shipped): runs a T-MU plugin's processor and editor outside a DAW.
//   tmu_test_<Plugin> <out.png>      renders the editor, plays audio through it, checks state save/load
#include "TmuProcessor.h"
#include "TmuEditor.h"
#include <cstdio>

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto proc = std::unique_ptr<juce::AudioProcessor>(createPluginFilter());
    auto& tp = dynamic_cast<tmu::Processor&>(*proc);
    std::printf("plugin: %s  host params: %d of %d engine params\n", proc->getName().toRawUTF8(),
                (int)proc->getParameters().size(), tp.plugin().numParams());
    proc->setPlayConfigDetails(tp.plugin().info().numInputs > 0 ? 2 : 0, 2, 48000.0, 512);
    proc->prepareToPlay(48000.0, 512);

    juce::AudioBuffer<float> buf(2, 512);
    juce::MidiBuffer midi;
    double t = 0, peak = 0;
    bool finite = true;
    for (int b = 0; b < 200; ++b)
    {
        for (int i = 0; i < 512; ++i, t += 1.0 / 48000.0)
        {
            const float s = 0.3f * (float)std::sin(2 * juce::MathConstants<double>::pi * 220.0 * t);
            buf.setSample(0, i, s); buf.setSample(1, i, s);
        }
        if (b == 10 && tp.plugin().info().isInstrument) midi.addEvent(juce::MidiMessage::noteOn(1, 37, (juce::uint8)100), 0);
        proc->processBlock(buf, midi);
        midi.clear();
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 512; ++i)
            {
                const float v = buf.getSample(c, i);
                if (!std::isfinite(v)) finite = false;
                peak = std::max(peak, (double)std::fabs(v));
            }
    }
    std::printf("audio: %s, peak %.3f\n", finite ? "all samples ok" : "BAD SAMPLES", peak);

    // state round trip
    juce::MemoryBlock st;
    if (auto* p0 = proc->getParameters()[2]) p0->setValueNotifyingHost(0.73f);
    proc->getStateInformation(st);
    const float before = proc->getParameters()[2]->getValue();
    proc->getParameters()[2]->setValueNotifyingHost(0.1f);
    proc->setStateInformation(st.getData(), (int)st.getSize());
    std::printf("state: %d bytes, restored %s\n", (int)st.getSize(), std::fabs(proc->getParameters()[2]->getValue() - before) < 1e-4f ? "ok" : "WRONG");

    // param text
    for (int i = 0; i < juce::jmin(6, (int)proc->getParameters().size()); ++i)
    {
        auto* p = proc->getParameters()[i];
        std::printf("  %-14s = %s\n", p->getName(20).toRawUTF8(), p->getCurrentValueAsText().toRawUTF8());
    }

    // editor: render every tab
    std::unique_ptr<juce::AudioProcessorEditor> ed(proc->createEditor());
    ed->setSize(1280, ed->getHeight() * 1280 / juce::jmax(1, ed->getWidth()));
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    if (argc > 1)
    {
        int tabIdx = 0;
        std::function<void(juce::Component&)> findTabs;
        std::vector<juce::Button*> tabButtons;
        findTabs = [&](juce::Component& c) {
            for (auto* k : c.getChildren()) { if (auto* b = dynamic_cast<juce::TextButton*>(k)) tabButtons.push_back(b); findTabs(*k); }
        };
        findTabs(*ed);
        const int n = juce::jmax(1, (int)tabButtons.size());
        for (tabIdx = 0; tabIdx < n; ++tabIdx)
        {
            if (!tabButtons.empty()) tabButtons[(size_t)tabIdx]->triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
            juce::Image im = ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f);
            juce::File f(juce::String(argv[1]).replace("%d", juce::String(tabIdx)));
            f.deleteFile();
            juce::FileOutputStream os(f);
            juce::PNGImageFormat().writeImageToStream(im, os);
            std::printf("editor tab %d -> %s (%dx%d)\n", tabIdx, f.getFullPathName().toRawUTF8(), im.getWidth(), im.getHeight());
        }
    }
    ed.reset();
    proc->releaseResources();
    return finite ? 0 : 1;
}
