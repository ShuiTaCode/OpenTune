#include "RemoteBridgeClient.h"
#include "../Utils/AppLogger.h"

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace OpenTune {
namespace {

constexpr int kProtocolVersion = 1;
constexpr int kMaxMessageBytes = 1024 * 1024;

juce::var makeErrorResponse(const juce::String& requestId,
                            const juce::String& code,
                            const juce::String& message)
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("type", "response");
    object->setProperty("protocolVersion", kProtocolVersion);
    object->setProperty("requestId", requestId);
    object->setProperty("ok", false);
    object->setProperty("code", code);
    object->setProperty("message", message);
    return juce::var(object.release());
}

#if JUCE_WINDOWS
bool writeAll(HANDLE pipe, const juce::String& text)
{
    auto utf8 = text.toRawUTF8();
    auto remaining = static_cast<DWORD>(std::strlen(utf8));
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, utf8, remaining, &written, nullptr) || written == 0)
            return false;
        utf8 += written;
        remaining -= written;
    }
    return true;
}
#endif

} // namespace

struct RemoteBridgeClient::DispatchState
{
    juce::WaitableEvent completed;
    juce::var response;
};

RemoteBridgeClient::RemoteBridgeClient(PairingCodeProvider pairingCodeProvider, Dispatcher dispatcher)
    : juce::Thread("OpenTune Remote Bridge"),
      pairingCodeProvider_(std::move(pairingCodeProvider)),
      instanceId_(juce::Uuid().toDashedString()),
      dispatcher_(std::move(dispatcher)),
      lifetime_(std::make_shared<std::atomic<bool>>(true))
{
}

int RemoteBridgeClient::currentPairingCode() const
{
    const int code = pairingCodeProvider_ ? pairingCodeProvider_() : 0;
    return juce::jlimit(1, 999999, code);
}

RemoteBridgeClient::~RemoteBridgeClient()
{
    stop();
    lifetime_->store(false, std::memory_order_release);
}

void RemoteBridgeClient::start()
{
    if (isThreadRunning())
        return;
    stopping_.store(false, std::memory_order_release);
    lifetime_->store(true, std::memory_order_release);
    startThread(juce::Thread::Priority::low);
}

void RemoteBridgeClient::stop()
{
    if (!isThreadRunning())
        return;
    stopping_.store(true, std::memory_order_release);
    signalThreadShouldExit();
#if JUCE_WINDOWS
    closePipe();
#endif
    stopThread(3000);
}

juce::File RemoteBridgeClient::bridgeConfigFile() const
{
#if JUCE_WINDOWS
    const auto appData = juce::SystemStats::getEnvironmentVariable("APPDATA", {});
    if (appData.isEmpty())
        return {};
    return juce::File(appData).getChildFile("reaper-ai").getChildFile("opentune-bridge.json");
#else
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("reaper-ai").getChildFile("opentune-bridge.json");
#endif
}

void RemoteBridgeClient::run()
{
    while (!threadShouldExit()) {
        if (!runConnection()) {
            for (int elapsed = 0; elapsed < 1000 && !threadShouldExit(); elapsed += 100)
                wait(100);
        }
    }
}

void RemoteBridgeClient::diagnose(const juce::String& message)
{
    if (message == lastDiagnostic_)
        return;
    lastDiagnostic_ = message;
    AppLogger::log("RemoteBridge: " + message);
}

