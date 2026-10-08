// What the search-string tool remembers between runs: PobTools\regex_ui.json.
//
// Two things live here and they are not the same kind of thing.
//
//   * the CURRENT state -- which list, which mode, what is ticked. Restored on
//     open so closing the window is not the same as throwing the work away.
//   * BOOKMARKS -- named selections the player saved on purpose. This is real
//     user data: it is not derivable from anything else, nobody else writes it,
//     and losing it silently would be the worst failure this file can have. It
//     is therefore written through a temporary file and moved into place, and a
//     bookmark that no longer resolves is REPORTED rather than dropped.
//
// ---- why the key is the English line ----------------------------------------
//
// A selection has to survive a new league's data regeneration, so it cannot be
// stored as a row number -- the list reorders and lengthens every patch. It is
// stored as the entry's ENGLISH line, which is the language-neutral identity of
// a printed modifier: the Chinese wording can be revised by our own pipeline
// without the player's bookmarks moving, and the same key would work if a third
// locale were added. Chinese is kept as a fallback for the rarer case where GGG
// rewords the English and leaves the translation alone. Algorithmic pages (the
// numeric section, the vendor page) key their rows by the entry id instead.
//
// ---- schema 5 (R5, ported from exile-appraiser regex/src/state.ts) ---------
//
// The file format is exile-appraiser's regex_state.json schema 5, field for
// field, so a state / bookmark file from there loads here as is:
//
//   current[{page, keys, alt, num?}]   num = the numeric section's ticked ids,
//                                      kept on the HOST page's record
//   numeric{storeKey:{entryId:{min?,max?,choice?}}}
//                                      storeKey = page id; a section's values
//                                      live under its host page id
//   custom[] / excludes[] / outScope   merge: custom terms, excludes, output scope
//   collapsed[]                        host page ids whose numeric section is folded
//   bookmarks[{name,page,game,mode,lang,keys,alt,numeric?,num?,hotkey?,folder?}]
//                                      a bookmark is the whole page (corpus ticks
//                                      + section ticks + their values)
//   folders{poe1:[{name,collapsed}],poe2:[...]} / uncatCollapsed[]
//                                      one level of bookmark folders per game
//
// PobTools only adds `panelView` ("combined"; written only when not the default
// "page"), which exile-appraiser ignores.
//
// Reading: schema 1 (this tool's old file) and exile-appraiser's 2-4 all load;
// schema <= 2's separate numeric pages (map_numeric / waystone_numeric) are
// moved onto their host page (MigrateSections). The FIRST save over an older
// file copies it to regex_ui.json.bak-s<N> (never overwriting an existing one).
// A file from a NEWER schema is read for display but never written over: Save
// refuses and the panel says so (SaveBlocked).
#pragma once

#include "regex_frag.h"

#include <string>
#include <utility>
#include <vector>

// entry id -> value, in file order (state.ts keeps JS object insertion order,
// and a byte-identical round trip with exile-appraiser needs the same).
using RegexValueList = std::vector<std::pair<std::string, RegexFrag::AlgoValue>>;

// Find / set in a RegexValueList (set appends a new id at the end, like JS).
const RegexFrag::AlgoValue* RegexValueFind(const RegexValueList& m, const std::string& id);
void RegexValueSet(RegexValueList& m, const std::string& id, const RegexFrag::AlgoValue& v);

struct RegexBookmark {
	std::string name;
	std::string page;                  // page id, e.g. "map_mods"
	// "poe1" / "poe2". Derivable from the page id today, and stored anyway: the
	// bookmark list is filtered by game, and a bookmark whose page has since
	// been retired would otherwise have nowhere to be shown -- which is how
	// user data disappears without anyone deciding to delete it.
	std::string game;
	std::string mode = "any";          // any | all | none
	// Which language the query was built from when this was saved. Stored so
	// loading a bookmark gives back the string the player actually copied: the
	// same picks in the other language are a different set of tokens.
	std::string lang = "zh";           // zh | en
	std::vector<std::string> keys;     // English lines (algorithmic page: entry ids)
	std::vector<std::string> alt;      // Chinese lines, same order; the fallback
	// Algorithmic page: entry id -> value of its ticked rows. Host page: the
	// numeric section's values (schema 2/3). Empty = not written.
	RegexValueList numeric;
	// Host page: the numeric section's ticked entry ids (schema 3). Empty = the
	// section is unticked when this loads (a bookmark is the whole page).
	std::vector<std::string> num;
	// exile-appraiser's per-bookmark hotkey (schema 4). PobTools has no hotkeys
	// and no paste-into-game; the string is kept verbatim and written back so a
	// file shared with exile-appraiser does not lose it. Nothing here reads it.
	std::string hotkey;
	// Folder name (schema 5); empty = uncategorised.
	std::string folder;
};

