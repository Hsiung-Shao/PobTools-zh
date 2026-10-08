// PobTools filter editor: notice when the open .filter is changed by someone
// else (another editor, FilterBlade's sync, a re-download) while it is open.
//
// A stat, not a read: the file's last-write time and size are compared with the
// ones recorded when it was loaded or saved by us, at most every `interval`
// seconds. Our own save records the new stamp first, so it never reports itself.
#pragma once

#include <string>

struct FileStamp {
	unsigned long long writeTime = 0;  // FILETIME as one number
	unsigned long long size = 0;
	bool ok = false;                   // the file could be stat'ed
	bool operator==(const FileStamp& o) const { return ok == o.ok && writeTime == o.writeTime && size == o.size; }
	bool operator!=(const FileStamp& o) const { return !(*this == o); }
};

FileStamp GetFileStamp(const std::wstring& path);

class ExternalChangeWatch {
public:
	// Start watching `path` as it is now (after a load, after our own save).
	void Reset(const std::wstring& path);
	// Stop watching (nothing open).
	void Clear();
	// Check at most every `interval` seconds of `now` (any monotonic clock).
	// True while a change is pending (until Reset / Acknowledge).
	bool Poll(double now, double interval = 2.0);
	bool changed() const { return changed_; }
	// The user dismissed the notice: take the current stamp as the new normal,
	// so only a further change is reported.
	void Acknowledge();
	const std::wstring& path() const { return path_; }
	// Test aid (POBTOOLS_FILTER_STATE=extchange): report a change without one.
	void MarkChangedForTest() { changed_ = true; }

private:
	std::wstring path_;
	FileStamp stamp_;
	double lastPoll_ = -1e30;
	bool changed_ = false;
};
