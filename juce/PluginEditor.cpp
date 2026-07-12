#include "PluginEditor.h"

namespace {
using juce::Path;
using juce::Point;

constexpr float kPi = juce::MathConstants<float>::pi;

Point<float> pol(float cx, float cy, float r, float svgDeg)
{
    const float a = svgDeg * kPi / 180.0f;
    return { cx + r * std::cos(a), cy + r * std::sin(a) };
}

// Append an arc (sampled) in the SVG angle convention (0 deg = +x, degrees).
void appendArc(Path& p, float cx, float cy, float r,
               float fromDeg, float toDeg, bool startNew)
{
    const float stepDeg = (toDeg >= fromDeg) ? 1.5f : -1.5f;
    const int n = juce::jmax(1, (int) std::ceil(std::abs(toDeg - fromDeg) / std::abs(stepDeg)));
    for (int i = 0; i <= n; ++i)
    {
        const float d = (i == n) ? toDeg : fromDeg + stepDeg * (float) i;
        const auto pt = pol(cx, cy, r, d);
        if (i == 0 && startNew) p.startNewSubPath(pt);
        else                    p.lineTo(pt);
    }
}

Path sectorPath(float cx, float cy, float rO, float rI, float startDeg, float endDeg)
{
    Path p;
    appendArc(p, cx, cy, rO, startDeg, endDeg, true);
    appendArc(p, cx, cy, rI, endDeg, startDeg, false);
    p.closeSubPath();
    return p;
}

// Upper (isTop) or lower semicircle of radius r, flat edge through the centre.
Path halfPath(float cx, float cy, float r, bool isTop)
{
    Path p;
    if (isTop) appendArc(p, cx, cy, r, 180.0f, 360.0f, true);   // through 270 (top)
    else       appendArc(p, cx, cy, r, 0.0f,   180.0f, true);   // through 90  (bottom)
    p.closeSubPath();
    return p;
}

struct AdsrPts { Point<float> p0, pa, pd, ps, pr; };

AdsrPts adsrPts(float ax0, float ayt, float ayb,
                float aPx, float dPx, float sPx, float rPx,
                float a, float d, float s, float r)
{
    const float xa = ax0 + a * aPx;
    const float xd = xa  + d * dPx;
    const float xs = xd  + sPx;
    const float xr = xs  + r * rPx;
    const float ys = ayt + (1.0f - s) * (ayb - ayt);
    return { { ax0, ayb }, { xa, ayt }, { xd, ys }, { xs, ys }, { xr, ayb } };
}

juce::Colour grey(int v) { return juce::Colour::fromRGB((juce::uint8) v, (juce::uint8) v, (juce::uint8) v); }
} // namespace

//==============================================================================
StepGateEditor::StepGateEditor(StepGateAudioProcessor& p)
    : juce::AudioProcessorEditor(p), proc(p), apvts(p.getValueTreeState())
{
    tempoSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    tempoSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 18);
    tempoSlider.setColour(juce::Slider::trackColourId, grey(0x6a));
    tempoSlider.setColour(juce::Slider::backgroundColourId, grey(0x2a));
    tempoSlider.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    tempoSlider.setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
    tempoSlider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(tempoSlider);

    auto styleBox = [this](juce::ComboBox& b)
    {
        b.setColour(juce::ComboBox::backgroundColourId, grey(0x1a));
        b.setColour(juce::ComboBox::textColourId, juce::Colours::white);
        b.setColour(juce::ComboBox::outlineColourId, grey(0x44));
        b.setColour(juce::ComboBox::arrowColourId, grey(0xbb));
        addAndMakeVisible(b);
    };
    for (int i = 0; i < 6; ++i)
        divisionBox.addItem(juce::StringArray { "1/1","1/2","1/4","1/8","1/16","1/32" }[i], i + 1);
    styleBox(divisionBox);
    for (int i = 0; i < 3; ++i)
        divModBox.addItem(juce::StringArray { "Straight","Dotted","Triplet" }[i], i + 1);
    styleBox(divModBox);

    auto styleLabel = [this](juce::Label& l, const juce::String& t)
    {
        l.setText(t, juce::dontSendNotification);
        l.setColour(juce::Label::textColourId, grey(0xbb));
        l.setFont(juce::Font(juce::FontOptions().withHeight(11.0f).withStyle("Bold")));
        l.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(l);
    };
    styleLabel(tempoLabel, "TEMPO");
    styleLabel(divisionLabel, "DIV");

    tempoAtt    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "tempo", tempoSlider);
    divisionAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "division", divisionBox);
    divModAtt   = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "div_mod", divModBox);

    setResizable(false, false);
    setSize((int) (kVW * 1.4f), (int) (kVH * 1.4f));
    startTimerHz(24);
}

