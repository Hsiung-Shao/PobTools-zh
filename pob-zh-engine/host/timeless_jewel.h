// Timeless Jewel calculator core (Phase 1: headless transformation engine).
//
// Ports Path of Building Community's Legion jewel logic (MIT) into PobTools.
// The compact dataset (host/data/timeless_jewels.json) is bundled; the large
// per-jewel lookup tables (<PoB>/Data/TimelessJewelData/*.bin) are read at
// runtime from the user's PoB folder — never shipped by us.
//
// Jewel types: 1 Glorious Vanity, 2 Lethal Pride, 3 Brutal Restraint,
//              4 Militant Faith, 5 Elegant Hubris, 6 Heroic Tragedy.
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

struct TJStatMod {
	std::string fmt = "d";
	int index = 1;
	double min = 0.0;
	double max = 0.0;
};

// One alternate passive (addition or replacement node/keystone).
struct TJEntry {
	std::string dn;   // display name (English)
	std::string dnZh; // display name (Chinese; empty -> fall back to dn)
	std::string id;
	std::vector<std::string> sd;          // stat description templates (English)
	std::vector<std::string> sdZh;        // parallel Chinese (baked; "" -> fall back to sd)
	std::vector<std::string> sortedStats; // stat keys, parallel to sd
	std::map<std::string, TJStatMod> stats;
	bool ks = false; // keystone
};

// One conqueror of a jewel: its keystone id (matches <type>_keystone_<id>),
// display name and the PoE-trade pseudo-stat id used for seed export.
struct TJConqueror {
	std::string id;      // "1" or a "1_v2" variant
	std::string name;    // English proper noun
	std::string nameZh;  // "" -> fall back to name
	std::string trade;   // explicit.pseudo_timeless_jewel_<name>
};

struct TJDataset {
	int additionsOffset = 96;
	std::map<int, std::string> types;
	std::map<int, std::string> binNames;               // jewelType -> LUT file stem when it
	                                                   // differs from the name (Abyss jewels)
	std::map<int, std::string> conqType;               // jewelType -> "maraketh" etc.
	std::map<int, std::vector<TJConqueror>> conquerors; // jewelType -> conquerors
	std::map<int, int> seedMin, seedMax;
	int size = 0, sizeNotable = 0;
	std::map<int, std::pair<int, int>> nodeIndex;       // nodeId -> (index, byteSize)
	std::map<int, std::map<int, int>> localToGlobal;    // jewelType -> (localId -> globalId)
	std::vector<TJEntry> additions;                     // 0-based (Lua i -> [i-1])
	std::vector<TJEntry> nodes;

	bool Load(const std::wstring& jsonPath, std::string* err);
	int L2G(int jewelType, int localId) const;
	bool HasNode(int nodeId) const { return nodeIndex.count(nodeId) != 0; }
};

// What a pasted jewel (game Ctrl+C text) resolved to; jewelType 0 = not found.
struct TJPaste {
	int jewelType = 0;
	int conqIndex = -1;
	int seed = -1;
};

// Identify jewel + conqueror + seed from copied item text. The conqueror name is
// unique across all jewels, so it pins both; the seed is the largest number on
// the line naming the conqueror. Matches English names first, then Chinese ones
// (zh clients copy translated text).
TJPaste TJParsePaste(const TJDataset& ds, const std::string& text);

// Render the jewel back out as the game's Ctrl+C item text, in English, so it
// can be pasted straight into PoB (its parser matches on the English wording).
// seed is the item seed — already x20 for Elegant Hubris, exactly as TJSearch
// reports it. Returns "" when the type/conqueror does not resolve.
std::string TJItemText(const TJDataset& ds, int jewelType, int conqIndex, int seed);

// Result of transforming a single passive node with a socketed jewel.
//
// `lines` holds ONLY what the jewel grants. When `replaced` is true those lines
// are the whole node; when it is false the jewel merely added to the node, so a
// caller showing the node must print the node's own stats first and these after
// — that is what PoB's NodeAdditionOrReplacementFromString does.
struct TJTransform {
	bool ok = false;
	bool replaced = false;            // node became a different notable/keystone
	std::string newName;              // replacement display name (English)
	std::string newNameZh;            // replacement display name (Chinese; "" -> use newName)
	std::vector<std::string> lines;   // stat lines the jewel grants (English)
	std::vector<std::string> linesZh; // parallel Chinese ("" -> fall back to English)
	std::string note;                 // diagnostics / errors
};

// Raw LUT bytes for (jewelType, seed, nodeId). binBlob = the entire .bin file.
std::vector<int> TJReadLUT(const TJDataset& ds, const std::string& binBlob,
                           int jewelType, int seed, int nodeId);

// ---- shared with the Abyss engine (timeless_jewel_abyss.cpp) ------------------

// Substitute a rolled value into one stat template, mirroring PoB's
// replaceHelperFunc — including the unit scaling it applies to "g"-format
// stats, whose rolls are stored in game units (per minute, permyriad, ms) and
// printed in display units.
std::string TJRollStat(const std::string& sd, const std::string& statKey,
                       const TJStatMod& m, double value);

