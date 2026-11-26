#pragma once
#include <JuceHeader.h>
#include "dsp/LimiterCore.h"

// Build with -DMOBIUS_SPECTRAL_PATH=1 (CMake: -DMOBIUS_SPECTRAL_PATH=ON) to select the legacy
// STFT core instead of the limiter. The legacy code lives in your repo, not in this drop-in:
// see the #if blocks in PluginProcessor.cpp.
#ifndef MOBIUS_SPECTRAL_PATH
 #define MOBIUS_SPECTRAL_PATH 0
#endif

class MobiusCancellerAudioProcessor : public juce::AudioProcessor,
                                      private juce::AsyncUpdater
{
public:
    MobiusCancellerAudioProcessor();
    ~MobiusCancellerAudioProcessor() override { cancelPendingUpdate(); }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi()  const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState apvts;

    float getGainReductionDb() const noexcept { return limiter.getGainReductionDb(); }

    static constexpr int kStateVersion = 2;   // v1 = legacy Mobius spectral parameter set

private:
    void handleAsyncUpdate() override;

    LimiterCore limiter;

    std::atomic<float>* pThreshold = nullptr;
    std::atomic<float>* pAttack    = nullptr;
    std::atomic<float>* pRelease   = nullptr;
    std::atomic<float>* pLookahead = nullptr;
    std::atomic<float>* pMakeup    = nullptr;
    std::atomic<float>* pTruePeak  = nullptr;
    std::atomic<float>* pBypass    = nullptr;

    float lastMakeup = 1.0f;
    std::atomic<int> pendingLatency { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MobiusCancellerAudioProcessor)
};
