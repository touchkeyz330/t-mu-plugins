#include "TmuEditor.h"

namespace tmu {

// ------------------------------------------------------------------ helpers
static juce::Rectangle<int> rectOf(const juce::var& entry)
{
    juce::StringArray t;
    t.addTokens(entry["bounds"]["bounds"].toString(), " ", "");
    if (t.size() < 4) return {};
    return { juce::roundToInt(t[0].getFloatValue()), juce::roundToInt(t[1].getFloatValue()),
             juce::roundToInt(t[2].getFloatValue()), juce::roundToInt(t[3].getFloatValue()) };
}
static juce::Colour colourOf(const juce::String& argb)
{
    return juce::Colour((juce::uint32)argb.getHexValue64());
}
static juce::Justification justOf(const juce::String& j)
{
    if (j.startsWith("left")) return juce::Justification::centredLeft;
    if (j.startsWith("right")) return juce::Justification::centredRight;
    return juce::Justification::centred;
}

bool Elem::conditionMet() const
{
    if (condParam < 0) return true;
    const int n = juce::jmax(2, condSteps);
    return juce::roundToInt(ed.value(condParam) * (float)(n - 1)) == condStep;
}

// a film strip image that follows a parameter (meters, waveform columns, lights) - display only
class StripElem : public Elem
{
public:
    StripElem(Editor& e, int p, juce::Image im, int frames) : Elem(e, p), img(std::move(im)), numFrames(juce::jmax(1, frames))
    { setInterceptsMouseClicks(false, false); }
    int frameNow() const { return param < 0 ? 0 : juce::jlimit(0, numFrames - 1, juce::roundToInt(ed.value(param) * (float)(numFrames - 1))); }
    bool poll() override { const int f = frameNow(); if (f == shown) return false; shown = f; return true; }
    void paint(juce::Graphics& g) override
    {
        if (!img.isValid()) return;
        const int fh = img.getHeight() / numFrames;
        g.drawImage(img, 0, 0, getWidth(), getHeight(), 0, frameNow() * fh, img.getWidth(), fh);
    }
    juce::Image img;
    int numFrames, shown = -1;
};

// a value shown as text
class LabelElem : public Elem
{
public:
    LabelElem(Editor& e, int p, float h, juce::Colour c, juce::Justification j, bool menu)
        : Elem(e, p), height(h), colour(c), just(j), isMenu(menu)
    {
        setInterceptsMouseClicks(menu, false);
        if (menu) setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    bool poll() override
    {
        const std::string t = param >= 0 ? ed.text(param) : std::string();
        if (t == shown) return false;
        shown = t; return true;
    }
    void paint(juce::Graphics& g) override
    {
        g.setColour(colour);
        g.setFont(ed.font(height));
        g.drawFittedText(juce::String::fromUTF8(shown.c_str()), getLocalBounds(), just, 1, 0.7f);
    }
    void mouseDown(const juce::MouseEvent&) override
    {
        if (!isMenu || param < 0) return;
        const mpc::Param& pr = ed.proc.plugin().param(param);
        const int steps = pr.kind == mpc::Param::Kind::Choice ? (int)pr.choices.size()
                        : pr.kind == mpc::Param::Kind::Bool ? 2
                        : pr.kind == mpc::Param::Kind::Int ? (int)(pr.max - pr.min) + 1 : 0;
        if (steps < 2 || steps > 128) { dragging = true; startV = ed.value(param); ed.proc.beginUi(param); return; }
        juce::PopupMenu m;
        const int cur = juce::roundToInt(pr.toPlain(ed.value(param)) - (pr.kind == mpc::Param::Kind::Int ? pr.min : 0.0f));
        for (int k = 0; k < steps; ++k)
        {
            const float plain = pr.kind == mpc::Param::Kind::Int ? pr.min + (float)k : (float)k;
            m.addItem(k + 1, juce::String::fromUTF8(ed.textFor(param, plain).c_str()), true, k == cur);
        }
        juce::Component::SafePointer<LabelElem> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMaximumNumColumns(steps > 24 ? 4 : 1),
                        [safe, steps, &pr](int r) {
                            if (safe == nullptr || r <= 0) return;
                            const float plain = pr.kind == mpc::Param::Kind::Int ? pr.min + (float)(r - 1) : (float)(r - 1);
                            safe->ed.proc.setFromUi(safe->param, pr.toNormalised(plain));
                            juce::ignoreUnused(steps);
                        });
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!dragging) return;
        const float v = juce::jlimit(0.0f, 1.0f, startV - (float)e.getDistanceFromDragStartY() / 300.0f);
        ed.proc.setFromUi(param, v, false);
    }
    void mouseUp(const juce::MouseEvent&) override { if (dragging) { dragging = false; ed.proc.endUi(param); } }