// What was ticked on one page.
struct RegexPagePicks {
	std::string page;
	std::vector<std::string> keys;
	std::vector<std::string> alt;
	std::vector<std::string> num;      // host page: the numeric section's ticked ids
};

struct RegexBookmarkFolder {
	std::string name;
	bool collapsed = false;
};

constexpr int kRegexStateSchema = 5;

struct RegexUiState {
	std::string game;                  // "poe1" / "poe2"; empty = use the launcher's
	std::string page;                  // the list that was showing
	std::string mode = "any";
	std::string lang = "zh";           // which language the query is built from
	bool bilingual = true;             // show the other language under each row
	std::vector<RegexPagePicks> current;
	std::vector<RegexBookmark> bookmarks;
	// Algorithmic values: store key (page id; section -> host id) -> values.
	std::vector<std::pair<std::string, RegexValueList>> numeric;
	std::vector<std::string> custom;   // merge: custom terms (unverified)
	std::vector<std::string> excludes; // merge: words joined into the "!" term
	std::string outScope = "combined"; // combined | page
	std::vector<std::string> collapsed;   // host page ids with the section folded
	std::vector<RegexBookmarkFolder> folders[2];   // [0] poe1, [1] poe2
	std::vector<std::string> uncatCollapsed;       // games whose "uncategorised" is folded
	std::string panelView = "page";    // PobTools only: page | combined
	// PobTools only: the ExileAppraiser.exe the user picked by hand (regex_send.h);
	// written only when set, so the file stays schema 5 and exile-appraiser's
	// state.ts (which drops keys it does not know) is unaffected.
	std::string exileAppraiserExe;

	// ---- file ----
	// The schema the loaded file declared: 0 = no file / no "schema" key.
	int loadedSchema = 0;
	bool loadedFile = false;           // a file was read and parsed
	// Both return false on a missing or unreadable file; a fresh install is not
	// an error and the caller carries on with the defaults above.
	bool Load(const std::wstring& exeDir);
	// Refuses (false, file untouched) when SaveBlocked(). Before the first write
	// over an older-schema file, copies it to regex_ui.json.bak-s<N>.
	bool Save(const std::wstring& exeDir);
	// The loaded file came from a newer schema than this build writes.
	bool SaveBlocked() const { return loadedSchema > kRegexStateSchema; }

	// state.ts parseRegexState / serializeRegexState, no file IO. Parse resets
	// everything first; false = not JSON / wrong types (state = defaults).
	bool Parse(const std::string& text);
	std::string Serialize() const;

	// The picks for one page, created if absent.
	RegexPagePicks& PicksFor(const std::string& pageId);
	// numeric[key], created (appended) if absent.
	RegexValueList& NumericFor(const std::string& key);
	const RegexValueList* NumericOf(const std::string& key) const;
	std::vector<RegexBookmarkFolder>& Folders(const std::string& game);
	const std::vector<RegexBookmarkFolder>& Folders(const std::string& game) const;
};

// state.ts regexStateSchemaOf: the "schema" a text declares (0 = none).
int RegexStateSchemaOf(const std::string& text);

// state.ts migrateSections (schema <= 2 -> 3, in place): records keyed by a
// numeric page id move onto the host page. Returns whether anything changed.
bool RegexMigrateSections(RegexUiState& s);

// Path helpers (selftest inspects the backup).
std::wstring RegexStatePath(const std::wstring& exeDir);
std::wstring RegexStateBackupPath(const std::wstring& exeDir, int schema);

// Turn saved keys back into ticks. `entryKeys` / `entryAlt` are the English and
// Chinese lines of the page's entries, in row order; `picked` comes back the same
// length, one byte per row.
//
// The return value is how many saved keys resolved to NOTHING, and it is the
// reason this is a function rather than a loop inside the panel: a bookmark that
// quietly comes back three modifiers short looks exactly like a bookmark the
// player mis-saved, and the only way to tell the two apart is to count. Plain
// string vectors on purpose -- this file must not learn what an entry is.
int RegexResolveKeys(const std::vector<std::string>& keys,
                     const std::vector<std::string>& alt,
                     const std::vector<std::string>& entryKeys,
                     const std::vector<std::string>& entryAlt,
                     std::vector<char>& picked);
