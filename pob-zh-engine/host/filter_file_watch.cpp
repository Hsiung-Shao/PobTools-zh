#include "filter_file_watch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

FileStamp GetFileStamp(const std::wstring& path)
{
	FileStamp s;
	WIN32_FILE_ATTRIBUTE_DATA a{};
	if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return s;
	s.writeTime = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
	s.size = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
	s.ok = true;
	return s;
}

void ExternalChangeWatch::Reset(const std::wstring& path)
{
	path_ = path;
	stamp_ = GetFileStamp(path);
	changed_ = false;
	lastPoll_ = -1e30;
}

void ExternalChangeWatch::Clear()
{
	path_.clear();
	stamp_ = FileStamp{};
	changed_ = false;
}

bool ExternalChangeWatch::Poll(double now, double interval)
{
	if (path_.empty() || changed_) return changed_;
	if (now - lastPoll_ < interval) return false;
	lastPoll_ = now;
	// A file briefly missing (an editor's atomic replace) is not a change yet:
	// the next poll sees the new file.
	const FileStamp cur = GetFileStamp(path_);
	if (cur.ok && cur != stamp_) changed_ = true;
	return changed_;
}

void ExternalChangeWatch::Acknowledge()
{
	stamp_ = GetFileStamp(path_);
	changed_ = false;
}
