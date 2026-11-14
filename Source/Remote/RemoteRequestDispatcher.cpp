#include "RemoteRequestDispatcher.h"
#include "../PluginProcessor.h"
#if JucePlugin_Enable_ARA
#include "../ARA/OpenTuneDocumentController.h"
#endif
#include "RemoteNoteTargetEdit.h"
#include <algorithm>
#include <limits>

namespace OpenTune {
namespace {

constexpr int kProtocolVersion = 1;
constexpr int kMaxPitchFramesPerResponse = 20000;

juce::var response(const juce::String& requestId,
                   bool ok,
                   const juce::String& code,
                   juce::var data = {},
                   const juce::String& message = {})
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("type", "response");
    object->setProperty("protocolVersion", kProtocolVersion);
    object->setProperty("requestId", requestId);
    object->setProperty("ok", ok);
    object->setProperty("code", code);
    if (!data.isVoid())
        object->setProperty("data", std::move(data));
    if (message.isNotEmpty())
        object->setProperty("message", message);
    return juce::var(object.release());
}

juce::var contentKeyToVar(ContentKey key)
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("domainKind", getDomainKindName(key.domainKind));
    object->setProperty("objectId", juce::String(key.objectId));
    object->setProperty("sourceWindowDiscriminator", juce::String(key.sourceWindowDiscriminator));
    return juce::var(object.release());
}

