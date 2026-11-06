// PobTools filter-editor filesystem layer: locate the POE1 filter folder under
// the user's Documents, enumerate *.filter files, and load/save them through the
// parser (backup .bak + atomic write, mirroring editor_data.cpp's SaveFile).
#pragma once

#include "filter_model.h"
#include <string>
#include <vector>

// A .filter file discovered on disk (for the editor's "open" dropdown).
// A game subscription (Documents\My Games\Path of Exile\OnlineFilters\<id>):
// read only -- the game re-downloads and overwrites these.
struct OnlineFilterEntry {
	std::wstring path;
	std::string name;              // "#name:" (NeverSink-5uberstr-softcore)
	std::string filterVersion;     // "#filterVersion:"
	std::string version;           // NeverSink's own "# VERSION:" in the body
};
// Subscriptions in `dir` (a folder path ending in a backslash); only the head
// of each file is read.
std::vector<OnlineFilterEntry> ListOnlineFiltersIn(const std::wstring& dir);
// ListOnlineFiltersIn(<My Games>\Path of Exile\OnlineFilters\), empty when there is none.
std::vector<OnlineFilterEntry> ListOnlineFilters();
// <Documents>\My Games\Path of Exile\ (with the backslash), "" when missing.
std::wstring Poe1FilterBaseDir();

struct FilterListEntry {
	std::wstring path;
	std::string  name;             // file name only, for display
	bool inItemFilters = false;    // found under the ItemFilters subfolder
};

// POE1 filter directories to scan: Documents\My Games\Path of Exile\ and its
// ItemFilters subfolder. Returns only the ones that exist (may be empty).
std::vector<std::wstring> Poe1FilterDirs();

// List *.filter files across the POE1 filter directories, sorted by name.
std::vector<FilterListEntry> ListFilters();

// Load and parse a .filter file. *ok is false (with an empty result) on failure.
FilterFile LoadFilter(const std::wstring& path, bool* ok);

// Serialize and save a FilterFile to file.path (backup .bak, then atomic
// replace). Returns false and sets *err on failure; clears dirty flags on success.
bool SaveFilter(FilterFile& file, std::string* err);
