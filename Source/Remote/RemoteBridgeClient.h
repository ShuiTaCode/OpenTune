#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <functional>
#include <memory>

namespace OpenTune {

/**
 * Local authenticated control-plane client for REAPER AI.
 *
 * All file and pipe I/O runs on this object's worker thread. Requests are
 * marshalled to JUCE's message thread before the dispatcher is called.
 */
class RemoteBridgeClient final : private juce::Thread
{
public:
    using Dispatcher = std::function<juce::var(const juce::var&)>;
    using PairingCodeProvider = std::function<int()>;

    RemoteBridgeClient(PairingCodeProvider pairingCodeProvider, Dispatcher dispatcher);
    ~RemoteBridgeClient() override;

    void start();
    void stop();
    const juce::String& instanceId() const noexcept { return instanceId_; }

private:
    struct DispatchState;

    void run() override;
    bool runConnection();
    juce::File bridgeConfigFile() const;
    void diagnose(const juce::String& message);
    int currentPairingCode() const;

    PairingCodeProvider pairingCodeProvider_;
    const juce::String instanceId_;
    Dispatcher dispatcher_;
    std::shared_ptr<std::atomic<bool>> lifetime_;
    std::atomic<bool> stopping_{false};
    juce::String lastDiagnostic_;

#if JUCE_WINDOWS
    void closePipe();
    juce::CriticalSection pipeLock_;
    void* pipe_ = nullptr;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RemoteBridgeClient)
};

} // namespace OpenTune
