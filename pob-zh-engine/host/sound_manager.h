// Custom-sound library for the filter editor (inspired by POE-Filter-Audio-Manager):
// point at a folder of audio files, list them, preview-play each, and pick one to
// assign to a rule's CustomAlertSound. The folder is remembered in pob-zh.ini.
#pragma once

#include <string>

// Sound folder persisted in pob-zh.ini [PobTools] SoundFolder.
std::wstring GetSoundFolder();
void SetSoundFolder(const std::wstring& folder);

// Win32 folder-picker dialog ("" if cancelled). Shared with the 音效管理 section.
std::wstring BrowseSoundFolder(void* owner = nullptr);
