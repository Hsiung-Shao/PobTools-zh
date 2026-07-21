// PobTools filter-editor shared helpers (string / file / dialog / block summary),
// used by the editor app shell and its per-section draw code. Extracted verbatim
// from filter_editor.cpp so the shell and sections can share one copy.
#pragma once

#include "filter_model.h"
#include <string>
#include <vector>

// UTF-16 -> UTF-8.
std::string EdNarrow(const std::wstring& w);

// UTF-8 -> UTF-16.
std::wstring EdWiden(const std::string& s);

// Read a whole file into bytes (empty on failure). Used for the font atlas.
std::vector<unsigned char> EdReadFile(const std::wstring& path);

// Case-insensitive (ASCII) substring test; needle is already lower-cased.
bool EdContainsCI(const std::string& hay, const std::string& needleLower);

// ASCII lower-case copy.
std::string EdToLowerAscii(const std::string& s);

// Win32 open/save dialog scoped to *.filter. Returns "" if cancelled.
// save=true -> Save As. initialDir may be empty.
std::wstring EdFilterDialog(const std::wstring& initialDir, bool save);

// One-line summary of a block's conditions (for rule lists / status / search).
std::string BlockSummary(const FilterFile& f, const FilterBlock& b);
