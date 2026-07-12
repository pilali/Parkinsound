/*
 * Parkinsound Step Gate - custom JUCE editor.
 *
 * Native reproduction of the MOD (modgui) radial UI:
 *   - outer tie ring   (16 segments, r 85..95)
 *   - inner step ring  (16 segments, r 50..80, 2 deg gaps), playing-step glow
 *   - two centre halves (top = enabled, bottom = sync source)
 *   - ADSR curve with three draggable handles (A, D+S, R)
 * Plus a footer (not in the modgui custom icon) exposing the Tempo and
 * Division parameters, which on a MOD device were generic knobs.
 *
 * Everything is vector-drawn (sharp at any size/Retina). The radial widgets
 * are painted/handled by this component directly; Tempo/Division use child
 * Slider/ComboBox with APVTS attachments.
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class StepGateEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    explicit StepGateEditor(StepGateAudioProcessor&);
    ~StepGateEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    // ---- helpers ----
    float scale() const { return (float) getWidth() / kVW; }
    juce::Point<float> toVirtual(juce::Point<int> p) const;
    float getParam(const juce::String& id) const;
    void  setParam(const juce::String& id, float value);

    int hitStepIndex(juce::Point<float> v, float rInner, float rOuter) const; // -1 if none
    enum class Drag { None, Attack, DecaySustain, Release };

    StepGateAudioProcessor& proc;
    juce::AudioProcessorValueTreeState& apvts;

    juce::Slider   tempoSlider;
    juce::ComboBox divisionBox, divModBox;
    juce::Label    tempoLabel, divisionLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   tempoAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> divisionAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> divModAtt;

    Drag drag = Drag::None;
    juce::Point<float> dragStartVirtual;
    float dragStartA = 0, dragStartD = 0, dragStartS = 0, dragStartR = 0;
    juce::RangedAudioParameter* dragParamA = nullptr; // for gesture begin/end

    int lastStep = -1;

    // ---- virtual geometry (matches the modgui 250x... viewBox) ----
    static constexpr float kVW        = 250.0f;
    static constexpr float kPedalH    = 360.0f;
    static constexpr float kFooterH   = 78.0f;
    static constexpr float kVH        = kPedalH + kFooterH;
    static constexpr float kCX        = 125.0f, kCY = 125.0f;
    static constexpr float kStepOuter = 80.0f, kStepInner = 50.0f;
    static constexpr float kTieOuter  = 95.0f, kTieInner  = 85.0f;
    static constexpr float kHalfR     = 45.0f;
    static constexpr float kGapDeg    = 2.0f;
    static constexpr float kStepDeg   = 22.5f;
    static constexpr float kStartDeg  = -90.0f;
    static constexpr float kAX0 = 10.0f, kAYT = 258.0f, kAYB = 348.0f;
    static constexpr float kAPx = 50.0f, kDPx = 44.0f, kSPx = 96.0f, kRPx = 40.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StepGateEditor)
};
