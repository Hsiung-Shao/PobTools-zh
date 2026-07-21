#include "audio_player.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <cwctype>

#pragma comment(lib, "winmm.lib")

static bool g_mciOpen = false;

static std::wstring ext_lower(const std::wstring& p)
{
	size_t dot = p.find_last_of(L'.');
	std::wstring e = (dot == std::wstring::npos) ? std::wstring() : p.substr(dot);
	for (wchar_t& c : e) c = (wchar_t)towlower(c);
	return e;
}

void StopAudio()
{
	PlaySoundW(nullptr, nullptr, 0);   // halt any PlaySound playback
	if (g_mciOpen) {
		mciSendStringW(L"close pobsnd", nullptr, 0, nullptr);
		g_mciOpen = false;
	}
}

bool PlayAudioFile(const std::wstring& path)
{
	StopAudio();
	if (path.empty()) return false;

	std::wstring e = ext_lower(path);
	if (e == L".wav") {
		return PlaySoundW(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT) == TRUE;
	}

	// Compressed formats via MCI (needs a system codec; mp3/wav always work).
	std::wstring open = L"open \"" + path + L"\" alias pobsnd";
	if (mciSendStringW(open.c_str(), nullptr, 0, nullptr) != 0) return false;
	g_mciOpen = true;
	if (mciSendStringW(L"play pobsnd", nullptr, 0, nullptr) != 0) { StopAudio(); return false; }
	return true;
}

bool PlayAudioFileVol(const std::wstring& path, int volumePct)
{
	StopAudio();
	if (path.empty()) return false;
	if (volumePct < 0) volumePct = 0;
	if (volumePct > 100) volumePct = 100;

	// Force the MPEGVideo (DirectShow) device: it plays wav AND mp3 and, unlike
	// waveaudio, supports "setaudio ... volume". Fall back to a plain open.
	std::wstring open = L"open \"" + path + L"\" type mpegvideo alias pobsnd";
	if (mciSendStringW(open.c_str(), nullptr, 0, nullptr) != 0) {
		open = L"open \"" + path + L"\" alias pobsnd";
		if (mciSendStringW(open.c_str(), nullptr, 0, nullptr) != 0) return false;
	}
	g_mciOpen = true;
	std::wstring vol = L"setaudio pobsnd volume to " + std::to_wstring(volumePct * 10);
	mciSendStringW(vol.c_str(), nullptr, 0, nullptr);   // best-effort (needs digital-video capable device? mp3 ok)
	if (mciSendStringW(L"play pobsnd", nullptr, 0, nullptr) != 0) { StopAudio(); return false; }
	return true;
}
