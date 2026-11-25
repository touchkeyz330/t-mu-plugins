// T-MU desktop editor: draws the plugin's MPC touchscreen skin (TUI.json + its images)
// and makes it work with a mouse: knobs drag, buttons click, menus pop up, meters move.
#pragma once
#include "TmuProcessor.h"
#include <map>
#include <memory>
#include <vector>

namespace tmu {

class Editor;

// one thing on the screen, bound to one engine parameter
class Elem : public juce::Component
{
public:
    Elem(Editor& e, int p) : ed(e), param(p) {}
    virtual bool poll() = 0;                 // returns true if it needs repainting
    void setCondition(int step, int steps, int prm) { condStep = step; condSteps = steps; condParam = prm; }
    bool conditionMet() const;
    Editor& ed;
    int param;
    int condStep = -1, condSteps = 0, condParam = -1;
};

class Editor : public juce::AudioProcessorEditor, public juce::DragAndDropContainer, private juce::Timer
{
public:
    explicit Editor(Processor&);
    ~Editor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

    Processor& proc;
    juce::Image image(const juce::String& name);
    juce::Font font(float height) const;
    float value(int p) const;
    std::string text(int p);
    std::string textFor(int p, float plain);

private:
    void timerCallback() override;
    void build();
    void addEntry(juce::Component& parent, const juce::var& entry, int handle, bool momentary, bool menu, std::vector<Elem*>& list);
    void showTab(int t);

    static constexpr int kW = 1280, kH = 628, kTabH = 40;
    juce::Component canvas;
    struct Tab
    {
        std::unique_ptr<juce::Component> page;
        juce::Image background;
        std::vector<Elem*> elems;
    };
    std::vector<std::unique_ptr<juce::Component>> owned;   // every screen element
    std::vector<Tab> tabs;
    std::vector<std::unique_ptr<juce::TextButton>> tabButtons;
    int current = 0;
    int totalH = kH;
    juce::var local;
    std::map<juce::String, juce::String> resourceNames;
    std::map<juce::String, juce::Image> images;
    juce::Typeface::Ptr typeface;
    struct Look : juce::LookAndFeel_V4
    {
        juce::Typeface::Ptr face;
        void drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
        {
            auto r = b.getLocalBounds().toFloat();
            g.setColour(juce::Colour(b.getToggleState() ? 0xff1d1d24 : 0xff0f0f13).brighter(over || down ? 0.08f : 0.0f));
            g.fillRect(r);
            g.setColour(juce::Colour(0xff2a2a33));
            g.drawVerticalLine(b.getRight() - b.getX() - 1, 0.0f, r.getBottom());
            if (b.getToggleState()) { g.setColour(juce::Colour(0xffff9628)); g.fillRect(r.removeFromTop(3.0f)); }
        }
        juce::Font getTextButtonFont(juce::TextButton&, int h) override
        {
            return face != nullptr ? juce::Font(juce::FontOptions(face).withHeight((float)h * 0.42f)) : juce::Font(juce::FontOptions((float)h * 0.42f, juce::Font::bold));
        }
    };
    Look laf;
};

} // namespace tmu