bool RemoteBridgeClient::runConnection()
{
#if !JUCE_WINDOWS
    return false;
#else
    const auto configFile = bridgeConfigFile();
    if (configFile == juce::File() || !configFile.existsAsFile()) {
        diagnose("waiting for bridge config");
        return false;
    }

    const auto parsed = juce::JSON::parse(configFile);
    const auto endpoint = parsed.getProperty("endpoint", {}).toString();
    const auto token = parsed.getProperty("token", {}).toString();
    if (static_cast<int>(parsed.getProperty("protocolVersion", 0)) != kProtocolVersion
        || !endpoint.startsWith("\\\\.\\pipe\\")
        || token.length() < 32 || token.length() > 256) {
        diagnose("bridge config is invalid");
        return false;
    }

    if (!WaitNamedPipeW(endpoint.toWideCharPointer(), 1000)) {
        diagnose("named pipe unavailable, error=" + juce::String(static_cast<int>(GetLastError())));
        return false;
    }
    HANDLE opened = CreateFileW(endpoint.toWideCharPointer(),
                                GENERIC_READ | GENERIC_WRITE,
                                0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (opened == INVALID_HANDLE_VALUE) {
        diagnose("named pipe open failed, error=" + juce::String(static_cast<int>(GetLastError())));
        return false;
    }
    {
        const juce::ScopedLock lock(pipeLock_);
        pipe_ = opened;
    }

    auto registration = std::make_unique<juce::DynamicObject>();
    registration->setProperty("type", "register");
    registration->setProperty("protocolVersion", kProtocolVersion);
    registration->setProperty("token", token);
    registration->setProperty("instanceId", instanceId_);
    registration->setProperty("pairingCode", currentPairingCode());
    registration->setProperty("pluginVersion", juce::String(OPENTUNE_VERSION));
    registration->setProperty("processId", static_cast<int>(GetCurrentProcessId()));
    if (!writeAll(opened, juce::JSON::toString(juce::var(registration.release()), true) + "\n")) {
        diagnose("registration write failed");
        closePipe();
        return false;
    }
    diagnose("registered instance " + instanceId_);

    juce::String buffer;
    char chunk[4096];
    while (!threadShouldExit()) {
        DWORD bytesRead = 0;
        if (!ReadFile(opened, chunk, sizeof(chunk), &bytesRead, nullptr) || bytesRead == 0) {
            diagnose("named pipe disconnected");
            break;
        }
        buffer += juce::String::fromUTF8(chunk, static_cast<int>(bytesRead));
        if (buffer.getNumBytesAsUTF8() > kMaxMessageBytes)
            break;

        int newline = -1;
        while ((newline = buffer.indexOfChar('\n')) >= 0) {
            const auto line = buffer.substring(0, newline);
            buffer = buffer.substring(newline + 1);
            if (line.isEmpty())
                continue;

            const auto request = juce::JSON::parse(line);
            const auto requestId = request.getProperty("requestId", {}).toString();
            if (request.getProperty("type", {}).toString() != "request"
                || static_cast<int>(request.getProperty("protocolVersion", 0)) != kProtocolVersion
                || requestId.isEmpty()) {
                closePipe();
                return false;
            }

            auto state = std::make_shared<DispatchState>();
            const auto lifetime = lifetime_;
            const auto dispatcher = dispatcher_;
            juce::MessageManager::callAsync([state, lifetime, dispatcher, request, requestId]() {
                if (!lifetime->load(std::memory_order_acquire)) {
                    state->response = makeErrorResponse(
                        requestId, "NOT_READY", "OpenTune instance is shutting down.");
                } else {
                    try {
                        state->response = dispatcher(request);
                    } catch (const std::exception& error) {
                        state->response = makeErrorResponse(
                            requestId, "INTERNAL_ERROR", juce::String(error.what()));
                    } catch (...) {
                        state->response = makeErrorResponse(
                            requestId, "INTERNAL_ERROR", "Unhandled OpenTune dispatcher error.");
                    }
                }
                state->completed.signal();
            });

            bool completed = false;
            for (int elapsed = 0; elapsed < 10000 && !threadShouldExit(); elapsed += 100) {
                if (state->completed.wait(100)) {
                    completed = true;
                    break;
                }
            }
            if (threadShouldExit())
                break;
            const auto response = completed
                ? state->response
                : makeErrorResponse(requestId, "TIMEOUT", "OpenTune message-thread dispatch timed out.");
            if (!writeAll(opened, juce::JSON::toString(response, true) + "\n")) {
                closePipe();
                return false;
            }
        }
    }

    closePipe();
    return false;
#endif
}

#if JUCE_WINDOWS
void RemoteBridgeClient::closePipe()
{
    HANDLE handle = nullptr;
    {
        const juce::ScopedLock lock(pipeLock_);
        handle = static_cast<HANDLE>(pipe_);
        pipe_ = nullptr;
    }
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CancelIoEx(handle, nullptr);
        CloseHandle(handle);
    }
}
#endif

} // namespace OpenTune
