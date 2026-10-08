// Bookmark packs of the Poe Regex tool: the bookmarks / folders the player
// picked (one or both games) as one code, for "送到 ExileAppraiser"
// (regex_send.h, --regex-bookmarks) and "匯入書籤包" (clipboard). The format is
// exile-appraiser's (it was defined there first; docs/regex-share-cli.md
// "書籤包"); every function names the line of regex/src/bookmarks-share.ts it
// mirrors, at exile-appraiser c9a7aae (B worktree branch
// claude/realtime-currency-rates-60c13d), and regex_r9_golden.inc (that TS run
// over generated packs) holds them to the same results.
//
//   { "kind":"regex-bookmarks", "v":1,
//     "folders":{"poe1":[{"name","collapsed"}],"poe2":[...]},
//     "bookmarks":[{"name","page","game","mode","lang","keys","alt","numeric"?,"num"?,"folder"?}] }
//
// - A bookmark is the regex_ui.json schema 5 bookmark (RegexParseBookmark), with
//   `game` required and NO `hotkey` (never written, dropped on read).
// - code = canonical JSON -> gzip -> base64url, the share code's path
//   (RegexShare::Gzip / GunzipCode). The JSON is byte-identical to
//   canonicalBookmarkPackJson; the gzip bytes are miniz's, so a code can differ
//   from exile-appraiser's for the same pack and both decode to the same JSON.
// - Merge adds bookmarks / folders only (ticks, values, custom text, excludes,
//   mode untouched); same game + folder + name -> "name (2)", "name (3)", ...
//
// Pure: no ImGui, no files.
#pragma once

#include "regex_state.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace RegexBookmarksShare {

constexpr const char* kKind = "regex-bookmarks";   // bookmarks-share.ts:20 BOOKMARK_PACK_KIND
constexpr int kVersion = 1;                          // bookmarks-share.ts:21 BOOKMARK_PACK_VERSION

// bookmarks-share.ts:23 BookmarkPack. Every bookmark has game poe1 / poe2 and no hotkey.
struct Pack {
	std::vector<RegexBookmarkFolder> folders[2];   // [0] poe1, [1] poe2
	std::vector<RegexBookmark> bookmarks;
};

struct Normalized {
	Pack pack;
	std::vector<std::string> warnings;   // the TS wording, in its order
};

// bookmarks-share.ts:52 canonicalBookmarkPackJson (JSON.stringify layout, fixed key
// order, empty optional fields left out, no hotkey).
std::string CanonicalJson(const Pack& p);
// bookmarks-share.ts:64 encodeBookmarks
std::string Encode(const Pack& p);
// bookmarks-share.ts:69 normalizeBookmarkPack over JSON text. False + *err where
// the TS throws (not an object, kind, version) or the text is not JSON
// ("書籤包內容不是 JSON", share.ts:240).
bool NormalizeJson(const std::string& jsonText, Normalized& out, std::string* err);
// bookmarks-share.ts:112 decodeBookmarks (share.ts:230 gunzipBase64url with label 書籤包).
bool Decode(const std::string& code, Normalized& out, std::string* err);

// bookmarks-share.ts:116 MergedBookmark / :125 MergeResult
struct Merged {
	std::string game;
	std::string folder;         // "" = uncategorised
	std::string name;           // after renaming
	std::string originalName;
};
struct MergeResult {
	RegexUiState state;         // the input with bookmarks / folders replaced
	std::vector<Merged> added;  // pack order
	std::vector<Merged> renamed;   // the renamed ones among `added`
	std::vector<std::pair<std::string, std::string>> foldersCreated;   // (game, name)
};
// bookmarks-share.ts:140 mergeBookmarks (does not change `s`; usable as a preview).
MergeResult Merge(const RegexUiState& s, const Pack& p);

// ---- PobTools: choosing what to send ------------------------------------------
//
// The "選擇要傳送的書籤" dialog's model. A group is one game's folder, or its
// uncategorised bookmarks (folder ""), exactly as RegexFolders::GroupBookmarks
// lists them. Bookmarks are ticked one by one (index into s.bookmarks); a folder
// ticked as a whole ticks all of its bookmarks. The folder mark itself only
// matters for an EMPTY folder (sent as a bare folder); every folder holding a
// ticked bookmark is sent anyway.
struct Selection {
	std::set<int> bookmarks;
	std::set<std::pair<std::string, std::string>> folders;   // (game, folder) ticked as a whole
};
enum class Tri { None, Some, All };

Tri GroupTri(const RegexUiState& s, const Selection& sel, const std::string& game, const std::string& folder);
void SetGroup(const RegexUiState& s, Selection& sel, const std::string& game, const std::string& folder, bool on);
// Every group of both games.
void SetAll(const RegexUiState& s, Selection& sel, bool on);
Tri AllTri(const RegexUiState& s, const Selection& sel);

struct PackStats {
	int bookmarks[2] = {0, 0};   // sent, per game
	int folders[2] = {0, 0};
	int itemMod = 0;             // sent bookmarks on the item-mod values page (a count only: its keys are shared since 2026-10-09)
	int skipped = 0;             // ticked but unsendable (no game / no name / page / keys)
	bool Empty() const { return bookmarks[0] + bookmarks[1] + folders[0] + folders[1] == 0; }
};
// The pack for a selection: folders in each game's list order (collapsed state as
// here), then any a sent bookmark names that the list lacks; bookmarks in
// s.bookmarks order, hotkey dropped, folder names normalised.
Pack PackOf(const RegexUiState& s, const Selection& sel, PackStats* stats = nullptr);

} // namespace RegexBookmarksShare
