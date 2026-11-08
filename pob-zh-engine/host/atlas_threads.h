// PobTools atlas planner: 3.30 Threads of Creation (the map-device content that
// replaced scarabs) -- catalogue and placement rules.
//
// Pure data + rule logic, no ImGui / GL -- same split as atlas_scarabs.
//
// The rules come from the 3.30 reveal (keynote screenshots + the developer
// Q&A), NOT from game data yet: the map device has three thread slots, each
// holds one thread type with a count; a keystone limits the device to a single
// type and adds two to it. Assumptions still to be checked against the 3.30
// GGPK once it ships:
//   * the same type cannot occupy two slots (it would bypass the per-slot cap)
//   * a placed slot holds at least one thread
//   * the keystone's +2 applies only to the one allowed type
#pragma once

#include "atlas_persist.h" // ThreadSlot

#include <string>
#include <unordered_map>
#include <vector>

static const int kMaxThreadSlots = 3;
static const int kMaxThreadsPerSlot = 8;
static const int kKeystoneThreadBonus = 2;

struct ThreadDef {
	std::string id;        // language-neutral key (BaseItemTypes id)
	std::string en, zh;    // official names, both locales
	std::string art;       // "Art/2DItems/..." without the extension
	std::string mechanic;  // e.g. "Abyss", "Blight" -- informational
	// Two INDEPENDENT lists, never paired by position (see atlas_scarabs.h).
	std::vector<std::string> descEn, descZh;
};

// Map-device state the rules depend on but this module cannot see. The atlas
// tree decides it; the caller passes it in.
struct ThreadRules {
	bool singleTypeKeystone = false; // keystone allocated: one type only, +2 to it
};

enum class ThreadPlace {
	kOk,
	kUnknown,        // no such thread in this catalogue
	kFull,           // all kMaxThreadSlots slots are taken
	kDuplicate,      // this type already sits in another slot
	kKeystoneMixed,  // keystone allows one type and a different one is placed
};

// Highest count one slot may hold under `rules`.
int ThreadMaxCount(const ThreadRules& rules);
// `n` clamped into 1 .. ThreadMaxCount(rules).
int ThreadClampCount(int n, const ThreadRules& rules);

class ThreadDb {
public:
	// Reads exeDir\Data\threads_poe1.json. Missing or malformed is not fatal:
	// available() stays false and Sanitize() returns its input untouched, so a
	// saved build keeps its threads instead of being silently emptied.
	bool Load(const std::wstring& exeDir, std::string* err = nullptr);
	// Same parser on an in-memory document (self-test fixtures).
	// Format: {"source": "...", "threads": [{"id","en","zh","art","mechanic",
	//          "descEn":[...], "descZh":[...]}, ...]}
	bool LoadFromJson(const std::string& json, std::string* err = nullptr);

	bool available() const { return !defs_.empty(); }
	const std::vector<ThreadDef>& All() const { return defs_; }
	const ThreadDef* ById(const std::string& id) const;
	const std::string& Source() const { return source_; }

	// Would placing `id` into a NEW slot be legal? Checks run in the order
	// kUnknown, kDuplicate, kKeystoneMixed, kFull so the reason is the most
	// specific one.
	ThreadPlace CanPlace(const std::vector<ThreadSlot>& cur, const std::string& id,
	                     const ThreadRules& rules) const;

	// Force a list into a legal state, preserving order: drops unknown ids,
	// drops a type already seen (first one wins), under the keystone drops every
	// type but the first, truncates to kMaxThreadSlots, clamps every count with
	// ThreadClampCount. Unavailable catalogue = input returned unchanged. When
	// *note is given it receives a UTF-8 summary of what changed (empty when
	// nothing did).
	std::vector<ThreadSlot> Sanitize(const std::vector<ThreadSlot>& slots, const ThreadRules& rules,
	                                 std::string* note) const;

private:
	std::vector<ThreadDef> defs_;
	std::unordered_map<std::string, int> byId_;
	std::string source_;
};

// Headless checks, appended to --atlas-selftest. Returns the failure count and
// appends a report to `out`.
int RunThreadSelfTest(const std::wstring& exeDir, std::string& out);