    float height; juce::Colour colour; juce::Justification just; bool isMenu;
    std::string shown = "\x01";
    bool dragging = false; float startV = 0;
};

// a film strip knob: drag up/down (or round and round for a jog wheel), double-click resets
class KnobElem : public StripElem
{
public:
    KnobElem(Editor& e, int p, juce::Image im, int frames, bool jogWheel) : StripElem(e, p, std::move(im), frames), jog(jogWheel)
    {
        setInterceptsMouseClicks(true, false);
        setMouseCursor(jog ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::UpDownResizeCursor);
    }
    void mouseDown(const juce::MouseEvent& e) override
    {
        startV = ed.value(param);
        lastAngle = angleAt(e.position);
        ed.proc.beginUi(param);
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        float v;
        if (jog)
        {   // turn it like a record: the angle you move round the centre is the amount it turns
            const float a = angleAt(e.position);
            float d = a - lastAngle;
            if (d > juce::MathConstants<float>::pi) d -= juce::MathConstants<float>::twoPi;
            if (d < -juce::MathConstants<float>::pi) d += juce::MathConstants<float>::twoPi;
            lastAngle = a;
            v = ed.value(param) + d / juce::MathConstants<float>::twoPi;
            v -= std::floor(v);
        }
        else
        {
            const float range = e.mods.isShiftDown() ? 1200.0f : 240.0f;
            v = juce::jlimit(0.0f, 1.0f, startV - (float)e.getDistanceFromDragStartY() / range);
        }
        ed.proc.setFromUi(param, v, false);
    }
    void mouseUp(const juce::MouseEvent&) override { ed.proc.endUi(param); }
    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (jog) return;
        const mpc::Param& pr = ed.proc.plugin().param(param);
        ed.proc.setFromUi(param, pr.toNormalised(pr.def));
    }
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        const float d = w.deltaY * (jog ? 0.25f : 0.08f);
        float v = ed.value(param) + d;
        if (jog) v -= std::floor(v); else v = juce::jlimit(0.0f, 1.0f, v);
        ed.proc.setFromUi(param, v);
    }
    float angleAt(juce::Point<float> p) const { const auto c = getLocalBounds().toFloat().getCentre(); return std::atan2(p.y - c.y, p.x - c.x); }
    bool jog; float startV = 0, lastAngle = 0;
};

// an image button: toggle, one of a group, or held (momentary). One button can be dragged
// out as a file (Melt Stutter's SAVE WAV -> your DAW).
class ButtonElem : public Elem, private juce::Timer
{
public:
    ButtonElem(Editor& e, int p, juce::Image on, juce::Image off, int id, int group, bool held)
        : Elem(e, p), onImg(std::move(on)), offImg(std::move(off)), buttonId(id), groupSize(group), momentary(held)
    {
        dragOut = p >= 0 && p == ed.proc.plugin().desktopDragParam();
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    bool isOn() const
    {
        if (param < 0) return false;
        const int n = juce::jmax(2, groupSize);
        return juce::roundToInt(ed.value(param) * (float)(n - 1)) == buttonId;
    }
    bool poll() override { const bool o = isOn(); if (o == shown) return false; shown = o; return true; }
    void paint(juce::Graphics& g) override
    {
        const juce::Image& im = isOn() || pressedLook ? onImg : offImg;
        if (im.isValid()) g.drawImage(im, getLocalBounds().toFloat());
    }
    void mouseDown(const juce::MouseEvent&) override
    {
        dragged = false;
        if (param < 0) return;
        if (dragOut) { pressedLook = true; repaint(); return; }   // decide on release: click or drag
        press();
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!dragOut || dragged || e.getDistanceFromDragStart() < 8) return;
        dragged = true; pressedLook = false; repaint();
        const std::string path = ed.proc.plugin().desktopDragFile();
        if (!path.empty())
            ed.performExternalDragDropOfFiles({ juce::String::fromUTF8(path.c_str()) }, false, this);
    }
    void mouseUp(const juce::MouseEvent&) override
    {
        if (param < 0) return;
        if (dragOut)
        {
            pressedLook = false; repaint();
            if (!dragged) { ed.proc.setFromUi(param, 1.0f); startTimer(120); }   // a plain click: SAVE WAV
            return;
        }
        if (momentary) ed.proc.setFromUi(param, 0.0f);
    }
    void press()
    {
        if (momentary) { ed.proc.setFromUi(param, 1.0f); return; }
        if (groupSize <= 1) { ed.proc.setFromUi(param, ed.value(param) >= 0.5f ? 0.0f : 1.0f); return; }
        ed.proc.setFromUi(param, (float)buttonId / (float)(groupSize - 1));
    }
    void timerCallback() override { stopTimer(); ed.proc.setFromUi(param, 0.0f); }

    juce::Image onImg, offImg;
    int buttonId, groupSize;
    bool momentary, dragOut = false, dragged = false, pressedLook = false, shown = false;
};

