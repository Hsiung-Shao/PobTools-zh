// Share codes and templates of the Poe Regex tool (R8). Ported from
// exile-appraiser `regex/src/share.ts` (and `embed.ts` shareStateOf /
// resolvedValues); every function names the TS line it mirrors, and
// regex_r8_golden.inc (that TS run over OUR Data files) holds them to the same
// results -- including decoding codes exile-appraiser produced.
//
//   ShareState = { v:2, game, mode, pages:{pageId:[key]}, sections:{hostId:[entryId]},
//                  numeric:{storeKey:{entryId:{min,max,choice}}}, custom:[], excludes:[] }
//   key: corpus page = the English first line (as bookmarks); algorithmic page = entry id.
//   code = JSON -> gzip -> base64url (no padding).
//
// gzip: miniz (tdefl / tinfl, already in the host for zip_extract) for the
// deflate stream, with the gzip header and the CRC32 / ISIZE trailer written
// and checked here. The header is the one Node's zlib writes (no name, mtime 0,
// OS 10), but the deflate bytes are miniz's, not zlib's: a code made here can
// differ byte for byte from exile-appraiser's for the same state, and both
// decode to the same JSON (which IS byte-identical: JSON.stringify's layout).
// Decoding refuses a bad header, a CRC / length mismatch, trailing bytes and
// anything that inflates past kMaxJsonBytes (a zip bomb stops there).
//
// Interop: every page shares keys with exile-appraiser EXCEPT the item-mod
// values page, whose entry ids here are GGPK stat ids (regex_itemmods.h). Its
// keys in a code from there resolve to nothing and are counted in `missed`
// (and per page in `missedByPage`), never dropped silently; a code made here
// with that page is read there without it.
//
// Pure: no ImGui. LoadTemplates is the only function that touches a file.
#pragma once

#include "regex_algo_pages.h"
#include "regex_embed.h"
#include "regex_state.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace RegexShare {

// share.ts:34 SHARE_VERSION; :36 READABLE_VERSIONS = 1, 2
constexpr int kVersion = 2;

// Inflated JSON larger than this is refused (a real state is a few KB; every
// page fully ticked is well under 1 MB).
constexpr size_t kMaxJsonBytes = 8u << 20;
// A pasted code longer than this is refused before decoding.
constexpr size_t kMaxCodeChars = 4u << 20;

// JSON object key order matters to the byte-identical JSON text, so these are
// ordered lists, not maps (JS objects keep insertion order).
using KeyLists = std::vector<std::pair<std::string, std::vector<std::string>>>;
using ValueLists = std::vector<std::pair<std::string, RegexValueList>>;

// share.ts:20 ShareState (always v2 once normalized).
struct State {
	std::string game;               // poe1 | poe2
	std::string mode = "any";       // any | all | none
	KeyLists pages;
	KeyLists sections;              // host page id -> numeric-section entry ids
	ValueLists numeric;             // vendor / item-mod page id, or host page id -> values
	std::vector<std::string> custom;
	std::vector<std::string> excludes;
};

struct Normalized {
	State state;
	std::vector<std::string> warnings;   // share.ts wording, in its order
};

// share.ts:140 base64url / :151 fromBase64url. Decoding drops whitespace (JS \s)
// and '=', accepts '+' '/' as '-' '_', ignores trailing bits; false + *err on
// any other character.
std::string Base64Url(const std::string& bytes);
bool FromBase64Url(const std::string& text, std::string& out, std::string* err);

// gzip of `data` (miniz deflate, level 6, Node's header).
std::string Gzip(const std::string& data);
// One gzip member, nothing after it; false + *err on a bad header, a broken
// deflate stream, a CRC32 / ISIZE mismatch, trailing bytes or output past maxOut.
bool Gunzip(const std::string& gz, std::string& out, size_t maxOut, std::string* err);

// share.ts:46 migrateShareSections (in place; idempotent).
void MigrateSections(State& s);

// share.ts:65 normalizeShareState over JSON text. False + *err where the TS
// throws (not an object, version, game; or text that is not JSON).
bool NormalizeJson(const std::string& jsonText, bool requireVersion, Normalized& out, std::string* err);

// encodeShare's JSON.stringify(doc): v, game, mode, pages, sections, numeric, custom, excludes.
std::string ToJson(const State& s);

// share.ts:182 encodeShare / :197 decodeShare. Decode errors use the TS wording
// ("分享碼是空的", "分享碼無法解壓縮(...)", "分享碼內容不是 JSON", version, game).
std::string Encode(const State& s);
bool Decode(const std::string& code, Normalized& out, std::string* err);

// share.ts:211 ResolvedState. Keys are INTERNAL page ids (a section's own id,
// e.g. map_numeric), as the TS has them.
struct Resolved {
	std::map<std::string, std::vector<int>> picks;
	ValueLists values;                     // page id -> values (existing entries only)
	int missed = 0;
	std::vector<std::string> unknownPages;
	// PobTools addition: where the misses are (page id as written in the code -> count),
	// so the panel can say which page lost them (the item-mod page, typically).
	std::vector<std::pair<std::string, int>> missedByPage;
};

// share.ts:223 resolveState; `pages` may hold both games (filtered to s.game).
Resolved Resolve(const State& s, const std::vector<RegexAlgo::PageRef>& pages);

// embed.ts:136 resolvedValues: internal page id -> store key (a section -> its host),
// merged {...earlier, ...later}.
ValueLists ResolvedValues(const ValueLists& values);

// embed.ts:113 shareStateOf: the game's ticked pages (sections under `sections`),
// values of ticked algorithmic entries only (stored value, else the default).
// `values` is keyed by store key (RegexEmbed::ValuesMap, as the bookmarks use).
State StateOf(const std::string& game, const std::vector<RegexAlgo::PageRef>& pages,
              const RegexEmbed::PicksMap& picks, const RegexEmbed::ValuesMap& values,
              const std::string& mode, const std::vector<std::string>& custom,
              const std::vector<std::string>& excludes);

// share.ts:262 RegexTemplate
struct Template {
	std::string id, game;
	std::string nameZh, nameEn, descZh, descEn;
	State state;
};

// share.ts:277 parseTemplates: a broken template is skipped and reported in
// `errors` (TS wording); false + *err when the file itself is unusable.
bool ParseTemplates(const std::string& text, std::vector<Template>& out, std::vector<std::string>& errors,
                    std::string* err);
// Data\regex_templates.json (exile-appraiser data/regex/templates.json, verbatim).
bool LoadTemplates(const std::wstring& exeDir, std::vector<Template>& out, std::vector<std::string>& errors,
                   std::string* err);

} // namespace RegexShare
