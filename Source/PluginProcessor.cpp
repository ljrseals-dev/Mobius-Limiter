#include "PluginProcessor.h"

#if MOBIUS_SPECTRAL_PATH
 // A/B build: this drop-in does not contain the legacy STFT processor. Include your repo's
 // spectral header here and route prepare/process/latency to it (see README, "A/B build").
 #error "MOBIUS_SPECTRAL_PATH=1 needs the legacy STFT code wired in: edit PluginProcessor.cpp"
#endif

using APF = juce::AudioParameterFloat;
using Attr = juce::AudioParameterFloatAttributes;

juce::AudioProcessorValueTreeState::ParameterLayout
MobiusCancellerAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;

    p.push_back (std::make_unique<APF> (juce::ParameterID { "threshold", 2 }, "Threshold",
        juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -0.1f, Attr().withLabel ("dBFS")));
    p.push_back (std::make_unique<APF> (juce::ParameterID { "attack", 2 }, "Attack",
        juce::NormalisableRange<float> (0.1f, 100.0f, 0.1f, 0.4f), 1.0f, Attr().withLabel ("ms")));
    p.push_back (std::make_unique<APF> (juce::ParameterID { "release", 2 }, "Release",
        juce::NormalisableRange<float> (1.0f, 1000.0f, 1.0f, 0.4f), 50.0f, Attr().withLabel ("ms")));

    // Look-ahead and true-peak change reported latency, so they are not host-automatable.
    p.push_back (std::make_unique<APF> (juce::ParameterID { "lookahead", 2 }, "Lookahead",
        juce::NormalisableRange<float> (0.1f, 10.0f, 0.1f), 1.5f,
        Attr().withLabel ("ms").withAutomatable (false)));
    p.push_back (std::make_unique<APF> (juce::ParameterID { "makeup", 2 }, "Makeup",
        juce::NormalisableRange<float> (0.0f, 24.0f, 0.1f), 0.0f, Attr().withLabel ("dB")));
    p.push_back (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "truepeak", 2 }, "True Peak",
        true, juce::AudioParameterBoolAttributes().withAutomatable (false)));
    p.push_back (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "bypass", 2 }, "Bypass", false));

    return { p.begin(), p.end() };
}

MobiusCancellerAudioProcessor::MobiusCancellerAudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    pThreshold = apvts.getRawParameterValue ("threshold");
    pAttack    = apvts.getRawParameterValue ("attack");
    pRelease   = apvts.getRawParameterValue ("release");
    pLookahead = apvts.getRawParameterValue ("lookahead");
    pMakeup    = apvts.getRawParameterValue ("makeup");
    pTruePeak  = apvts.getRawParameterValue ("truepeak");
    pBypass    = apvts.getRawParameterValue ("bypass");
}

void MobiusCancellerAudioProcessor::prepareToPlay (double sampleRate, int)
{
    limiter.prepare (sampleRate, juce::jmax (1, getTotalNumOutputChannels()));
    limiter.configure (pLookahead->load(), pTruePeak->load() > 0.5f);
    limiter.setParameters (pThreshold->load(), pAttack->load(), pRelease->load());

    lastMakeup = (pBypass->load() > 0.5f) ? 1.0f : juce::Decibels::decibelsToGain (pMakeup->load());
    setLatencySamples (limiter.getLatencySamples());
}

void MobiusCancellerAudioProcessor::releaseResources() { limiter.reset(); }

bool MobiusCancellerAudioProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto& out = l.getMainOutputChannelSet();
    return l.getMainInputChannelSet() == out
        && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void MobiusCancellerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n      = buffer.getNumSamples();
    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    for (int ch = numIn; ch < numOut; ++ch) buffer.clear (ch, 0, n);

    const bool bypass = pBypass->load() > 0.5f;

    // Latency-affecting settings. Applied on a block boundary; clears limiter state (brief gap)
    // and asks the message thread to publish the new latency to the host.
    if (limiter.configure (pLookahead->load(), pTruePeak->load() > 0.5f))
    {
        pendingLatency.store (limiter.getLatencySamples());
        triggerAsyncUpdate();
    }

    limiter.setBypassed (bypass);
    limiter.setParameters (pThreshold->load(), pAttack->load(), pRelease->load());
    limiter.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), n);

    // Makeup: per-block linear ramp (no zipper). Bypass ramps it to unity instead of switching it off.
    const float target = bypass ? 1.0f : juce::Decibels::decibelsToGain (pMakeup->load());
    for (int ch = 0; ch < numOut; ++ch)
        buffer.applyGainRamp (ch, 0, n, lastMakeup, target);
    lastMakeup = target;
}

void MobiusCancellerAudioProcessor::handleAsyncUpdate()
{
    setLatencySamples (pendingLatency.load());
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withLatencyChanged (true));
}

juce::AudioProcessorEditor* MobiusCancellerAudioProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void MobiusCancellerAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("mobiusStateVersion", kStateVersion, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void MobiusCancellerAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        if (! xml->hasTagName (apvts.state.getType())) return;
        // Pre-limiter presets (no version) use the old Mobius parameter IDs: ignore them, keep defaults.
        if (xml->getIntAttribute ("mobiusStateVersion", 1) < kStateVersion) return;
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MobiusCancellerAudioProcessor(); }