bool parseUnsigned(const juce::var& value, uint64_t& result)
{
    try {
        const auto text = value.toString().toStdString();
        size_t consumed = 0;
        const auto parsed = std::stoull(text, &consumed, 10);
        if (consumed != text.size())
            return false;
        result = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseContentKey(const juce::var& value, ContentKey& key)
{
    if (value.getDynamicObject() == nullptr)
        return false;
    const auto kind = value.getProperty("domainKind", {}).toString();
    if (kind == "ARAAudioModification")
        key.domainKind = DomainKind::ARAAudioModification;
    else if (kind == "RegularVST3Capture")
        key.domainKind = DomainKind::RegularVST3Capture;
    else
        return false;
    return parseUnsigned(value.getProperty("objectId", {}), key.objectId)
        && parseUnsigned(value.getProperty("sourceWindowDiscriminator", {}),
                         key.sourceWindowDiscriminator)
        && key.isValid();
}

juce::String originalF0StateName(OriginalF0State state)
{
    switch (state) {
        case OriginalF0State::NotRequested: return "not_requested";
        case OriginalF0State::Extracting: return "extracting";
        case OriginalF0State::Ready: return "ready";
        case OriginalF0State::Failed: return "failed";
    }
    return "unknown";
}

juce::String planStatusCode(RemoteNoteTargetPlanStatus status)
{
    switch (status) {
        case RemoteNoteTargetPlanStatus::Ready:
        case RemoteNoteTargetPlanStatus::NoChange: return "SUCCESS_VERIFIED";
        case RemoteNoteTargetPlanStatus::AnalysisNotReady: return "NO_ANALYSIS";
        case RemoteNoteTargetPlanStatus::StaleRevision: return "STALE_REVISION";
        case RemoteNoteTargetPlanStatus::NoteNotFound: return "CONTENT_NOT_FOUND";
        case RemoteNoteTargetPlanStatus::InvalidRequest: return "VALIDATION_FAILED";
    }
    return "INTERNAL_ERROR";
}

juce::var revisionsToVar(const EditableContentSnapshot& snapshot)
{
    auto data = std::make_unique<juce::DynamicObject>();
    data->setProperty("contentRevision", juce::String(snapshot.contentRevision));
    data->setProperty("notesRevision", juce::String(snapshot.notesRevision));
    data->setProperty("pitchRevision", juce::String(snapshot.pitchRevision));
    return juce::var(data.release());
}

} // namespace

RemoteRequestDispatcher::RemoteRequestDispatcher(OpenTuneAudioProcessor& processor)
    : processor_(processor)
{
}

juce::var RemoteRequestDispatcher::dispatch(const juce::var& request)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto requestId = request.getProperty("requestId", {}).toString();
    const auto method = request.getProperty("method", {}).toString();

    if (method == "get_instance_state") {
        juce::Array<juce::var> contents;
        for (const auto key : processor_.listRemoteContentKeys()) {
            auto item = std::make_unique<juce::DynamicObject>();
            item->setProperty("contentKey", contentKeyToVar(key));
            if (const auto snapshot = processor_.getContentSnapshot(key)) {
                item->setProperty("revisions", revisionsToVar(*snapshot));
                item->setProperty("analysisState", originalF0StateName(snapshot->originalF0State));
                item->setProperty("noteCount", static_cast<int>(snapshot->notes.size()));
            }
#if JucePlugin_Enable_ARA
            if (const auto* dc = processor_.getDocumentController()) {
                juce::Array<juce::var> placements;
                for (const auto& placement : dc->listPlaybackRegionProjections()) {
                    if (placement.contentKey != key)
                        continue;
                    auto value = std::make_unique<juce::DynamicObject>();
                    value->setProperty("startInPlaybackTime", placement.startInPlaybackTime);
                    value->setProperty("startInModificationTime", placement.startInModificationTime);
                    value->setProperty("durationInPlaybackTime", placement.durationInPlaybackTime);
                    value->setProperty("durationInModificationTime", placement.durationInModificationTime);
                    placements.add(juce::var(value.release()));
                }
                item->setProperty("placements", placements);
            }
#endif
            contents.add(juce::var(item.release()));
        }
        auto data = std::make_unique<juce::DynamicObject>();
        data->setProperty("contents", contents);
        return response(requestId, true, "SUCCESS_VERIFIED", juce::var(data.release()));
    }

    ContentKey key;
    if (!parseContentKey(request.getProperty("contentKey", {}), key))
        return response(requestId, false, "VALIDATION_FAILED", {},
                        "A valid contentKey is required.");

    if (method == "analyze") {
        if (!processor_.requestRemoteAnalysis(key))
            return response(requestId, false, "CONTENT_NOT_FOUND", {});
        const auto snapshot = processor_.getContentSnapshot(key);
        if (snapshot == nullptr)
            return response(requestId, false, "CONTENT_NOT_FOUND", {});
        if (!snapshot->hasUsableOriginalF0() || snapshot->originalF0State != OriginalF0State::Ready)
            return response(requestId, false, "NOT_READY", revisionsToVar(*snapshot),
                            "Pitch analysis is in progress.");
        if (snapshot->notes.empty())
            return response(requestId, false, "NO_ANALYSIS", revisionsToVar(*snapshot),
                            "Pitch analysis completed without monophonic notes.");
        return response(requestId, true, "SUCCESS_VERIFIED", revisionsToVar(*snapshot));
    }

    const auto snapshot = processor_.getContentSnapshot(key);
    if (snapshot == nullptr)
        return response(requestId, false, "CONTENT_NOT_FOUND", {});

    if (method == "get_notes") {
        juce::Array<juce::var> notes;
        for (const auto& view : makeRemoteNoteViews(*snapshot)) {
            auto note = std::make_unique<juce::DynamicObject>();
            note->setProperty("noteHandle", juce::String(view.noteHandle));
            note->setProperty("noteIndex", view.noteIndex);
            note->setProperty("startSeconds", view.startSeconds);
            note->setProperty("endSeconds", view.endSeconds);
            note->setProperty("originalPitchHz", view.originalPitchHz);
            note->setProperty("targetPitchHz", view.targetPitchHz);
            note->setProperty("pitchOffsetSemitones", view.pitchOffsetSemitones);
            note->setProperty("retuneSpeed", view.retuneSpeed);
            note->setProperty("pitchDriftScale", view.pitchDriftScale);
            note->setProperty("vibratoDepth", view.vibratoDepth);
            note->setProperty("vibratoRate", view.vibratoRate);
            note->setProperty("voiced", view.voiced);
            notes.add(juce::var(note.release()));
        }
        auto data = revisionsToVar(*snapshot);
        data.getDynamicObject()->setProperty("notes", notes);
        return response(requestId, true, "SUCCESS_VERIFIED", std::move(data));
    }

    if (method == "get_pitch_curve") {
        if (!snapshot->hasUsableOriginalF0())
            return response(requestId, false, "NO_ANALYSIS", revisionsToVar(*snapshot));
        const auto params = request.getProperty("params", {});
        const auto& curve = *snapshot->pitchCurve;
        const int frameCount = static_cast<int>(curve.size());
        const int startFrame = juce::jlimit(
            0, frameCount, static_cast<int>(params.getProperty("startFrame", 0)));
        const int requestedEnd = static_cast<int>(
            params.getProperty("endFrameExclusive", std::min(frameCount, startFrame + kMaxPitchFramesPerResponse)));
        const int endFrame = juce::jlimit(
            startFrame, std::min(frameCount, startFrame + kMaxPitchFramesPerResponse), requestedEnd);

        juce::Array<juce::var> originalF0;
        const auto& original = curve.getOriginalF0();
        originalF0.ensureStorageAllocated(endFrame - startFrame);
        for (int frame = startFrame; frame < endFrame; ++frame)
            originalF0.add(original[static_cast<size_t>(frame)]);

        juce::Array<juce::var> segments;
        for (const auto& segment : curve.getCorrectionSegments()) {
            if (segment.endFrame <= startFrame || segment.startFrame >= endFrame)
                continue;
            const int clippedStart = std::max(startFrame, segment.startFrame);
            const int clippedEnd = std::min(endFrame, segment.endFrame);
            juce::Array<juce::var> f0;
            for (int frame = clippedStart; frame < clippedEnd; ++frame)
                f0.add(segment.f0Data[static_cast<size_t>(frame - segment.startFrame)]);
            auto item = std::make_unique<juce::DynamicObject>();
            item->setProperty("startFrame", clippedStart);
            item->setProperty("endFrameExclusive", clippedEnd);
            item->setProperty("f0Hz", f0);
            segments.add(juce::var(item.release()));
        }

        auto data = revisionsToVar(*snapshot);
        auto* object = data.getDynamicObject();
        object->setProperty("startFrame", startFrame);
        object->setProperty("endFrameExclusive", endFrame);
        object->setProperty("totalFrames", frameCount);
        object->setProperty("hopSize", curve.getHopSize());
        object->setProperty("sampleRate", curve.getSampleRate());
        object->setProperty("originalF0Hz", originalF0);
        object->setProperty("correctionSegments", segments);
        return response(requestId, true, "SUCCESS_VERIFIED", std::move(data));
    }

    if (method == "apply_note_targets") {
        const auto params = request.getProperty("params", {});
        uint64_t expectedContentRevision = 0;
        uint64_t expectedNotesRevision = 0;
        if (!parseUnsigned(params.getProperty("expectedContentRevision", {}),
                           expectedContentRevision)
            || !parseUnsigned(params.getProperty("expectedNotesRevision", {}),
                              expectedNotesRevision)
            || params.getProperty("targets", {}).getArray() == nullptr)
            return response(requestId, false, "VALIDATION_FAILED", {});

        std::vector<RemoteNoteTarget> targets;
        for (const auto& value : *params.getProperty("targets", {}).getArray()) {
            RemoteNoteTarget target;
            target.noteHandle = value.getProperty("noteHandle", {}).toString().toStdString();
            target.targetMidi = static_cast<float>(
                static_cast<double>(value.getProperty("targetMidi", std::numeric_limits<double>::quiet_NaN())));
            const auto retune = value.getProperty("retuneSpeed", {});
            if (!retune.isVoid())
                target.retuneSpeed = static_cast<float>(static_cast<double>(retune));
            targets.push_back(std::move(target));
        }
        const auto result = processor_.applyRemoteNoteTargets(
            key, expectedContentRevision, expectedNotesRevision, targets,
            "REAPER AI pitch targets");
        const auto code = planStatusCode(result.status);
        const bool ok = result.status == RemoteNoteTargetPlanStatus::Ready
            || result.status == RemoteNoteTargetPlanStatus::NoChange;
        return response(requestId, ok, code,
                        result.contentSnapshot != nullptr
                            ? revisionsToVar(*result.contentSnapshot)
                            : juce::var());
    }

    if (method == "get_render_status") {
        const auto stats = processor_.getReadableContentChunkStats(key);
        auto data = revisionsToVar(*snapshot);
        auto* object = data.getDynamicObject();
        object->setProperty("idle", stats.idle);
        object->setProperty("pending", stats.pending);
        object->setProperty("running", stats.running);
        object->setProperty("blank", stats.blank);
        object->setProperty("failed", stats.failed);
        object->setProperty("complete", !stats.hasActiveWork() && stats.failed == 0);
        return response(requestId, true, "SUCCESS_VERIFIED", std::move(data));
    }

    return response(requestId, false, "VALIDATION_FAILED", {}, "Unknown method.");
}

} // namespace OpenTune
