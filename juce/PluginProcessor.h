/*
 * Parkinsound Step Gate - JUCE wrapper (VST3 / AU / Standalone).
 *
 * Thin wrapper around the shared host-agnostic DSP core
 * (../src/stepgate_dsp.{c,h}). All signal processing lives in the core;
 * this class only maps the AudioProcessorValueTreeState parameters and
 * the host transport onto the core's parameter struct / transport update,
 * then calls stepgate_dsp_process(). Parameter IDs are the same symbols as
 * the LV2 .ttl, so state stays consistent across formats.
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <array>

#include "stepgate_dsp.h"

class StepGateAudioProcessor : public juce::AudioProcessor
{
public:
    StepGateAudioProcessor();
    ~StepGateAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    using juce::AudioProcessor::processBlock;   // float-only; don't hide the double overload
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getValueTreeState() { return apvts; }

    /* 1-based current step, for the editor's monitor display. */
    int getCurrentStep() const { return currentStep.load(); }

    /* Effective pattern length (16 in the legacy fixed mode, bar-derived
     * in the bar modes), for the editor's adaptive ring. */
    int getActiveSteps() const { return activeSteps.load(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void cacheParameterPointers();

    juce::AudioProcessorValueTreeState apvts;

    /* Cached atomic raw-value pointers (resolved once in the ctor). */
    std::atomic<float>* pSync   = nullptr;
    std::atomic<float>* pTempo  = nullptr;
    std::atomic<float>* pDiv    = nullptr;
    std::atomic<float>* pDivMod = nullptr;
    std::atomic<float>* pPatternMode = nullptr;
    std::atomic<float>* pMeterSource = nullptr;
    std::atomic<float>* pMeterNum    = nullptr;
    std::atomic<float>* pMeterDenom  = nullptr;
    std::atomic<float>* pEnabled = nullptr;
    std::atomic<float>* pAttack  = nullptr;
    std::atomic<float>* pDecay   = nullptr;
    std::atomic<float>* pSustain = nullptr;
    std::atomic<float>* pRelease = nullptr;
    std::array<std::atomic<float>*, STEPGATE_NUM_STEPS> pStepOn{};
    std::array<std::atomic<float>*, STEPGATE_NUM_STEPS> pStepTie{};

    struct DspDeleter { void operator()(StepGateDsp* d) const { stepgate_dsp_free(d); } };
    std::unique_ptr<StepGateDsp, DspDeleter> dsp;
    double preparedRate = 0.0;

    std::atomic<int> currentStep { 1 };
    std::atomic<int> activeSteps { STEPGATE_NUM_STEPS };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StepGateAudioProcessor)
};
