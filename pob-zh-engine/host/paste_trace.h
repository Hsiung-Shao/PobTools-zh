// Diagnostics for the Chinese paste path. Both write TSV and return 0 on success.
//
//   RunPasteTrace   one item, one row per line: which rule fired, which file won
//   RunPasteCensus  every dictionary entry's Chinese, same columns
//
// Neither judges pass/fail -- they record what happened and leave classifying to
// analysis. See paste_trace.cpp for why that separation matters here.
#pragma once

#include <string>

int RunPasteTrace(const std::wstring& inPath, const std::wstring& outPath);

// `shell` picks the item context each probe is wrapped in, because section kind
// now drives the rules: "plain" (default) keeps every line Unknown, "mods" puts
// the probe in an annotated modifier section, "property" puts it in the property
// block. Comparing the plain census across two builds is what proves the Unknown
// path did not move.
int RunPasteCensus(const std::wstring& outPath, const std::string& shell = "plain");
