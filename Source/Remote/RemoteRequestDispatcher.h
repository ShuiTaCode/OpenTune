#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace OpenTune {

class OpenTuneAudioProcessor;

/** Message-thread adapter from the versioned JSON protocol to canonical edits. */
class RemoteRequestDispatcher
{
public:
    explicit RemoteRequestDispatcher(OpenTuneAudioProcessor& processor);
    juce::var dispatch(const juce::var& request);

private:
    OpenTuneAudioProcessor& processor_;
};

} // namespace OpenTune
