#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace {
constexpr int kVersionHint = 1;

juce::String stepId(int step1based, const char* suffix)
{
    return "step_" + juce::String(step1based) + "_" + suffix;
}
} // namespace

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
StepGateAudioProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    // Sync Source: 0 = Host Sync, 1 = Free Run  (matches .ttl scalePoints)
    layout.add(std::make_unique<AudioParameterChoice>(
        ParameterID { "sync_source", kVersionHint }, "Sync Source",
        StringArray { "Host Sync", "Free Run" }, 0));

    // Tempo (bpm), free-run / fallback.
    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "tempo", kVersionHint }, "Tempo",
        NormalisableRange<float> { 20.0f, 300.0f, 0.0f }, 120.0f,
        AudioParameterFloatAttributes {}.withLabel("BPM")));

    // Division: 0..5 -> 1/1,1/2,1/4,1/8,1/16,1/32, default 1/16 (index 4).
    layout.add(std::make_unique<AudioParameterChoice>(
        ParameterID { "division", kVersionHint }, "Division",
        StringArray { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" }, 4));

    // Division feel: straight (x1), dotted (x1.5), triplet (x2/3).
    // Matches the div_mod LV2 port appended in the .ttl.
    layout.add(std::make_unique<AudioParameterChoice>(
        ParameterID { "div_mod", kVersionHint }, "Division Feel",
        StringArray { "Straight", "Dotted", "Triplet" }, 0));

    // 16 step on/tie toggles. Step 1 on by default; all ties on by default.
    for (int s = 1; s <= STEPGATE_NUM_STEPS; ++s)
    {
        layout.add(std::make_unique<AudioParameterBool>(
            ParameterID { stepId(s, "on"), kVersionHint },
            "Step " + juce::String(s) + " On", s == 1));
        layout.add(std::make_unique<AudioParameterBool>(
            ParameterID { stepId(s, "tie"), kVersionHint },
            "Step " + juce::String(s) + " Tie", true));
    }

    // lv2:enabled equivalent (1 = active, 0 = transparent pass-through).
    layout.add(std::make_unique<AudioParameterBool>(
        ParameterID { "enabled", kVersionHint }, "Enabled", true));

    // ADSR, fractions of step length.
    const NormalisableRange<float> unit { 0.0f, 1.0f, 0.0f };
    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "attack",  kVersionHint }, "Attack",  unit, 0.0f));
    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "decay",   kVersionHint }, "Decay",   unit, 0.0f));
    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "sustain", kVersionHint }, "Sustain", unit, 1.0f));
    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "release", kVersionHint }, "Release", unit, 0.5f));

    return layout;
}

//==============================================================================
StepGateAudioProcessor::StepGateAudioProcessor()
    : AudioProcessor(BusesProperties()
          .withInput("Input",   juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMS", createLayout())
{
    cacheParameterPointers();
}

StepGateAudioProcessor::~StepGateAudioProcessor() = default;

void StepGateAudioProcessor::cacheParameterPointers()
{
    pSync    = apvts.getRawParameterValue("sync_source");
    pTempo   = apvts.getRawParameterValue("tempo");
    pDiv     = apvts.getRawParameterValue("division");
    pDivMod  = apvts.getRawParameterValue("div_mod");
    pEnabled = apvts.getRawParameterValue("enabled");
    pAttack  = apvts.getRawParameterValue("attack");
    pDecay   = apvts.getRawParameterValue("decay");
    pSustain = apvts.getRawParameterValue("sustain");
    pRelease = apvts.getRawParameterValue("release");
    for (size_t k = 0; k < STEPGATE_NUM_STEPS; ++k)
    {
        pStepOn[k]  = apvts.getRawParameterValue(stepId((int) k + 1, "on"));
        pStepTie[k] = apvts.getRawParameterValue(stepId((int) k + 1, "tie"));
    }
}

//==============================================================================
void StepGateAudioProcessor::prepareToPlay(double sampleRate, int)
{
    // (Re)create the DSP core only when the sample rate changes; allocate
    // here, off the audio thread.
    if (!dsp || !juce::exactlyEqual(sampleRate, preparedRate))
    {
        dsp.reset(stepgate_dsp_new(sampleRate));
        preparedRate = sampleRate;
    }
    if (dsp)
        stepgate_dsp_reset(dsp.get());
}

bool StepGateAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    const auto in = layouts.getMainInputChannelSet();
    return in == out || in.isDisabled();
}

void StepGateAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    if (!dsp)
        return;

    // ---- host transport -> core ----
    bool   haveBpm = false, haveBeat = false;
    double bpm = 120.0, beat = 0.0;
    bool   playing = true;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm())          { bpm = *b;  haveBpm  = true; }
            if (auto q = pos->getPpqPosition())  { beat = *q; haveBeat = true; }
            playing = pos->getIsPlaying();
        }
    }
    stepgate_dsp_update_position(dsp.get(),
                                 haveBpm,  bpm,
                                 haveBeat, beat,
                                 true,     playing ? 1.0 : 0.0,
                                 false,    0.0);

    // ---- parameters -> core (raw values; clamping happens in the core) ----
    StepGateParams p;
    p.sync_source = pSync->load();
    p.tempo       = pTempo->load();
    p.division    = pDiv->load();
    p.division_mod = pDivMod->load();
    p.enabled     = pEnabled->load();
    p.attack      = pAttack->load();
    p.decay       = pDecay->load();
    p.sustain     = pSustain->load();
    p.release     = pRelease->load();
    for (size_t k = 0; k < STEPGATE_NUM_STEPS; ++k)
    {
        p.step_on[k]  = pStepOn[k]->load();
        p.step_tie[k] = pStepTie[k]->load();
    }

    // ---- process in place. Same gate applied to L and R, exactly as LV2. ----
    float* L = buffer.getNumChannels() > 0 ? buffer.getWritePointer(0) : nullptr;
    float* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
    const int step = stepgate_dsp_process(dsp.get(), &p, L, R, L, R, (uint32_t) n);
    currentStep.store(step);

    // Mirror onto any further output channels (e.g. >2-channel layouts).
    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.copyFrom(ch, 0, buffer, 0, 0, n);
}

//==============================================================================
juce::AudioProcessorEditor* StepGateAudioProcessor::createEditor()
{
    return new StepGateEditor(*this);
}

void StepGateAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void StepGateAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(apvts.state.getType()))
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StepGateAudioProcessor();
}
