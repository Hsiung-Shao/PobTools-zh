#include "data_updater.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

long long DataUpdaterNowFiletime()
{
	FILETIME ft{};
	GetSystemTimeAsFileTime(&ft);
	ULARGE_INTEGER u{ ft.dwLowDateTime, ft.dwHighDateTime };
	return (long long)u.QuadPart;
}

bool DataUpdaterWriteCacheFile(const std::wstring& dst, const std::vector<unsigned char>& bytes)
{
	const std::wstring part = dst + L".part";
	HANDLE h = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD wr = 0;
	bool ok = bytes.empty() || (WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wr, nullptr) && wr == bytes.size());
	CloseHandle(h);
	ok = ok && MoveFileExW(part.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
	if (!ok) DeleteFileW(part.c_str());
	return ok;
}
