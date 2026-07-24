// In-memory zip extraction (miniz wrapper) for the app updater.
//
// Entry names are normalized ('\' -> '/', tolerating PowerShell
// Compress-Archive output) and validated before any disk write: absolute
// paths, drive letters and "."/".." components are rejected (zip-slip).
// All file I/O is wide-char Win32, so non-ASCII install paths are fine.
//
// On failure returns false and fills *err; a partial extraction may remain,
// so callers must always extract into a fresh staging directory, never over
// live data.
#pragma once

#include <string>

bool ExtractZipToDir(const void* data, size_t size, const std::wstring& destDir,
                     std::string* err, int* filesOut = nullptr);
