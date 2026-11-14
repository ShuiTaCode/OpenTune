#include "Remote/RemoteNoteTargetEdit.h"
#include "Utils/PitchUtils.h"
#include <cmath>
#include <iostream>
#include <vector>

namespace {

using namespace OpenTune;

EditableContentSnapshot makeSnapshot()
{
    EditableContentSnapshot snapshot;
    snapshot.contentRevision = 19;
    snapshot.notesRevision = 7;
    snapshot.pitchRevision = 11;

    Note first;
    first.startTime = 0.2;
    first.endTime = 0.6;
    first.pitch = PitchUtils::midiToFreq(60.0f);
    first.originalPitch = PitchUtils::midiToFreq(60.25f);
    first.retuneSpeed = 0.35f;

    Note second = first;
    second.startTime = 0.8;
    second.endTime = 1.2;
    second.pitch = PitchUtils::midiToFreq(64.0f);
    second.originalPitch = PitchUtils::midiToFreq(63.8f);
    snapshot.notes = {first, second};

    std::vector<float> originalF0(200, first.originalPitch);
    snapshot.pitchCurve = std::make_shared<const PitchCurveSnapshot>(
        std::move(originalF0), std::vector<float>(200, 1.0f),
        std::vector<PitchCorrectionSegment>{}, 10, 1000.0);
    return snapshot;
}

bool near(float left, float right)
{
    return std::abs(left - right) < 0.01f;
}

bool checkReadViewsExposeRevisionBoundHandles()
{
    const auto snapshot = makeSnapshot();
    const auto views = makeRemoteNoteViews(snapshot);
    return views.size() == 2
        && !views[0].noteHandle.empty()
        && views[0].noteHandle != views[1].noteHandle
        && views[0].noteHandle == makeRemoteNoteHandle(snapshot.notes[0], 0, snapshot.notesRevision)
        && near(views[0].originalPitchHz, snapshot.notes[0].originalPitch)
        && near(views[0].targetPitchHz, snapshot.notes[0].getAdjustedPitch());
}

bool checkTargetProducesCanonicalCorrectionPlan()
{
    const auto snapshot = makeSnapshot();
    const auto views = makeRemoteNoteViews(snapshot);
    RemoteNoteTarget target;
    target.noteHandle = views[0].noteHandle;
    target.targetMidi = 62.0f;
    target.retuneSpeed = 0.55f;

    const auto plan = planRemoteNoteTargetEdit(
        {DomainKind::RegularVST3Capture, 42, 0},
        snapshot,
        snapshot.contentRevision,
        snapshot.notesRevision,
        {target});

    return plan.status == RemoteNoteTargetPlanStatus::Ready
        && plan.affectedRange.startFrame == 4
        && plan.affectedRange.endFrameExclusive == 76
        && plan.beforeNotes.size() == 1
        && plan.afterNotes.size() == 1
        && near(plan.afterNotes[0].getAdjustedPitch(), PitchUtils::midiToFreq(62.0f))
        && near(plan.afterNotes[0].retuneSpeed, 0.55f)
        && !plan.afterSegments.empty()
        && plan.afterSegments[0].source == PitchCorrectionSegment::Source::NoteBased;
}

bool checkStaleAndUnknownHandlesAreRejected()
{
    const auto snapshot = makeSnapshot();
    auto target = RemoteNoteTarget{makeRemoteNoteViews(snapshot)[0].noteHandle, 62.0f, std::nullopt};
    const ContentKey key{DomainKind::RegularVST3Capture, 42, 0};

    const auto staleContent = planRemoteNoteTargetEdit(
        key, snapshot, snapshot.contentRevision + 1, snapshot.notesRevision, {target});
    const auto staleNotes = planRemoteNoteTargetEdit(
        key, snapshot, snapshot.contentRevision, snapshot.notesRevision + 1, {target});
    target.noteHandle = "note:unknown";
    const auto unknown = planRemoteNoteTargetEdit(
        key, snapshot, snapshot.contentRevision, snapshot.notesRevision, {target});

    return staleContent.status == RemoteNoteTargetPlanStatus::StaleRevision
        && staleNotes.status == RemoteNoteTargetPlanStatus::StaleRevision
        && unknown.status == RemoteNoteTargetPlanStatus::NoteNotFound;
}

} // namespace

int main()
{
    if (!checkReadViewsExposeRevisionBoundHandles()
        || !checkTargetProducesCanonicalCorrectionPlan()
        || !checkStaleAndUnknownHandlesAreRejected()) {
        std::cerr << "Remote note target edit regression failed\n";
        return 1;
    }
    return 0;
}
