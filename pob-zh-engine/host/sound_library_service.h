// PobTools sound library service (音效管理), modelled on POE-Filter-Audio-Manager:
// scan a folder of alert sounds, preview them, manage naming rules (saved to
// Data\sound_rules.json), batch-rename by rule with a dry-run plan, and keep the
// open filter's CustomAlertSound references in sync when files are renamed.
//
// UI-free data layer shared by the 音效管理 section and the CustomAlertSound
// card's sound-library popup. Folder persistence stays in pob-zh.ini
// (Get/SetSoundFolder from sound_manager.h).
#pragma once

#include "filter_doc_editor.h"
#include <string>
#include <vector>

struct SoundFileInfo {
	std::wstring name;                 // file name only
	unsigned long long size = 0;
};

// One naming rule: files whose name contains `match` (case-insensitive, empty =
// manual-only) rename to `rename`, where {n} is a running number and {ext}
// keeps the original extension.
struct NamingRule {
	std::string name;                  // display name (e.g. 神聖石音效)
	std::string match;                 // substring filter for auto-apply
	std::string rename;                // target template, e.g. "divine{n}.{ext}"
	bool enabled = true;
};

struct RenamePlanEntry {
	std::wstring oldName, newName;
	enum class State { Rename, Unchanged, Conflict } state = State::Unchanged;
	enum class Resolution { Unset, Skip, Suffix, Swap } resolution = Resolution::Unset;
	std::vector<int> refLines;         // CustomAlertSound lines referencing oldName
};

// Rules <-> json text (pure, for tests and Save/LoadRules).
std::string SoundRulesToJson(const std::vector<NamingRule>& rules);
bool SoundRulesFromJson(const std::string& text, std::vector<NamingRule>* out);

// Line indices of CustomAlertSound / CustomAlertSoundOptional whose value's last
// path component equals fileName (case-insensitive).
std::vector<int> FindCustomSoundRefs(const FilterFile& f, const std::wstring& fileName);

// 替換引用: rewrite every CustomAlertSound reference of oldName to newName
// (path prefix and volume preserved). Disk files untouched; the model is
// marked dirty, NOT saved. Returns the number of rewritten lines.
int ReplaceSoundRefs(FilterDocumentEditor* doc, const std::wstring& oldName,
                     const std::wstring& newName);

class SoundLibraryService {
public:
	// Reads the folder from pob-zh.ini and the rules from Data\sound_rules.json.
	void Init(const std::wstring& exeDir);

	const std::wstring& folder() const { return folder_; }
	void SetFolder(const std::wstring& folder);   // persists to ini + rescans
	void Rescan();
	const std::vector<SoundFileInfo>& files() const { return files_; }

	std::vector<NamingRule>& rules() { return rules_; }
	bool SaveRules(std::string* err = nullptr);

	// Dry-run: apply every enabled rule with a non-empty match to the scanned
	// files. No disk access beyond the existing scan. openFilter may be null.
	std::vector<RenamePlanEntry> BuildRenamePlan(const FilterFile* openFilter) const;

	// Dry-run for renaming one file to an explicit new name.
	RenamePlanEntry BuildSingleRename(const std::wstring& oldName, const std::wstring& newName,
	                                  const FilterFile* openFilter) const;

	struct ApplyResult {
		int renamed = 0, skipped = 0, swapped = 0, refsUpdated = 0;
		std::string err;               // non-empty on any failure (partial results kept)
	};
	// Executes the plan. Refuses (err set, nothing touched) while any Conflict
	// entry is still Resolution::Unset. Stops audio first (MCI file locks).
	// When syncDoc is non-null, refLines values are rewritten to the new name
	// (directory prefix preserved) and the model is marked dirty — NOT saved.
	ApplyResult ApplyRenamePlan(std::vector<RenamePlanEntry>& plan, FilterDocumentEditor* syncDoc);

private:
	std::wstring exeDir_, folder_;
	std::vector<SoundFileInfo> files_;
	std::vector<NamingRule> rules_;
};
