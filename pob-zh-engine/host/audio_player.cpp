#include "audio_player.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <cwchar>

#pragma comment(lib, "winmm.lib")

static bool g_mciOpen = false;
static std::wstring g_curPath;   // the file last started ("" after StopAudio)

void StopAudio()
{
	PlaySoundW(nullptr, nullptr, 0);   // halt any PlaySound playback
	if (g_mciOpen) {
		mciSendStringW(L"close pobsnd", nullptr, 0, nullptr);
		g_mciOpen = false;
	}
	g_curPath.clear();
}

bool AudioIsPlaying()
{
	if (!g_mciOpen) return false;
	wchar_t mode[64] = L"";
	if (mciSendStringW(L"status pobsnd mode", mode, 64, nullptr) != 0) return false;
	return wcscmp(mode, L"playing") == 0;
}

const std::wstring& AudioCurrentPath() { return g_curPath; }

bool PlaySystemCue(int id)
{
	StopAudio();
	// five distinct Windows sounds, cycled over the game's sixteen ids
	static const wchar_t* kCues[5] = { L"SystemAsterisk", L"SystemExclamation", L"SystemNotification",
	                                   L"SystemQuestion", L"SystemHand" };
	if (id < 1) id = 1;
	return PlaySoundW(kCues[(id - 1) % 5], nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT) == TRUE;
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
	g_curPath = path;
	return true;
}
