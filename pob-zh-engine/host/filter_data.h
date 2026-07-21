// PobTools filter-editor filesystem layer: locate the POE1 filter folder under
// the user's Documents, enumerate *.filter files, and load/save them through the
// parser (backup .bak + atomic write, mirroring editor_data.cpp's SaveFile).
#pragma once

#include "filter_model.h"
#include <string>
#include <vector>

// A .filter file discovered on disk (for the editor's "open" dropdown).
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