// Entry lookup by global id. Replacements index `nodes` (offset by the addition
// count, as PoB's `legion.nodes[id + 1 - timelessJewelAdditions]` does);
// additions index `additions` directly. Both return nullptr when out of range.
const TJEntry* TJNodeAt(const TJDataset& ds, int globalId);
const TJEntry* TJAdditionAt(const TJDataset& ds, int globalId);

// Transform one node. nodeType is "Notable" / "Keystone" / "Normal".
// origSd is the node's original stat lines (used by Normal/GV rolls).
TJTransform TJApply(const TJDataset& ds, const std::string& binBlob,
                    int jewelType, int seed, int nodeId, const std::string& nodeType,
                    const std::vector<std::string>& origSd,
                    const std::string& conquerorType = "", const std::string& conquerorId = "",
                    const std::string& nodeName = "");

// ---- seed search (batch query) ------------------------------------------------

// Normalize a rolled stat line to a template by turning number runs into '#'
// (e.g. "20% increased Damage with Poison" -> "#% increased Damage with Poison").
std::string TJNormalizeStat(const std::string& line);

// A desired stat and its scoring.
struct TJWantStat {
	std::string tmpl;       // normalized template to match
	double minValue = 0.0;  // ignore matches whose rolled value is below this
	double weight = 1.0;    // added to a seed's score per matching node
};

// The number a rolled line contributes, for the per-stat minimum. This is a
// magnitude WITHIN the line's own template, not a signed quantity: a leading '-'
// survives TJNormalizeStat ("-#% to all maximum Elemental Resistances"), so the
// sign belongs to the identity of the stat and the digits after it are what
// actually varies. Reading it as signed would make the one reachable negative
// keystone fail its own default minimum of 0.
double TJStatValue(const std::string& line);

// Decides whether one rolled line satisfies one of the wanted stats — template
// equal AND value at least minValue.
//
// This is deliberately the ONLY implementation of that rule. The search and
// every place the UI shows or highlights a line as a hit both go through it, so
// what the user sees marked as matched is exactly what the ranking counted.
// They used to be written separately and the display side ignored minValue
// entirely, which showed under-minimum rolls as hits.
class TJWantMatcher {
public:
	TJWantMatcher() = default;
	explicit TJWantMatcher(const std::vector<TJWantStat>& wants);

	// The wanted stat this line satisfies, or nullptr. The pointer belongs to
	// the matcher and stays valid for its lifetime.
	const TJWantStat* Match(const std::string& line) const;
	bool empty() const { return wants_.empty(); }

private:
	std::vector<TJWantStat> wants_;              // owned copy: safe to outlive the caller's
	std::unordered_map<std::string, int> byTmpl_; // template -> index into wants_
};

struct TJSearchQuery {
	int jewelType = 3;
	int scope = 1;                    // 0 = all indexed, 1 = notables only, 2 = smalls only
	std::vector<TJWantStat> wants;
	double minTotalWeight = 0.0;
	// Every wanted stat must appear on the seed, not just enough of ANY of them
	// to clear the weight. Summing weight alone lets a seed carrying one stat
	// three times score the same as a seed carrying all three, which is not what
	// picking several stats means. Turn off for an "any of these" search.
	bool requireAll = true;
	// Restrict evaluation to these node ids (a jewel socket's in-radius nodes).
	// Empty = every indexed node in scope (global search). Node type (notable vs
	// normal) is derived from the dataset, so smalls are rolled correctly too.
	std::vector<int> nodeIds;
};

struct TJSeedHit {
	int seed = 0;         // the item seed (already *20 for Elegant Hubris)
	double weight = 0;    // total matched weight across nodes
	int matches = 0;      // number of matching node-stats
	int distinctWants = 0; // how many distinct wanted stats this seed satisfies
};

// Scan the whole seed range and rank seeds by matched weight. `progress`, if set,
// is polled to allow cancellation (search runs on a worker thread in the UI).
std::vector<TJSeedHit> TJSearch(const TJDataset& ds, const std::string& blob,
                                const TJSearchQuery& q, int topN,
                                const volatile bool* cancel = nullptr);

// A pickable stat template, English (search key) + Chinese (display).
struct TJStatTemplate {
	std::string en; // normalized English template ("#% increased ...") — the match key
	std::string zh; // normalized Chinese template for display ("" -> use en)
};

// Unique stat templates a jewel can produce (for the UI picker), sorted by zh/en.
// jewelType 1-6 restricts to that jewel's own additions/keystones (matched by the
// conqueror-type id prefix); 0 returns the union across all jewels.
std::vector<TJStatTemplate> TJStatTemplates(const TJDataset& ds, int jewelType = 0);

// Load the whole .bin for a jewel type from the detected PoE1 PoB install
// (FindPoe1Dir): <pob dir>\Data\TimelessJewelData\<Name>.bin.
bool TJLoadBin(const std::wstring& exeDir, const TJDataset& ds, int jewelType,
               std::string& out, std::string* err);

// CLIs.
int RunTimelessJewelSelfTest(const std::wstring& exeDir);           // --tj-selftest
int RunTimelessJewelCli(const std::wstring& exeDir, int jewelType,  // --tj <t> <seed> <node>
                        int seed, int nodeId);
int RunTimelessJewelVerify(const std::wstring& exeDir);            // --tj-verify
