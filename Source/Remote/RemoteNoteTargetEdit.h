#pragma once

#include "../Content/ContentEditCommands.h"
#include "../Content/ContentKey.h"
#include "../Content/EditableContentSnapshot.h"
#include <optional>
#include <string>
#include <vector>

namespace OpenTune {

struct RemoteNoteView
{
    std::string noteHandle;
    int noteIndex = -1;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    float originalPitchHz = 0.0f;
    float targetPitchHz = 0.0f;
    float pitchOffsetSemitones = 0.0f;
    float retuneSpeed = 0.0f;
    float pitchDriftScale = 1.0f;
    float vibratoDepth = 0.0f;
    float vibratoRate = 0.0f;
    bool voiced = false;
};

struct RemoteNoteTarget
{
    std::string noteHandle;
    float targetMidi = 0.0f;
    std::optional<float> retuneSpeed;
};

enum class RemoteNoteTargetPlanStatus
{
    Ready,
    NoChange,
    InvalidRequest,
    AnalysisNotReady,
    StaleRevision,
    NoteNotFound
};

struct RemoteNoteTargetEditPlan
{
    RemoteNoteTargetPlanStatus status = RemoteNoteTargetPlanStatus::InvalidRequest;
    F0FrameRange affectedRange;
    std::vector<Note> beforeNotes;
    std::vector<Note> afterNotes;
    std::vector<PitchCorrectionSegment> beforeSegments;
    std::vector<PitchCorrectionSegment> afterSegments;
};

struct RemoteNoteTargetEditResult
{
    RemoteNoteTargetPlanStatus status = RemoteNoteTargetPlanStatus::InvalidRequest;
    ContentCommitSnapshot contentSnapshot;
};

std::string makeRemoteNoteHandle(const Note& note, int noteIndex, uint64_t notesRevision);
std::vector<RemoteNoteView> makeRemoteNoteViews(const EditableContentSnapshot& snapshot);

RemoteNoteTargetEditPlan planRemoteNoteTargetEdit(
    ContentKey contentKey,
    const EditableContentSnapshot& snapshot,
    uint64_t expectedContentRevision,
    uint64_t expectedNotesRevision,
    const std::vector<RemoteNoteTarget>& targets);

} // namespace OpenTune
