#include "RemoteNoteTargetEdit.h"
#include "../Utils/F0Timeline.h"
#include "../Utils/PitchUtils.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace OpenTune {
namespace {

uint64_t fnv1a64(const std::string& text)
{
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<Note> notesInRange(
    const std::vector<Note>& notes, double startSeconds, double endSeconds)
{
    std::vector<Note> result;
    for (const auto& note : notes) {
        if (note.startTime < endSeconds && note.endTime > startSeconds)
            result.push_back(note);
    }
    return result;
}

std::vector<PitchCorrectionSegment> segmentsInRange(
    const std::vector<PitchCorrectionSegment>& segments, int startFrame, int endFrame)
{
    std::vector<PitchCorrectionSegment> result;
    for (const auto& segment : segments) {
        if (segment.endFrame <= startFrame || segment.startFrame >= endFrame)
            continue;

        const int clippedStart = std::max(segment.startFrame, startFrame);
        const int clippedEnd = std::min(segment.endFrame, endFrame);
        const int offset = clippedStart - segment.startFrame;
        const int length = clippedEnd - clippedStart;
        if (offset < 0 || length <= 0 || offset + length > static_cast<int>(segment.f0Data.size()))
            continue;

        auto clipped = segment;
        clipped.startFrame = clippedStart;
        clipped.endFrame = clippedEnd;
        clipped.f0Data.assign(segment.f0Data.begin() + offset,
                              segment.f0Data.begin() + offset + length);
        if (!segment.baseF0Data.empty()
            && offset + length <= static_cast<int>(segment.baseF0Data.size())) {
            clipped.baseF0Data.assign(segment.baseF0Data.begin() + offset,
                                      segment.baseF0Data.begin() + offset + length);
        }
        result.push_back(std::move(clipped));
    }
    return result;
}

} // namespace

std::string makeRemoteNoteHandle(const Note& note, int noteIndex, uint64_t notesRevision)
{
    std::ostringstream identity;
    identity.imbue(std::locale::classic());
    identity << std::setprecision(17)
             << notesRevision << ':' << noteIndex << ':'
             << note.startTime << ':' << note.endTime << ':'
             << note.originalPitch << ':' << note.isVoiced;

    std::ostringstream handle;
    handle.imbue(std::locale::classic());
    handle << "note:" << notesRevision << ':' << noteIndex << ':'
           << std::hex << fnv1a64(identity.str());
    return handle.str();
}

std::vector<RemoteNoteView> makeRemoteNoteViews(const EditableContentSnapshot& snapshot)
{
    std::vector<RemoteNoteView> views;
    views.reserve(snapshot.notes.size());
    for (int index = 0; index < static_cast<int>(snapshot.notes.size()); ++index) {
        const auto& note = snapshot.notes[static_cast<size_t>(index)];
        views.push_back({
            makeRemoteNoteHandle(note, index, snapshot.notesRevision),
            index,
            note.startTime,
            note.endTime,
            note.originalPitch,
            note.getAdjustedPitch(),
            note.pitchOffset,
            note.retuneSpeed,
            note.pitchDriftScale,
            note.vibratoDepth,
            note.vibratoRate,
            note.isVoiced
        });
    }
    return views;
}

RemoteNoteTargetEditPlan planRemoteNoteTargetEdit(
    ContentKey contentKey,
    const EditableContentSnapshot& snapshot,
    uint64_t expectedContentRevision,
    uint64_t expectedNotesRevision,
    const std::vector<RemoteNoteTarget>& targets)
{
    RemoteNoteTargetEditPlan plan;
    if (!contentKey.isValid() || targets.empty()) {
        plan.status = RemoteNoteTargetPlanStatus::InvalidRequest;
        return plan;
    }
    if (snapshot.contentRevision != expectedContentRevision
        || snapshot.notesRevision != expectedNotesRevision) {
        plan.status = RemoteNoteTargetPlanStatus::StaleRevision;
        return plan;
    }
    if (!snapshot.hasUsableOriginalF0()) {
        plan.status = RemoteNoteTargetPlanStatus::AnalysisNotReady;
        return plan;
    }

    const auto views = makeRemoteNoteViews(snapshot);
    std::unordered_map<std::string, int> noteIndices;
    for (const auto& view : views)
        noteIndices.emplace(view.noteHandle, view.noteIndex);

    auto afterNotes = snapshot.notes;
    std::unordered_set<std::string> seenHandles;
    double dirtyStartSeconds = 0.0;
    double dirtyEndSeconds = 0.0;
    bool hasDirtyRange = false;
    bool changed = false;

    for (const auto& target : targets) {
        if (!std::isfinite(target.targetMidi)
            || target.targetMidi < 0.0f
            || target.targetMidi > 127.0f
            || (target.retuneSpeed.has_value()
                && (!std::isfinite(*target.retuneSpeed)
                    || *target.retuneSpeed < 0.0f
                    || *target.retuneSpeed > 1.0f))
            || !seenHandles.insert(target.noteHandle).second) {
            plan.status = RemoteNoteTargetPlanStatus::InvalidRequest;
            return plan;
        }

        const auto indexIt = noteIndices.find(target.noteHandle);
        if (indexIt == noteIndices.end()) {
            plan.status = RemoteNoteTargetPlanStatus::NoteNotFound;
            return plan;
        }

        auto& note = afterNotes[static_cast<size_t>(indexIt->second)];
        const float targetPitch = PitchUtils::midiToFreq(target.targetMidi);
        const bool pitchChanged = std::abs(note.getAdjustedPitch() - targetPitch) > 1.0e-4f;
        const bool retuneChanged = target.retuneSpeed.has_value()
            && std::abs(note.retuneSpeed - *target.retuneSpeed) > 1.0e-6f;
        if (!pitchChanged && !retuneChanged)
            continue;

        note.pitch = targetPitch;
        note.pitchOffset = 0.0f;
        if (target.retuneSpeed.has_value())
            note.retuneSpeed = *target.retuneSpeed;
        note.dirty = true;
        dirtyStartSeconds = hasDirtyRange
            ? std::min(dirtyStartSeconds, note.startTime)
            : note.startTime;
        dirtyEndSeconds = hasDirtyRange
            ? std::max(dirtyEndSeconds, note.endTime)
            : note.endTime;
        hasDirtyRange = true;
        changed = true;
    }

    if (!changed) {
        plan.status = RemoteNoteTargetPlanStatus::NoChange;
        return plan;
    }

    const auto curveSnapshot = snapshot.pitchCurve;
    const F0Timeline timeline(
        curveSnapshot->getHopSize(), curveSnapshot->getSampleRate(),
        static_cast<int>(curveSnapshot->size()));
    const auto editRange = timeline.rangeForTimes(dirtyStartSeconds, dirtyEndSeconds);
    if (editRange.isEmpty()) {
        plan.status = RemoteNoteTargetPlanStatus::InvalidRequest;
        return plan;
    }

    auto derivedCurve = PitchCurve::fromSnapshot(curveSnapshot);
    const auto derivedSnapshot = derivedCurve->applyCorrectionToRange(
        afterNotes,
        editRange.startFrame,
        editRange.endFrameExclusive,
        static_cast<float>(snapshot.pitchShiftSettings.getPitchRatio()),
        0.0f,
        0.0f,
        PitchControlConfig::kDefaultVibratoRateHz);

    plan.affectedRange = PitchCurve::expandNoteBasedCorrectionRange(
        editRange.startFrame, editRange.endFrameExclusive, timeline.endFrameExclusive());
    const double affectedStartSeconds = timeline.timeAtFrame(plan.affectedRange.startFrame);
    const double affectedEndSeconds = timeline.timeAtFrame(plan.affectedRange.endFrameExclusive);
    plan.beforeNotes = notesInRange(snapshot.notes, affectedStartSeconds, affectedEndSeconds);
    plan.afterNotes = notesInRange(afterNotes, affectedStartSeconds, affectedEndSeconds);
    plan.beforeSegments = segmentsInRange(
        curveSnapshot->getCorrectionSegments(),
        plan.affectedRange.startFrame,
        plan.affectedRange.endFrameExclusive);
    plan.afterSegments = segmentsInRange(
        derivedSnapshot->getCorrectionSegments(),
        plan.affectedRange.startFrame,
        plan.affectedRange.endFrameExclusive);
    plan.status = RemoteNoteTargetPlanStatus::Ready;
    return plan;
}

} // namespace OpenTune
