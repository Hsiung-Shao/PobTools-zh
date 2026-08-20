#include "clipboard_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

std::string ReadClipboardUtf8(void* ownerHwnd)
{
	std::string out;
	if (!OpenClipboard((HWND)ownerHwnd)) return out;
	HANDLE h = GetClipboardData(CF_UNICODETEXT);
	if (h) {
		const wchar_t* w = (const wchar_t*)GlobalLock(h);
		if (w) {
			int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
			if (n > 1) {
				out.resize(n - 1);
				WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
			}
			GlobalUnlock(h);
		}
	}
	CloseClipboard();
	return out;
}

bool WriteClipboardUtf8(void* ownerHwnd, const std::string& textUtf8)
{
	std::wstring w;
	if (!textUtf8.empty()) {
		int n = MultiByteToWideChar(CP_UTF8, 0, textUtf8.data(), (int)textUtf8.size(), nullptr, 0);
		w.resize(n);
		MultiByteToWideChar(CP_UTF8, 0, textUtf8.data(), (int)textUtf8.size(), &w[0], n);
	}
	if (!OpenClipboard((HWND)ownerHwnd)) return false;
	bool ok = false;
	if (EmptyClipboard()) {
		HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
		if (h) {
			if (void* p = GlobalLock(h)) {
				memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
				GlobalUnlock(h);
				// SetClipboardData takes ownership on success; only free on failure.
				ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
			}
			if (!ok) GlobalFree(h);
		}
	}
	CloseClipboard();
	return ok;
}