StepGateEditor::~StepGateEditor() { stopTimer(); }

//==============================================================================
juce::Point<float> StepGateEditor::toVirtual(juce::Point<int> pt) const
{
    const float s = scale();
    return { (float) pt.x / s, (float) pt.y / s };
}

float StepGateEditor::getParam(const juce::String& id) const
{
    if (auto* a = apvts.getRawParameterValue(id)) return a->load();
    return 0.0f;
}

void StepGateEditor::setParam(const juce::String& id, float value)
{
    if (auto* prm = apvts.getParameter(id))
    {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost(prm->convertTo0to1(value));
        prm->endChangeGesture();
    }
}

int StepGateEditor::hitStepIndex(juce::Point<float> v, float rInner, float rOuter) const
{
    const float dx = v.x - kCX, dy = v.y - kCY;
    const float r  = std::sqrt(dx * dx + dy * dy);
    if (r < rInner || r > rOuter) return -1;
    float ang = std::atan2(dy, dx) * 180.0f / kPi;          // SVG deg
    float norm = ang - kStartDeg;
    norm = std::fmod(std::fmod(norm, 360.0f) + 360.0f, 360.0f);
    return juce::jlimit(0, 15, (int) std::floor(norm / kStepDeg));
}

//==============================================================================
void StepGateEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);

    juce::Graphics::ScopedSaveState save(g);
    g.addTransform(juce::AffineTransform::scale(scale()));

    const int curStep = proc.getCurrentStep();

    // ---- rings ----
    for (int i = 0; i < 16; ++i)
    {
        const int   n         = i + 1;
        const float tieStart  = kStartDeg + (float) i * kStepDeg;
        const float tieEnd    = tieStart + kStepDeg;
        const float stepStart = tieStart + kGapDeg * 0.5f;
        const float stepEnd   = tieEnd   - kGapDeg * 0.5f;

        // tie (outer)
        const bool tieOn = getParam("step_" + juce::String(n) + "_tie") > 0.5f;
        Path tie = sectorPath(kCX, kCY, kTieOuter, kTieInner, tieStart, tieEnd);
        g.setColour(tieOn ? juce::Colours::white : grey(0x3a));
        g.fillPath(tie);
        if (tieOn) { g.setColour(juce::Colours::white.withAlpha(0.40f)); g.strokePath(tie, juce::PathStrokeType(2.0f)); }

        // step (inner)
        const bool stepOn = getParam("step_" + juce::String(n) + "_on") > 0.5f;
        Path step = sectorPath(kCX, kCY, kStepOuter, kStepInner, stepStart, stepEnd);
        g.setColour(stepOn ? juce::Colours::white : grey(0x4a));
        g.fillPath(step);
        if (stepOn) { g.setColour(juce::Colours::white.withAlpha(0.45f)); g.strokePath(step, juce::PathStrokeType(2.5f)); }
        if (n == curStep)   // playing glow
        {
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.strokePath(step, juce::PathStrokeType(3.0f));
        }
    }

    // ---- centre halves ----
    const bool enabled = getParam("enabled") > 0.5f;
    const bool freeRun = getParam("sync_source") > 0.5f;

    Path top = halfPath(kCX, kCY, kHalfR, true);
    g.setColour(enabled ? juce::Colours::white : grey(0x1a));
    g.fillPath(top);
    g.setColour(juce::Colours::black); g.strokePath(top, juce::PathStrokeType(1.5f));

    Path bottom = halfPath(kCX, kCY, kHalfR, false);
    g.setColour(freeRun ? grey(0x2a) : grey(0x1a));
    g.fillPath(bottom);
    g.setColour(juce::Colours::black); g.strokePath(bottom, juce::PathStrokeType(1.5f));

    g.setFont(juce::Font(juce::FontOptions().withHeight(11.0f).withStyle("Bold")));
    g.setColour(enabled ? juce::Colours::black : grey(0xbb));
    g.drawText(enabled ? "ON" : "OFF",
               juce::Rectangle<float>(kCX - 40, kCY - kHalfR * 0.45f - 8, 80, 16),
               juce::Justification::centred);
    g.setColour(grey(0xbb));
    g.drawText(freeRun ? "FREE" : "HOST",
               juce::Rectangle<float>(kCX - 40, kCY + kHalfR * 0.45f - 8, 80, 16),
               juce::Justification::centred);

    // ---- ADSR ----
    g.setColour(grey(0x33));
    g.drawLine(8.0f, 253.0f, 242.0f, 253.0f, 1.0f);

    const auto pts = adsrPts(kAX0, kAYT, kAYB, kAPx, kDPx, kSPx, kRPx,
                             getParam("attack"), getParam("decay"),
                             getParam("sustain"), getParam("release"));
    Path curve;
    curve.startNewSubPath(pts.p0);
    curve.lineTo(pts.pa); curve.lineTo(pts.pd); curve.lineTo(pts.ps); curve.lineTo(pts.pr);
    g.setColour(juce::Colours::white);
    g.strokePath(curve, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    auto handle = [&g](Point<float> c)
    {
        g.setColour(juce::Colours::black);  g.fillEllipse(c.x - 5, c.y - 5, 10, 10);
        g.setColour(juce::Colours::white);  g.drawEllipse(c.x - 5, c.y - 5, 10, 10, 1.5f);
    };
    handle(pts.pa); handle(pts.pd); handle(pts.pr);

    g.setColour(grey(0x66));
    g.setFont(juce::Font(juce::FontOptions().withHeight(9.0f)));
    const float ly = 357.0f;
    g.drawText("A", juce::Rectangle<float>(kAX0 + kAPx * 0.5f - 8, ly - 6, 16, 12), juce::Justification::centred);
    g.drawText("D", juce::Rectangle<float>(kAX0 + kAPx + kDPx * 0.5f - 8, ly - 6, 16, 12), juce::Justification::centred);
    g.drawText("S", juce::Rectangle<float>(kAX0 + kAPx + kDPx + kSPx * 0.5f - 8, ly - 6, 16, 12), juce::Justification::centred);
    g.drawText("R", juce::Rectangle<float>(kAX0 + kAPx + kDPx + kSPx + kRPx * 0.5f - 8, ly - 6, 16, 12), juce::Justification::centred);

    // ---- footer separator ----
    g.setColour(grey(0x33));
    g.drawLine(8.0f, kPedalH + 2.0f, 242.0f, kPedalH + 2.0f, 1.0f);
}

