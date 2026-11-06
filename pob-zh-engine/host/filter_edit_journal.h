// PobTools filter editor: keep your edits across NeverSink updates.
//
// A NeverSink update replaces the whole .filter (a download overwrites it, a
// subscription is re-downloaded by the game), so edits to NeverSink's own
// rules would be lost. The journal remembers, per rule identity
// ("$type->... $tier->..."), what the rule looked like in NeverSink's file
// ("before") and what you made of it ("after"), plus the custom zone. Applying
// it to a newer file is a three-way merge per setting: what only you changed
// is taken, what only NeverSink changed is kept, and what both changed is a
// conflict you decide (your version by default).
//
// Pure data apart from JournalSave / JournalLoad (one JSON file).
#pragma once

#include "filter_model.h"
#include <map>
#include <set>
#include <string>
#include <vector>

class FilterDocumentEditor;

// "$type->currency $tier->t1exalted" for a NeverSink rule, "" for any other.
std::string FilterRuleKey(const FilterFile& f, const FilterBlock& b);
// The "# VERSION:" / "# TYPE:" values of NeverSink's banner ("" when absent).
std::string FilterBannerValue(const FilterFile& f, const char* field);

// A rule's own lines as text: header through its last syntax or "#!" line.
std::vector<std::string> FilterOwnLines(const FilterFile& f, const FilterBlock& b);

struct JournalRule {
	std::vector<std::string> before;   // NeverSink's version of the rule (the one `version` names)
	std::vector<std::string> after;    // yours
	std::string version;               // the NeverSink version "before" is from ("" = the journal's)
	bool pending = false;              // re-expressed on a newer version, not applied to it yet
};

struct EditJournal {
	std::string filterType;            // "2-SEMI-STRICT"
	std::string fromVersion;           // the NeverSink version the edits were made on
	std::map<std::string, JournalRule> rules;
	std::string customZone;            // the custom rules as a fragment ("" = none)
	std::string subscription;          // the game subscription this local copy follows ("" = none)
};

// At a save: every NeverSink rule that differs from the baseline (the file as
// loaded) is recorded -- a new entry takes the baseline as "before"; an
// existing one takes the current "after", and when the baseline is a newer
// NeverSink version and the rule is what applying gives, that version becomes
// its "before" (a rule not carried over yet stays pending); an entry whose
// "after" equals its "before" is dropped. The custom zone is
// stored whole. Also sets filterType / fromVersion from `current`.
// `carried`: rules an apply actually put on this file (JournalReport::carried) --
// those take the baseline as their new "before" whatever was chosen or tweaked
// since; without it, a rule counts as carried over only when it is what
// applying with the default choices gives.
void JournalRecord(EditJournal& j, const FilterFile& baseline, const FilterFile& current,
                   const std::set<std::string>* carried = nullptr);

struct JournalConflict {
	std::string key;                   // rule identity
	std::string setting;               // the line's identity in the rule ("SetTextColor")
	std::string mine, theirs;          // the two versions ("" = the line is absent)
};

struct JournalReport {
	int applied = 0;                   // rules changed to carry your edits
	int already = 0;                   // rules that already had them
	std::vector<JournalConflict> conflicts;
	std::vector<std::string> missing;  // keys the new file does not have
	int customImported = 0;            // custom rules put back
	std::set<std::string> carried;     // rules now carrying your edits (applied or already), for JournalRecord
};

// Choices for conflicts: "key|setting" -> true = take NeverSink's new version.
// Anything not listed keeps yours.
using JournalChoices = std::map<std::string, bool>;

// Merge the journal into the document's file (through `doc`, so undo covers
// it). With dryRun, only the report is computed and nothing changes. Applying
// twice gives the same file as applying once.
JournalReport JournalApply(const EditJournal& j, FilterDocumentEditor& doc, const JournalChoices& choices,
                           bool dryRun = false);

// JSON file (write: temp file + rename). False + *err on failure.
bool JournalSave(const std::wstring& path, const EditJournal& j, std::string* err);
bool JournalLoad(const std::wstring& path, EditJournal* j);

// ---- game subscriptions (OnlineFilters\) ------------------------------------

struct OnlineFilterHeader {
	bool online = false;               // the "#Online Item Filter" line is there
	std::string name;                  // "#name:NeverSink-5uberstr-softcore"
	std::string filterVersion;         // "#filterVersion:8.20.2.2026.309.7"
};
OnlineFilterHeader ParseOnlineHeader(const std::string& text);
// The text without the subscription's own header lines (the "#Online Item
// Filter" line and the "#key:value" lines right after it); everything else
// byte-for-byte.
std::string StripOnlineHeader(const std::string& text);
// A subscription's name made safe as a file name in one folder: path separators,
// the characters Windows forbids and control characters become '_', a ".."
// cannot remain, trailing dots and spaces go; "" becomes "subscription".
std::string SafeFilterFileName(const std::string& name);