// a composite component from the skin: a box holding the parts above
class GroupElem : public Elem
{
public:
    GroupElem(Editor& e, int p) : Elem(e, p) { setInterceptsMouseClicks(false, true); }
    bool poll() override { return false; }
};

// ------------------------------------------------------------------ editor
Editor::Editor(Processor& p) : juce::AudioProcessorEditor(p), proc(p)
{
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        resourceNames[BinaryData::originalFilenames[i]] = BinaryData::namedResourceList[i];
    {
        int size = 0;
        auto it = resourceNames.find("Inter-Bold.otf");
        if (it != resourceNames.end())
            if (const char* d = BinaryData::getNamedResource(it->second.toRawUTF8(), size))
                typeface = juce::Typeface::createSystemTypefaceFor(d, (size_t)size);
    }
    laf.face = typeface;
    laf.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff15151a));
    laf.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff2c2c36));
    laf.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff8a8a98));
    laf.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    laf.setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xff1b1b22));
    laf.setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(0xffff9628));
    setLookAndFeel(&laf);
    addAndMakeVisible(canvas);
    build();
    const float sc = proc.uiScale > 0.3f ? proc.uiScale : 0.8f;
    setResizable(true, true);
    setResizeLimits(kW / 2, totalH / 2, kW * 3 / 2, totalH * 3 / 2);
    if (auto* c = getConstrainer()) c->setFixedAspectRatio((double)kW / (double)totalH);
    setSize(juce::roundToInt(kW * sc), juce::roundToInt(totalH * sc));
    startTimerHz(30);
}

Editor::~Editor()
{
    stopTimer();
    setLookAndFeel(nullptr);
}

juce::Image Editor::image(const juce::String& name)
{
    if (name.isEmpty()) return {};
    auto it = images.find(name);
    if (it != images.end()) return it->second;
    juce::Image im;
    auto r = resourceNames.find(name);
    int size = 0;
    if (r != resourceNames.end())
        if (const char* d = BinaryData::getNamedResource(r->second.toRawUTF8(), size))
            im = juce::ImageFileFormat::loadFrom(d, (size_t)size);
    images[name] = im;
    return im;
}

juce::Font Editor::font(float height) const
{
    if (typeface != nullptr) return juce::Font(juce::FontOptions(typeface).withHeight(height * 0.95f));
    return juce::Font(juce::FontOptions(height * 0.95f, juce::Font::bold));
}

float Editor::value(int p) const
{
    auto& pl = proc.plugin();
    return (p >= 0 && p < pl.numParams()) ? pl.getNormalised(p) : 0.0f;
}
std::string Editor::text(int p)
{
    auto& pl = proc.plugin();
    return (p >= 0 && p < pl.numParams()) ? pl.displayText(p) : std::string();
}
std::string Editor::textFor(int p, float plain)
{
    auto& pl = proc.plugin();
    std::string s;
    if (!pl.formatValue(p, plain, s)) s = pl.param(p).display(plain);
    return s;
}

void Editor::build()
{
    const juce::var tui = proc.skin();
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        const juce::var defs = tui["pageData"]["componentDefinitions"]["localComponentDefinitions"];
        for (int i = 0; i < defs.size(); ++i) o->setProperty(juce::Identifier(defs[i]["key"].toString()), defs[i]["value"]);
        local = juce::var(o.get());
    }
    const juce::var tabsVar = tui["pageData"]["tabs"];
    const int nTabs = tabsVar.size();
    totalH = kH + (nTabs > 1 ? kTabH : 0);
    canvas.setBounds(0, 0, kW, totalH);
    for (int t = 0; t < nTabs; ++t)
    {
        Tab tab;
        tab.page = std::make_unique<juce::Component>();
        tab.page->setBounds(0, 0, kW, kH);
        tab.page->setInterceptsMouseClicks(false, true);
        tab.background = image(tabsVar[t]["componentDefinition"]["backgroundData"]["unfocussed"]["image"].toString());
        const juce::var comps = tabsVar[t]["componentDefinition"]["componentsData"];
        for (int k = 0; k < comps.size(); ++k) addEntry(*tab.page, comps[k], -1, false, false, tab.elems);
        canvas.addChildComponent(*tab.page);
        tabs.push_back(std::move(tab));
        if (nTabs > 1)
        {
            auto b = std::make_unique<juce::TextButton>(tabsVar[t]["tabName"].toString());
            b->setClickingTogglesState(false);
            b->setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
            const int w = kW / nTabs;
            b->setBounds(t * w, kH, t == nTabs - 1 ? kW - t * w : w, kTabH);
            b->onClick = [this, t] { showTab(t); };
            canvas.addAndMakeVisible(*b);
            tabButtons.push_back(std::move(b));
        }
    }
    showTab(0);
}