void StepGateEditor::resized()
{
    const float s = scale();
    auto vx = [s](float v) { return juce::roundToInt(v * s); };
    // Footer rows in virtual space (y in [360, 438]).
    const int rowH = vx(kPedalH + 38) - vx(kPedalH + 12);
    tempoLabel.setBounds(vx(12), vx(kPedalH + 10), vx(46), rowH);
    tempoSlider.setBounds(vx(58), vx(kPedalH + 10), vx(180), rowH);
    divisionLabel.setBounds(vx(12), vx(kPedalH + 44), vx(46), rowH);
    divisionBox.setBounds(vx(58), vx(kPedalH + 44), vx(72), rowH);
    divModBox.setBounds(vx(136), vx(kPedalH + 44), vx(102), rowH);
}

//==============================================================================
void StepGateEditor::mouseDown(const juce::MouseEvent& e)
{
    const auto v = toVirtual(e.getPosition());

    // ADSR handles first.
    const auto pts = adsrPts(kAX0, kAYT, kAYB, kAPx, kDPx, kSPx, kRPx,
                             getParam("attack"), getParam("decay"),
                             getParam("sustain"), getParam("release"));
    auto near = [&v](Point<float> p) { return v.getDistanceFrom(p) <= 8.0f; };

    drag = Drag::None;
    if (near(pts.pa))      drag = Drag::Attack;
    else if (near(pts.pd)) drag = Drag::DecaySustain;
    else if (near(pts.pr)) drag = Drag::Release;

    if (drag != Drag::None)
    {
        dragStartVirtual = v;
        dragStartA = getParam("attack");  dragStartD = getParam("decay");
        dragStartS = getParam("sustain"); dragStartR = getParam("release");
        if (drag == Drag::Attack)       { if (auto* a = apvts.getParameter("attack"))  a->beginChangeGesture(); }
        else if (drag == Drag::Release) { if (auto* a = apvts.getParameter("release")) a->beginChangeGesture(); }
        else { if (auto* a = apvts.getParameter("decay"))   a->beginChangeGesture();
               if (auto* a = apvts.getParameter("sustain")) a->beginChangeGesture(); }
        return;
    }

    // Centre halves.
    const float dx = v.x - kCX, dy = v.y - kCY;
    if (std::sqrt(dx * dx + dy * dy) <= kHalfR)
    {
        if (dy < 0) setParam("enabled",     getParam("enabled")     > 0.5f ? 0.0f : 1.0f);
        else        setParam("sync_source", getParam("sync_source") > 0.5f ? 0.0f : 1.0f);
        return;
    }

    // Step ring then tie ring.
    int idx = hitStepIndex(v, kStepInner, kStepOuter);
    if (idx >= 0)
    {
        const auto id = "step_" + juce::String(idx + 1) + "_on";
        setParam(id, getParam(id) > 0.5f ? 0.0f : 1.0f);
        return;
    }
    idx = hitStepIndex(v, kTieInner, kTieOuter);
    if (idx >= 0)
    {
        const auto id = "step_" + juce::String(idx + 1) + "_tie";
        setParam(id, getParam(id) > 0.5f ? 0.0f : 1.0f);
    }
}

void StepGateEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (drag == Drag::None) return;
    const auto v = toVirtual(e.getPosition());
    const float dvx = v.x - dragStartVirtual.x;
    const float dvy = v.y - dragStartVirtual.y;

    auto setRaw = [this](const juce::String& id, float value)
    {
        if (auto* prm = apvts.getParameter(id))
            prm->setValueNotifyingHost(prm->convertTo0to1(value));
    };

    if (drag == Drag::Attack)
        setRaw("attack", juce::jlimit(0.0f, 1.0f, dragStartA + dvx / kAPx));
    else if (drag == Drag::Release)
        setRaw("release", juce::jlimit(0.0f, 1.0f, dragStartR + dvx / kRPx));
    else // DecaySustain
    {
        setRaw("decay",   juce::jlimit(0.0f, 1.0f, dragStartD + dvx / kDPx));
        setRaw("sustain", juce::jlimit(0.0f, 1.0f, dragStartS - dvy / (kAYB - kAYT)));
    }
}

void StepGateEditor::mouseUp(const juce::MouseEvent&)
{
    if (drag == Drag::Attack)       { if (auto* a = apvts.getParameter("attack"))  a->endChangeGesture(); }
    else if (drag == Drag::Release) { if (auto* a = apvts.getParameter("release")) a->endChangeGesture(); }
    else if (drag == Drag::DecaySustain)
    {
        if (auto* a = apvts.getParameter("decay"))   a->endChangeGesture();
        if (auto* a = apvts.getParameter("sustain")) a->endChangeGesture();
    }
    drag = Drag::None;
}

//==============================================================================
void StepGateEditor::timerCallback()
{
    // Repaint to reflect the playing step and any external automation.
    const int s = proc.getCurrentStep();
    repaint();
    lastStep = s;
}
