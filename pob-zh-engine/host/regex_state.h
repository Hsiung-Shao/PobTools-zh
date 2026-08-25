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
// rewords the English and leaves the translation alone.
#pragma once

#include <string>
#include <vector>

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
	std::vector<std::string> keys;     // English lines
	std::vector<std::string> alt;      // Chinese lines, same order; the fallback
};

// What was ticked on one page.
struct RegexPagePicks {
	std::string page;
	std::vector<std::string> keys;
	std::vector<std::string> alt;
};

struct RegexUiState {
	std::string game;                  // "poe1" / "poe2"; empty = use the launcher's
	std::string page;                  // the list that was showing
	std::string mode = "any";
	std::string lang = "zh";           // which language the query is built from
	bool bilingual = true;             // show the other language under each row
	std::vector<RegexPagePicks> current;
	std::vector<RegexBookmark> bookmarks;

	// Both return false on a missing or unreadable file; a fresh install is not
	// an error and the caller carries on with the defaults above.
	bool Load(const std::wstring& exeDir);
	bool Save(const std::wstring& exeDir) const;

	// The picks for one page, created if absent.
	RegexPagePicks& PicksFor(const std::string& pageId);
};

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