void Editor::addEntry(juce::Component& parent, const juce::var& entry, int handle, bool momentary, bool menu, std::vector<Elem*>& list)
{
    const juce::var cd = entry["componentData"];
    const juce::String type = cd["type"].toString();
    const juce::var data = cd["data"];
    if (entry["bounds"]["whenVisible"].toString() == "WhenFocussed" || type == "Focus") return;

    const juce::var map = entry["handle remapping"]["map"];
    if (map.isArray() && map.size() > 0)
    {
        const juce::String v = map[0]["value"].toString();
        handle = v.fromLastOccurrenceOf(" ", false, false).getIntValue();
        momentary = v.startsWith("ResetOnGestureEnd");
    }
    const juce::Rectangle<int> r = rectOf(entry);
    std::unique_ptr<Elem> el;

    if (local.hasProperty(type))
    {
        const juce::var comp = local[juce::Identifier(type)];
        const juce::var acts = comp["actions"];
        bool isMenu = false;
        for (int k = 0; k < acts.size(); ++k)
            if (acts[k]["handler"].toString() == "Show Overlay" && acts[k]["additionalData"].toString().containsIgnoreCase("menu")) isMenu = true;
        auto g = std::make_unique<GroupElem>(*this, handle);
        g->setBounds(r);
        const juce::var kids = comp["componentsData"];
        std::vector<Elem*> inner;
        for (int k = 0; k < kids.size(); ++k) addEntry(*g, kids[k], handle, momentary, isMenu, inner);
        el = std::move(g);
        for (Elem* e : inner) list.push_back(e);
    }
    else if (type == "Knob")
    {
        const juce::String strip = data["filmStrip"].toString();
        const bool jog = strip.containsIgnoreCase("jog") || strip.containsIgnoreCase("platter");
        el = std::make_unique<KnobElem>(*this, handle, image(strip), (int)data["numFrames"], jog);
    }
    else if (type == "Button")
        el = std::make_unique<ButtonElem>(*this, handle, image(data["onImage"].toString()), image(data["offImage"].toString()),
                                          (int)data["buttonId"], (int)data["numButtonsInGroup"], momentary);
    else if (type == "Image")
    {
        if (data["imageType"].toString() == "FilmStrip")
            el = std::make_unique<StripElem>(*this, handle, image(data["image"].toString()), (int)data["numFrames"]);
        else
            el = std::make_unique<StripElem>(*this, -1, image(data["image"].toString()), 1);
    }
    else if (type == "Label")
    {
        const juce::var st = data["textStyle"];
        el = std::make_unique<LabelElem>(*this, handle, (float)(double)st["font"]["height"], colourOf(st["colour"].toString()),
                                         justOf(st["justification"].toString()), menu);
    }
    if (!el) return;

    // "only show this while parameter X is at step S of N"
    const juce::var inv = entry["bounds"]["additionalInvalidatingHandles"];
    for (int k = 0; k < inv.size(); ++k)
    {
        juce::StringArray t;
        t.addTokens(inv[k].toString(), "/", "");
        if (t.size() >= 4 && t[0] == "IndexedEnabling")
            el->setCondition(t[1].getIntValue(), t[2].getIntValue(), t[3].fromLastOccurrenceOf(" ", false, false).getIntValue());
    }
    if (!dynamic_cast<GroupElem*>(el.get())) el->setBounds(r);
    parent.addAndMakeVisible(*el);
    list.push_back(el.get());
    owned.push_back(std::move(el));
}

void Editor::showTab(int t)
{
    current = juce::jlimit(0, (int)tabs.size() - 1, t);
    for (size_t i = 0; i < tabs.size(); ++i) tabs[i].page->setVisible((int)i == current);
    for (size_t i = 0; i < tabButtons.size(); ++i) tabButtons[i]->setToggleState((int)i == current, juce::dontSendNotification);
    for (Elem* e : tabs[(size_t)current].elems) { e->poll(); e->setVisible(e->conditionMet()); e->repaint(); }
    repaint();
}

void Editor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0e0e12));
    if (tabs.empty()) return;
    g.addTransform(canvas.getTransform());
    const juce::Image& bg = tabs[(size_t)current].background;
    if (bg.isValid()) g.drawImage(bg, juce::Rectangle<float>(0, 0, (float)kW, (float)kH));
}

void Editor::resized()
{
    const float sc = (float)getWidth() / (float)kW;
    canvas.setTransform(juce::AffineTransform::scale(sc));
    proc.uiScale = sc;
}

void Editor::timerCallback()
{
    if (tabs.empty()) return;
    for (Elem* e : tabs[(size_t)current].elems)
    {
        const bool vis = e->conditionMet();
        if (vis != e->isVisible()) e->setVisible(vis);
        if (vis && e->poll()) e->repaint();
    }
}

} // namespace tmu
