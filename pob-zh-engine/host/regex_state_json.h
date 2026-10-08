// The JSON pieces regex_state.cpp uses for numeric values, shared with the share
// code (regex_share.cpp) so the two read and write an AlgoValue the same way.
// Separate from regex_state.h so only the files that already parse JSON see
// nlohmann.
#pragma once

#include "regex_frag.h"

#include <string>
#include <vector>

#include <json.hpp>   // nlohmann::ordered_json (deps/nlohmann)

namespace RegexStateJson {

// state.ts numericValue / pages/index.ts sanitizeValue: an object keeps min /
// max (finite numbers, truncated) and choice (string, first 16 UTF-16 units);
// anything that is not an object -> false.
bool ValueFrom(const nlohmann::ordered_json& raw, RegexFrag::AlgoValue& v);
// JSON.stringify of an AlgoValue: {min?, max?, choice?}, integral numbers without a fraction.
nlohmann::ordered_json ValueTo(const RegexFrag::AlgoValue& v);
// sections.ts unionKeys: a then b, first occurrence kept.
std::vector<std::string> UnionKeys(const std::vector<std::string>& a, const std::vector<std::string>& b);
// JS Number#toString of an integral double below 1e21: the shortest round-trip
// digits written out positionally ("12345678901234567000").
std::string JsIntegral(double d);

} // namespace RegexStateJson

struct RegexBookmark;
// exile-appraiser state.ts:174 parseBookmark (c9a7aae), shared by the state file
// (RegexUiState::Parse) and the bookmark pack (regex_bookmarks_share.cpp): false =
// not an object / no name / no page / neither keys nor num (skip it); a field of
// the wrong type THROWS std::runtime_error("欄位 <key> 不是字串") -- the state file
// then refuses the whole file, the pack skips just this bookmark.
bool RegexParseBookmark(const nlohmann::ordered_json& b, RegexBookmark& rec);
