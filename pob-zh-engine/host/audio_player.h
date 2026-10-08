// Minimal audio preview for the filter editor's sound library.
//   .wav            -> PlaySound (winmm)
//   .mp3/.ogg/...   -> MCI (mciSendString)
// Async; stops any previous clip first. Used only to audition custom alert sounds.
#pragma once

#include <string>

// Play an audio file. Returns false if the format/codec isn't available.
bool PlayAudioFile(const std::wstring& path);

// Play with a volume (0-100%). Routes every format through MCI so the volume
// applies uniformly (wav included). 100 == device volume, same as PlayAudioFile.
bool PlayAudioFileVol(const std::wstring& path, int volumePct);

// Stop whatever is currently playing.
void StopAudio();

// Is the clip started by PlayAudioFile / PlayAudioFileVol still playing? Asks
// MCI (one cheap status query); a .wav started by PlayAudioFile goes through
// PlaySound, which cannot be asked, and reports false.
bool AudioIsPlaying();
// The file last started (empty after StopAudio).
const std::wstring& AudioCurrentPath();

// A stand-in for the game's built-in alert sound `id` (1..16): one of the
// Windows system sounds. The game's own sound files are not on disk, so this
// only says "a sound would play here", not what it sounds like.
bool PlaySystemCue(int id);
