# REAPER AI integration

This AGPLv3 fork is based on OpenTune upstream commit
`34e87af5e9a00e4c91890c19570bebdac5147da4`.

## Architecture

- `Source/Remote/RemoteBridgeClient.*` is a background-only local named-pipe
  client. It reads `%APPDATA%/reaper-ai/opentune-bridge.json`, authenticates
  with the per-process token, and never performs file, JSON, or pipe work on
  the audio thread. The pairing code is read live from the host parameter on
  every registration attempt so project restore and later parameter changes
  stay aligned with REAPER AI binding.
- Requests are dispatched on JUCE's message thread by
  `RemoteRequestDispatcher`.
- Reads always use the live `EditableContentSnapshot`.
- Writes use `ContentEditCommands::commitNotesAndSegments` and
  `PianoRollEditAction`. There is no REAPER-AI correction store.
- ARA edits remain owned by `doStoreObjectsToStream` /
  `doRestoreObjectsFromStream`; capture-mode VST3 edits remain in the existing
  processor-state `CAPz` tail.
- The `REAPER AI Pairing Code` host parameter joins a process-local plugin
  registration to REAPER project, track, item, take, and FX GUID observations.

Protocol version 1 supports `get_instance_state`, `analyze`, `get_notes`,
`get_pitch_curve`, `apply_note_targets`, and `get_render_status`. Note writes
require both content and note revisions plus revision-bound note handles.

## Licensing

The fork remains AGPLv3. FCPE is MIT licensed. The bundled OpenVPI HifiGAN
weights are CC BY-NC-SA 4.0 and therefore block an unreviewed commercial
distribution. A commercial product must replace those weights with a model
whose code, training data, and weights permit the intended use, or obtain
separate rights. REAPER AI does not upload source audio to a cloud model.
