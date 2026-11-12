/*
** translation_manager.cpp - JSON translation loader
**
** Loads translations from Data/Translate/poe1/{locale}/ directories.
** JSON format (flat):       { "entries": { "key": "value", ... } }
** JSON format (structured): { "entries": { "key": { "翻譯": "value", "色碼標籤": "^7", ... }, ... } }
** Additional config: meta.json, synonyms.json, item_metadata.json
**
** Locale: env POB_LOCALE, else the first entry of Data/Translate.json, else passthrough.
*/

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fstream>
#include <iterator>
#include <sstream>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include "translation_manager.h"
#include "game_rules.h"
#include "../host/error_log.h"
#include "../host/hang_watch.h"
#include "startup_trace.h"

/* nlohmann/json for structured JSON translation loading */
#include <json.hpp>
using njson = nlohmann::json;

/* ========== Internal State ========== */

/* 樣板配對的產物是填值計畫而非雜湊字串:'#' 留不住 {N} 編號,編號是中英語序不同時唯一的對位依據。
**   text  目標樣板(待填處仍是 '#',填不出來的字面數字已直接寫回去)
**   src   text 裡第 i 個 '#' 要取來源的第 src[i] 格;-1 = 沒有來源,留 '#' */
struct FillPlan {
    std::string      text;
    std::vector<int> src;
};

static std::unordered_map<std::string, std::string> s_translations;      /* English → Chinese */
static std::unordered_map<std::string, std::string> s_reverse;           /* Chinese → English (exact) */
static std::unordered_map<std::string, FillPlan>    s_reverse_pattern;   /* Chinese pattern → English fill plan */
/* Which file supplied the entry that currently OWNS each reverse key (both maps are last-wins
** across the eight dictionaries). Always populated: needed the moment something looks wrong. */
static std::unordered_map<std::string, std::string> s_reverse_origin;
static std::unordered_map<std::string, std::string> s_reverse_pattern_origin;
static std::unordered_map<std::string, FillPlan>    s_forward_pattern;   /* English pattern → Chinese fill plan */
/* s_forward_pattern is keyed lower-case, so keys differing ONLY in case share one slot. For such
** collisions each case form also keeps its own plan here, keyed case-preserved
** (find_forward_pattern). s_forward_pattern_case_of: lower-case key -> case form of its plan. */
static std::unordered_map<std::string, FillPlan>    s_forward_pattern_cased;
static std::unordered_map<std::string, std::string> s_forward_pattern_case_of;
static std::unordered_map<std::string, std::string> s_reverse_bases;    /* Chinese base type → English (from items.json base entries) */
/* Forward twin of s_reverse_bases: only recognises POB's generated "<title>, <base type>" item names
** (comma-list path in translation_lookup). */
static std::unordered_set<std::string> s_base_items_en;
static std::vector<std::pair<std::string, std::string>> s_term_glossary; /* sorted longest-first for substring replacement */
static std::unordered_map<std::string, std::string> s_lookup_cache;     /* forward lookup cache (input → result, empty = no match) */
/* Item-title results are cached apart from s_lookup_cache: the relaxed answer must not leak into
** the general path (translation_lookup_item_title). */
static std::unordered_map<std::string, std::string> s_item_title_cache;
static std::string s_locale;
static bool s_initialized = false;
static bool s_translation_enabled = true;

/* 載入耗時統計(毫秒)。字典在背景執行緒建(見 translation_init_async),所以真正
** 落在 POB 啟動關鍵路徑上的只有 wait_ms:主執行緒第一次要用字典時實際等了多久。
** total/parse/build 是 worker 自己的工時;--tr 探針與引擎 console 都印。 */
static struct {
    double parse_ms = 0;   /* nlohmann::json::parse 累計 */
    double build_ms = 0;   /* 逐筆建表(正向/反向/樣板)累計 */
    double total_ms = 0;   /* translation_init 全程 */
    double wait_ms  = 0;   /* 主執行緒第一次 wait_ready 實際阻塞的時間 */
    int    files    = 0;
    bool   async    = false;
} s_init_stats;
using tr_clock = std::chrono::steady_clock;
static double tr_ms_since(tr_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(tr_clock::now() - t0).count();
}

/* ========== 背景載入 ==========
**
** 三態:s_initialized = 已開始,s_ready = 已完成。
** 執行緒物件放堆上而非 static std::thread:sys_main_c::Error() 走 ExitProcess 不經 shutdown,
** 還 joinable 的 static thread 會在 DLL 靜態解構時 std::terminate。
** worker 只能用絕對路徑:主執行緒在 Lua 載入期會反覆 SetWorkDir。 */
static std::atomic<bool> s_ready{true};   /* 沒有 async 在跑時恆為 true */
static std::thread *s_init_thread = nullptr;

static void do_init(void);

/* ========== Source dictionaries ==========
**
** The lookup map is flat and merged in meta.json load order, so the LAST file to define a key
** wins everywhere. That breaks when the same bare English word means different things by where
** POB read it (support gem "Volatility" vs the passive of that name; 65 keys collide in poe1).
** Load order cannot fix it, so POB tells us WHERE it read the string from (PobToolsSetSource)
** and that file's own dictionary is consulted first: exact entries, then its own value
** patterns, both ahead of the merged exact map. Nothing is marked by default.
*/
struct SourceDict {
    const char *name;   /* what Lua passes to PobToolsSetSource */
    const char *file;   /* the dictionary file backing it */
};
static const SourceDict kSourceDicts[] = {
    { "gems",     "gems.json" },
    { "passives", "passives.json" },
    { "affixes",  "affixes.json" },
    { "stats",    "stats.json" },
    { "uniques",  "uniques.json" },
    { "items",    "items.json" },
    /* Listed only in meta.json "source_dicts": loaded (with source_lookup) as source tables,
    ** never merged -- their wording differs from the merged map on purpose. */
    { "anoint",   "anoint.json" },
    { "legion",   "legion.json" },
    { "tattoo",   "tattoo.json" },
    /* GGPK gem display names without the "Support" suffix -- what a stat's
    ** gem NAME slot needs ("...Supported by level {2} {1}" already says 輔助). */
    { "gem_names", "gem_names.json" },
    /* Same, split by gem kind; stats_by_id.json "name_slots" may name these
    ** directly (a non-active support's name, an indexable support's name). */
    { "gem_names_nonactive", "gem_names_nonactive.json" },
    { "gem_names_support",   "gem_names_support.json" },
    /* GGPK words-table wording for title words whose bare English is ALSO a UI
    ** string in clientstrings ("Keep" = 堅城 in a rare name, 保留 as a button).
    ** Read only by translate_title_words, ahead of the merged map. */
    { "title_words", "title_words.json" },
};

static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> s_sources;

/* Id-keyed dictionaries: same English, different id, different wording. Keys are ids, never
** English, so these files feed ONLY this table (not the merged map, not the source exact or
** pattern maps), reachable only through translation_lookup_by_id. Listed in meta.json
** "source_dicts" (load_order also works), keyed by the same source names. */
static const SourceDict kIdDicts[] = {
    { "passives", "passives_by_id.json" },   /* GraphId -> name */
    { "affixes",  "affixes_by_id.json" },    /* mod Id  -> affix name */
    { "gems_desc", "gems_desc_by_id.json" }, /* grantedEffect id -> gem description */
    /* Drop-down rows by language-independent id. The two *_stats files hold no Chinese: their value is the GGPK stat ids
    ** (space separated) the inject hands to translation_lookup_stat_row /
    ** PobToolsTranslateStat for that mod's / passive's lines. */
    { "mod_stats", "mod_stats_by_id.json" },         /* Mods.Id -> stat ids */
    { "passive_stats", "passive_stats_by_id.json" }, /* PassiveSkillGraphId -> stat ids */
    { "gem_names", "gem_names_by_id.json" },         /* GrantedEffects.Id -> gem name */
    { "ascendancy", "ascendancy_by_id.json" },       /* Ascendancy.Id -> ascendancy name */
    /* No "legion" id table: GGPK's Id STRINGS have the two Faith notables swapped, so naming by
    ** id would be wrong; legion names go through the English "legion" source table. */
};
static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> s_id_dicts;
/* source -> (id -> whitespace-normalised GGPK English), from the id file's optional "templates".
** A source present here (even empty) had a templates field: its id answers are English-checked.
** Kept out of s_id_dicts so translation_debug_sources is unchanged. */
static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> s_id_templates;

/* stats_by_id.json: GGG stat descriptions keyed "stat_ids|cond|src_file|dup_ix", Chinese
** templates in "entries", English in "templates". Feeds ONLY its own index (s_stat_index) --
** never the merged map, reverse/pattern tables or s_id_dicts -- reachable only through
** translation_lookup_stat. Built only when meta.json has "source_lookup". */
static const char *const kStatDictFile = "stats_by_id.json";
static const char *const kStatFileItem = "stat_descriptions.txt";
static const char *const kStatFilePassive = "passive_skill_stat_descriptions.txt";
/* Aura-type passives (nearby enemies...) live only here. */
static const char *const kStatFilePassiveAura = "passive_skill_aura_stat_descriptions.txt";

/* Every description file in stats_by_id.json gets an id; a ctx turns into a CHAIN of file ids,
** most specific first (stat_ctx_chain):
**   item          -> stat_descriptions
**   passive       -> passive_skill -> stat_descriptions -> passive_skill_aura
**   gem           -> skill_stat -> active_skill_gem -> gem_stat -> stat_descriptions
**                    (POB scopeList order for an active skill)
**   gem:<scope>   -> <scope>.txt and its parents ("stat_desc_parents"); the gem default when
**                    the scope is unknown or the data pack predates it.
** The first file with a candidate decides (an exact-case match is preferred over all). */
static std::vector<std::string> s_stat_files;                  /* id -> basename */
static std::unordered_map<std::string, int> s_stat_file_ids;   /* basename -> id */
static std::unordered_map<std::string, std::string> s_stat_parents;  /* child -> parent */
static int stat_file_id(const std::string &name) {
    auto it = s_stat_file_ids.find(name);
    if (it != s_stat_file_ids.end()) return it->second;
    const int id = (int)s_stat_files.size();
    s_stat_files.push_back(name);
    s_stat_file_ids.emplace(name, id);
    return id;
}
static const char *const kStatGemDefaultChain[] = {
    "skill_stat_descriptions.txt",
    "active_skill_gem_stat_descriptions.txt",
    "gem_stat_descriptions.txt",
    "stat_descriptions.txt",
};
struct StatCand {
    int         file;      /* description file id (s_stat_files) */
    /* The index key WITHOUT case folding: a candidate whose English matches
    ** the line's case exactly beats one that only matches case-insensitively
    ** ("Chance To Bleed" vs "Chance to Bleed", "Taken" vs "taken"). */
    std::string ckey;
    std::vector<std::string> ids;  /* the key's stat_ids */
    /* Per English slot (same index as the line's values): 1 = a "{N:+d}"
    ** placeholder, i.e. the game prints that value with its sign. */
    std::vector<char> plus_slot;
    int         nstats;    /* stat_ids count (space separated) */
    FillPlan    plan;      /* English {N} -> Chinese {N}; text has real newlines */
    /* Numbers the English template writes out ("per 10 Devotion"): slot index
    ** in the line's values -> literal. A line with another number there is a
    ** different stat that happens to hash the same. */
    std::vector<std::pair<int, std::string>> lits;
};
/* normalised English template -> candidates */
static std::unordered_map<std::string, std::vector<StatCand>> s_stat_index;

/* Stat entries with NAME slots (stats_by_id.json "name_slots": {key: {"N": "passives"|"gems"}}):
** a {N} filled by a passive / skill NAME, not a number. A name is not a '#', so they cannot
** live in the hash index; they are matched token by token (stat_match_named) from here. */
struct StatNamedTok {
    std::string lit;   /* literal text, or empty for a placeholder */
    int         ph;    /* placeholder number, -1 for a literal */
};
struct StatNamedCand {
    int         file;      /* description file id (s_stat_files) */
    std::vector<std::string> ids;
    std::vector<StatNamedTok> toks;            /* the English template */
    std::string zh;                            /* Chinese template, real newlines */
    std::unordered_map<int, std::string> name_src;  /* {N} -> "passives" / "gems" */
};
static std::vector<StatNamedCand> s_stat_named;

/* ggpk_groups.json: every GGPK description whose English has more than one line -- "stats"
** (same keys as stats_by_id.json) and "texts" such as flavour text. POB draws one tooltip
** line per English line, so a per-line lookup only translates fragments. The catalog says
** which consecutive lines are ONE description; the whole is translated at once
** (translation_group_lines) and laid out the way GGPK's Chinese is.
**   stats -> joined lines go through the stat lookup (the catalog only says where a
**            description starts and how many lines it spans)
**   texts -> the catalog's Chinese, only where its line count differs from the English
**            (same count: the per-line dictionaries and their rulings stay in charge)
** Never merged; loaded by name when meta.json has "source_lookup". */
static const char *const kGroupFile = "ggpk_groups.json";
struct GroupStat { std::vector<std::string> keys; };   /* stat_index_key per English line */
struct GroupText { std::vector<std::string> lines; std::string zh; };   /* normalised English lines */
/* first line's key -> candidates (longest first) */
static std::unordered_map<std::string, std::vector<GroupStat>> s_group_stat_heads;
static std::unordered_map<std::string, std::vector<GroupText>> s_group_text_heads;
/* slash_group_lookup's answers by ctx + '\x1f' + input ("" = no answer). Capped at
** kSlashCacheMax entries (emptied before the next insert), so an answer pointer lives only
** until the next translation_lookup; callers copy before looking up again. */
static std::unordered_map<std::string, std::string> s_slash_group_cache;
static const size_t kSlashCacheMax = 4096;
/* translation_set_slash_context: the ctx the "/" lookup asks the stat lookup
** under ("" = "item", the crafting rows). */
static std::string s_slash_ctx;
static int group_lines_core(const std::vector<std::string> &in, const char *ctx,
                            std::vector<std::string> &out, std::vector<char> &hit);

/* display_exclude.json:
**   {"version":1,"entries":{"stats.json":["<原字典鍵>",...],"ui.json":[...]}}
** A listed key of THAT file never reaches a forward table (merged exact map, merged forward
** patterns, per-line entries derived into them, that file's source copy -- a source-marked
** lookup of an excluded key falls back to the merged table) nor the glossary. The same file
** cannot bring it back another way: an entry whose [X|Y]-stripped key is listed, or a line
** derived from an unlisted multi-line key that is itself listed, is kept out too.
** Still built: its reverse entries (paste Chinese -> English) and s_base_items_en
** ("<rare name>, <base>" titles depend on it). Keys are raw dictionary keys (before any
** 色碼標籤 prefix). Loaded by name like synonyms.json; absent file = nothing excluded. */
static const char *const kDisplayExcludeFile = "display_exclude.json";
static std::unordered_map<std::string, std::unordered_set<std::string>> s_display_exclude;

/* exact_only.json:
**   {"version":1,"entries":{"ggpk_full.json":["<原字典鍵>",...]}}
** A listed key of THAT file is matched as the whole sentence only. It enters the exact tables
** as usual (merged exact map, source copy, reverse exact map) but nothing is generalised from
** it: no forward pattern, no reverse / compact / fuzzy reverse pattern, no per-line entries,
** and the glossary skips it -- so its number never spills onto another value of the same
** template. display_exclude wins: a key on both lists stays out of every forward table and
** still gets no reverse pattern. Keys are raw dictionary keys; an [X|Y] key also matches by
** its stripped form. Loaded by name; absent file = nothing restricted. */
static const char *const kExactOnlyFile = "exact_only.json";
static std::unordered_map<std::string, std::unordered_set<std::string>> s_exact_only;
/* Merged-map keys (色碼 prefix included) whose current value came from an
** exact-only entry: the glossary skips them. Last writer decides, like the map. */
static std::unordered_set<std::string> s_exact_only_keys;

/* stat_wrappers.json: English wrapper template with one {0} -> Chinese
** ("Added Small Passive Skills grant: {0}"). A stat line that misses is
** retried with its inside ({0}) looked up as a stat and wrapped. */
static const char *const kStatWrapperFile = "stat_wrappers.json";
struct StatWrapper { std::string prefix, suffix, zh; };
static std::vector<StatWrapper> s_stat_wrappers;
/* ctx + '\x1f' + line -> answer ("" = no answer). Node-stable: pointers into it
** stay valid until translation_shutdown, like translation_lookup's results. */
static std::unordered_map<std::string, std::string> s_stat_cache;
/* Same, for translation_lookup_stat_colored (answers carry the colour). */
static std::unordered_map<std::string, std::string> s_stat_colored_cache;
/* translation_lookup_stat_row: ctx '\x1f' ids '\x1f' row -> answer ("" = none);
** node-stable until the next reload. */
static std::unordered_map<std::string, std::string> s_stat_row_cache;
/* meta.json "source_lookup". false (absent) = only the legacy "gems" source can be selected,
** exact entries only, and id lookups answer nothing (poe2, zh-rCN, ko-KR). */
static bool s_source_lookup = false;
/* The game whose rules run (game_rules.h, translation_game()). Set at the start
** of every load from POB_GAME; with POB_GAME unset, meta.json "source" decides
** once it is read -- the behaviour from before the rules were split. */
static const GameRules *s_rules = &kGameRulesPoe1;
static bool s_game_from_env = false;
static std::string s_meta_source; /* meta.json "source", "" when absent */
static const char *const kLegacySource = "gems";
static const std::unordered_map<std::string, std::string> *s_active_source = nullptr;
static std::string s_active_source_name;
/* Each source's own forward patterns (same shape as s_forward_pattern), so a sourced value
** line resolves through ITS file's template, not whichever later file redefined it. */
static std::unordered_map<std::string, std::unordered_map<std::string, FillPlan>> s_source_patterns;
static const std::unordered_map<std::string, FillPlan> *s_active_source_patterns = nullptr;

/* Results computed while a source is active may embed a source-specific translation (the
** recursing paths), so they must never land in the shared cache; each source caches into its
** own map. Entries are created once at load time (pointers stay valid). */
static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> s_source_cache;
static std::unordered_map<std::string, std::string> *s_lookup_cache_active = &s_lookup_cache;
/* Same isolation for translation_lookup_item_title: its cached answer came
** through the active source too, so each source keeps its own title cache. */
static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> s_item_title_source_cache;
static std::unordered_map<std::string, std::string> *s_item_title_cache_active = &s_item_title_cache;
/* ========== Dynamic structures for externalized data ========== */

struct HeaderMapping_Dyn {
    std::string chinese;
    std::string english;
};

struct SynonymRule {
    enum Type { REPLACE, REMOVE, PROTECT_REMOVE };
    Type type;
    std::string from;
    std::string to;       /* REPLACE: target; PROTECT_REMOVE: placeholder restored */
    std::string protect;  /* PROTECT_REMOVE: compound to protect */
};

/* ========== Reverse-translation trace ==========
**
** See translation_manager.h. The only field threaded through the lookup helpers is the
** origin: which dictionary file supplied the winning entry. */
static bool        s_trace_on = false;
static std::string s_trace_buf;
static const char *s_rev_rule = "";      /* branch that produced the last result */
static std::string s_rev_key;            /* dictionary key that matched, if any */
static std::string s_rev_file;           /* file that key's winning copy came from */

/* Set right before returning, so the wrapper can record which branch ran; a comma
** expression wraps the returned value in place. */
#define REV(tag, expr) (s_rev_rule = (tag), (expr))

static void rev_note(const std::string &key, const std::string &file)
{
    if (!s_trace_on) return;
    s_rev_key = key;
    s_rev_file = file;
}

/* Dynamic vectors (populated from JSON) */
static std::vector<HeaderMapping_Dyn> s_item_headers_vec;
static std::vector<HeaderMapping_Dyn> s_rarity_values_vec;
static std::vector<HeaderMapping_Dyn> s_item_classes_vec;
static std::vector<HeaderMapping_Dyn> s_influence_tags_vec;
static std::vector<HeaderMapping_Dyn> s_status_lines_vec;
static std::vector<std::string>       s_skip_patterns_vec;
static std::vector<std::string>       s_mod_suffixes_vec;
/* Advanced-copy "{ ... }" annotation markers, LONGEST Chinese first — the file
** ships them sorted because 固定詞綴 is a substring of 灼烙總督固定詞綴 and the
** scan consumes each match. */
static std::vector<HeaderMapping_Dyn> s_mod_annotations_vec;
/* English header name → its translatable values (e.g. Radius → 大/中/小). */
static std::unordered_map<std::string, std::vector<HeaderMapping_Dyn>> s_header_values;
/* English header name -> word substitutions inside its value, e.g. Requires:
** 等級 -> Level, 力量 -> Str; "*" applies under every header (the weapon damage
** range " 到 " -> "-"). Longest Chinese first, like the annotations. */
static std::unordered_map<std::string, std::vector<HeaderMapping_Dyn>> s_header_words;
/* The advanced copy's "unscalable value" marker words, after the em-dash. PoE1's
** client writes 無法使用的值, PoE2's 無法變動的值 (clientstrings
** DescriptionLabelFixedValueStat); item_metadata may add more. */
static std::vector<std::string> s_unscalable_markers;
/* Whitespace-blind reverse patterns. PoE2's client prints "+77(60-80)最大生命" with no
** space while the dictionaries carry "{0} 最大生命", so the exact pattern map never sees
** those lines. Keyed on the pattern with every space removed; templates differing only by
** spacing but translating differently are ambiguous: recorded, never answered. */
static std::unordered_map<std::string, FillPlan> s_reverse_pattern_compact;
static std::unordered_map<std::string, std::string> s_reverse_pattern_compact_origin;
static std::unordered_set<std::string> s_reverse_pattern_compact_ambiguous;
/* The spaced pattern each compact key was registered from. */
static std::unordered_map<std::string, std::string> s_reverse_pattern_compact_src;
/* Headers whose value is itself a name the dictionaries know ("賦予技能: 長矛投擲"
** -> "Grants Skill: Spear Throw"): every run of non-ASCII text in the value is
** looked up whole in the exact reverse map. */
static std::unordered_set<std::string> s_header_names;
/* Lines the game COMPOSES as "<prefix><an ordinary stat>"; GGPK holds only the template, so the
** composed string is in no dictionary -- translate it the way it was built.
** POB uses two English prefixes for the same Chinese one:
**     enchant  ->  "Added Small Passive Skills grant: "
**     modifier ->  "Added Small Passive Skills also grant: "
** The discriminator is the trailing " (enchant)", not the Chinese wording. */
struct CompositePrefix {
    std::string chinese;
    std::string english;          /* rolled modifier form */
    std::string english_enchant;  /* enchant form; falls back to english */
};
static std::vector<CompositePrefix> s_composite_prefixes_vec;
/* Chinese affix name -> GGG's English affix name, the key POB matches against
** its own mod database to fill an item's prefix/suffix slots. See
** rebuild_mod_annotation for why an empty slot is destructive on cluster jewels. */
static std::unordered_map<std::string, std::string> s_affix_names;

/* ========== Section grammar ==========
**
** A pasted item is a sequence of dash-separated SECTIONS; what a line means depends on its
** section (the same prose is flavour text on a unique, a modifier on a rare).
** Deliberately NOT split into implicit/explicit/enchant kinds: POB derives the stage from the
** "{ ... Modifier }" annotations we rebuild, and a distinction no rule reads drifts silently. */
enum class LineKind {
    Unknown = 0,  /* no structural evidence — behave exactly as before */
    Separator,
    Header,       /* section 0: item class, rarity, name, base type */
    Property,     /* everything up to and including the Item Level section */
    Enchant,
    Mods,
    Status,
    Flavour,
    Description,
};

static const char* kind_name(LineKind k) {
    switch (k) {
        case LineKind::Separator:   return "sep";
        case LineKind::Header:      return "header";
        case LineKind::Property:    return "property";
        case LineKind::Enchant:     return "enchant";
        case LineKind::Mods:        return "mods";
        case LineKind::Status:      return "status";
        case LineKind::Flavour:     return "flavour";
        case LineKind::Description: return "desc";
        default:                    return "-";
    }
}

/* Dynamic synonym rules */
static std::vector<SynonymRule>   s_synonym_rules;
static std::vector<std::string>   s_strip_punctuation;
static std::vector<std::string>   s_strip_space_around;
static std::vector<std::pair<std::string,std::string>> s_strip_measure_words;

/* Incomplete-translation whitelist (from meta.json) */
static std::vector<std::string>   s_whitelist_terms;

/* Keys that must never become glossary terms (from meta.json): GGG's internal placeholder
** affixes ("[UNUSED]" -> "反鍊金的") would silently rewrite unrelated UI text. */
static std::vector<std::string>   s_glossary_blacklist;

/* ========== Helper: read env via Win32 (NOT getenv) ==========
**
** The host sets POB_* with SetEnvironmentVariableW, but this DLL's UCRT snapshots the
** environment at ucrtbase init (BEFORE the host sets anything), so getenv() may never see
** them. GetEnvironmentVariableA reads the live Win32 environment. */
const char* translation_win_env(const char *name, char *buf, unsigned size) {
    DWORD n = GetEnvironmentVariableA(name, buf, size);
    return (n > 0 && n < size) ? buf : nullptr;
}

static const char* win_env(const char *name, char *buf, unsigned size) {
    return translation_win_env(name, buf, size);
}

/* ========== Launcher dictionary switches (POB_ZH_DICT_SWITCHES) ==========
**
** "a.json=0;b.json=1" -- the settings page's per-file switches (launcher_config.h
** kOptionalDicts). A file switched off is skipped although load_order names it; with no
** variable (older launcher, CLI path) nothing is skipped. Names compare case-insensitively. */
struct DictSwitches {
    std::vector<std::pair<std::string, bool>> items;   /* file, on */
    std::vector<std::string> notes;                    /* one per switched file met */

    void read() {
        char buf[1024];
        const char *v = win_env("POB_ZH_DICT_SWITCHES", buf, sizeof(buf));
        if (!v) return;
        const std::string s(v);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t end = s.find(';', pos);
            if (end == std::string::npos) end = s.size();
            const std::string item = s.substr(pos, end - pos);
            const size_t eq = item.find('=');
            if (eq != std::string::npos && eq > 0)
                items.emplace_back(item.substr(0, eq), item.substr(eq + 1) != "0");
            pos = end + 1;
        }
    }
    const std::pair<std::string, bool> *find(const std::string &filename) const {
        for (const auto &it : items)
            if (_stricmp(it.first.c_str(), filename.c_str()) == 0) return &it;
        return nullptr;
    }
    /* true = leave this load_order entry out */
    bool skip(const std::string &filename) {
        const auto *it = find(filename);
        if (!it || it->second) return false;
        notes.push_back(filename + " off: skipped");
        return true;
    }
    void loaded(const std::string &filename, int count) {
        if (find(filename)) notes.push_back(filename + " on: loaded " + std::to_string(count) + " entries");
    }
};


static inline bool is_ascii_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}


static std::string to_ascii_lower(const std::string &s) {
    std::string r = s;
    for (auto &c : r) {
        if (c >= 'A' && c <= 'Z') c += 32;
    }
    return r;
}


static bool is_glossary_candidate(const std::string &key) {
    if (key.size() < 4 || key.size() > 60) return false;
    bool has_letter = false;
    for (char c : key) {
        if (is_ascii_alpha(c)) has_letter = true;
        /* Reject entries with placeholders */
        if (c == '{') return false;
    }
    if (!key.empty() && key[0] == '^') return false;
    if (!has_letter) return false;
    for (const std::string &bad : s_glossary_blacklist) {
        if (key == bad) return false;
    }
    return true;
}


static std::string get_exe_directory() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *lastSlash = strrchr(path, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
    }
    return std::string(path);
}


static std::string normalize_whitespace(const std::string &s) {
    std::string result;
    result.reserve(s.size());
    bool last_was_space = true; /* true → skip leading spaces */
    for (size_t i = 0; i < s.size(); i++) {
        /* Ideographic space U+3000 = E3 80 80 */
        if ((unsigned char)s[i] == 0xE3 && i + 2 < s.size() &&
            (unsigned char)s[i+1] == 0x80 && (unsigned char)s[i+2] == 0x80) {
            if (!last_was_space) { result += ' '; last_was_space = true; }
            i += 2; /* skip 3-byte sequence */
            continue;
        }
        /* Regular ASCII space/tab */
        if (s[i] == ' ' || s[i] == '\t') {
            if (!last_was_space) { result += ' '; last_was_space = true; }
            continue;
        }
        result += s[i];
        last_was_space = false;
    }
    /* Trim trailing space */
    if (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

/* A pattern key with every ASCII and ideographic space removed. */
static std::string compact_key(const std::string &s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == ' ' || s[i] == '\t') continue;
        if ((unsigned char)s[i] == 0xE3 && i + 2 < s.size() &&
            (unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0x80) {
            i += 2;
            continue;
        }
        r += s[i];
    }
    return r;
}

static void register_compact_pattern(const std::string &key, const FillPlan &plan, const std::string &origin) {
    const std::string ck = compact_key(key);
    if (ck.empty() || s_reverse_pattern_compact_ambiguous.count(ck)) return;
    auto it = s_reverse_pattern_compact.find(ck);
    if (it == s_reverse_pattern_compact.end() || s_reverse_pattern_compact_src[ck] == key) {
        /* new, or the same spaced pattern again: last-wins, as the exact map does */
        s_reverse_pattern_compact[ck] = plan;
        s_reverse_pattern_compact_origin[ck] = origin;
        s_reverse_pattern_compact_src[ck] = key;
        return;
    }
    auto lower = [](std::string t) {
        for (auto &c : t) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return t;
    };
    if (lower(it->second.text) == lower(plan.text) && it->second.src == plan.src) return;
    s_reverse_pattern_compact.erase(it);
    s_reverse_pattern_compact_origin.erase(ck);
    s_reverse_pattern_compact_src.erase(ck);
    s_reverse_pattern_compact_ambiguous.insert(ck);
}

/* ========== Helper: replace digits with # for pattern matching ========== */

/* ========== Helper: check if position starts a (X-Y) range pattern ========== */

/* Returns length of the range if matched (including parens), or 0 if not a range.
 * Matches (4-6), (10-20), (1.5-3.0) and the NEGATIVE form used for penalties
 * ("-72(-80--70)%", four minus signs run together), so a digit must not be required
 * straight after '('. */
static size_t match_range(const std::string &s, size_t pos) {
    if (pos >= s.size() || s[pos] != '(') return 0;
    size_t i = pos + 1;
    /* First number, optionally signed */
    if (i < s.size() && s[i] == '-') i++;
    if (i >= s.size() || !(s[i] >= '0' && s[i] <= '9')) return 0;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == ',')) i++;
    /* Dash separator */
    if (i >= s.size() || s[i] != '-') return 0;
    i++;
    /* Second number, optionally signed */
    if (i < s.size() && s[i] == '-') i++;
    if (i >= s.size() || !(s[i] >= '0' && s[i] <= '9')) return 0;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == ',')) i++;
    /* Closing paren */
    if (i >= s.size() || s[i] != ')') return 0;
    return (i + 1) - pos;
}

/* A '+'/'-' right before a number is folded into its '#' (templates write "{0}" and let the
** value carry its sign, so signed and unsigned must hash the same). A '-' BETWEEN two numbers
** is a range separator ("5-10"): absorb one only when what precedes it is not a number. */
static bool absorb_sign(std::string &result) {
    if (result.empty()) return false;
    char back = result.back();
    if (back == '+') { result.pop_back(); return true; }
    /* "8%-12%" is a range too: the '-' after a percentage separates. */
    const bool after_percent = result.size() >= 3 && result[result.size() - 2] == '%' && result[result.size() - 3] == '#';
    if (back == '-' && !after_percent && (result.size() < 2 || result[result.size() - 2] != '#')) {
        result.pop_back();
        return true;
    }
    return false;
}

/* extract_numbers' counterpart to absorb_sign, applying the same rule to the raw
** input: is s[i-1] a sign belonging to the number that starts at i? The two must
** agree or the extracted token stops matching the '#' it was hashed into. */
static bool sign_before(const std::string &s, size_t i) {
    if (i == 0) return false;
    char c = s[i - 1];
    if (c == '+') return true;
    if (c != '-') return false;
    if (i >= 2) {
        char p = s[i - 2];
        if ((p >= '0' && p <= '9') || p == '.' || p == ',') return false; /* "5-10" */
    }
    return true;
}

/* 除了雜湊字串,還可以回報「每一個 '#' 是哪來的」。
**
** 塞在同一支函式:第 i 個 '#' 對應哪段原文完全由這裡的掃描規則決定(正負號吸收、(X-Y) 範圍、
** 7(6-11) 進階複製形式、千分位);另寫抽值器等於同一套規則寫兩份,漂開後會安靜錯位。
** slots 若不為 null,長度等於輸出的 '#' 數,依序:
**   非空 = 由數字(含吸收的正負號、範圍)摺疊來的,內容就是原文;
**   空字串 = 輸入本來就寫著 '#'(POB 工藝介面詞綴列),沒有數值可填。 */
static std::string digits_to_hash(const std::string &s, std::vector<std::string> *slots = nullptr) {
    std::string result;
    result.reserve(s.size());
    bool in_number = false;
    /* 把 result 尾端被 absorb_sign 吃掉的正負號還給正在收集的那一格 */
    auto begin_slot = [&](bool absorbed, char sign) {
        if (!slots) return;
        slots->push_back(absorbed ? std::string(1, sign) : std::string());
    };
    auto append_slot = [&](const std::string &t) {
        if (slots && !slots->empty()) slots->back() += t;
    };
    for (size_t i = 0; i < s.size(); i++) {
        /* Check for (X-Y) range pattern — collapse entire range to single # */
        size_t range_len = match_range(s, i);
        if (range_len > 0) {
            if (in_number) {
                /* Roll range attached to a value, e.g. "7(6-11)" from the
                 * game's advanced item copy — absorb into the preceding #
                 * so "+7(6-11) X" hashes the same as "+7 X". */
                append_slot(s.substr(i, range_len));
                i += range_len - 1;
                in_number = false;
                continue;
            }
            /* Absorb preceding sign, same as for plain numbers */
            char sign = result.empty() ? '\0' : result.back();
            bool absorbed = absorb_sign(result);
            result += '#';
            begin_slot(absorbed, sign);
            append_slot(s.substr(i, range_len));
            i += range_len - 1; /* -1 because loop increments */
            in_number = false;
            continue;
        }
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (!in_number) {
                /* Absorb a preceding sign into the # placeholder: POB prints "{0:+d}" ("+20") but
                 * templates use "{0}" (just "#"); likewise "-2" must match the key "+2" does. */
                char sign = result.empty() ? '\0' : result.back();
                bool absorbed = absorb_sign(result);
                result += '#';
                begin_slot(absorbed, sign);
                in_number = true;
            }
            append_slot(std::string(1, c));
            /* skip digit */
        } else if (c == '.' && in_number && i + 1 < s.size() && s[i+1] >= '0' && s[i+1] <= '9') {
            /* decimal point inside number, skip */
            append_slot(".");
        } else if (c == ',' && in_number && i + 1 < s.size() && s[i+1] >= '0' && s[i+1] <= '9') {
            /* thousands separator inside number (e.g., "7,881"), skip */
            append_slot(",");
        } else {
            in_number = false;
            if (c == '#') {
                /* 原文自己寫著 '#'(POB 工藝下拉 "+# to all Attributes")也是一格,且要像數字一樣
                ** 吃掉前面的正負號,否則鍵跟 "+5 ..." 對不上。吃掉的正負號留在這格的值裡("+#"),
                ** 填回譯文時照原樣放回;沒有正負號的仍是空字串。 */
                char sign = result.empty() ? '\0' : result.back();
                bool absorbed = absorb_sign(result);
                result += '#';
                if (slots) slots->push_back(absorbed ? std::string(1, sign) + "#" : std::string());
                continue;
            }
            result += c;
        }
    }
    return result;
}

/* ========== Helper: extract numbers from a string in order ========== */

static std::vector<std::string> extract_numbers(const std::string &s) {
    std::vector<std::string> nums;
    std::string current;
    bool in_number = false;
    for (size_t i = 0; i < s.size(); i++) {
        /* Check for (X-Y) range — extract as a single token */
        size_t range_len = match_range(s, i);
        if (range_len > 0) {
            if (in_number) {
                /* Roll range attached to a value, e.g. "7(6-11)" — keep the
                 * range glued to its value as one token so fill_numbers()
                 * reproduces the advanced-copy form POB parses natively. */
                current += s.substr(i, range_len);
                nums.push_back(current);
                current.clear();
                in_number = false;
                i += range_len - 1;
                continue;
            }
            /* Include preceding sign (mirrors absorb_sign in digits_to_hash) */
            std::string range_text;
            if (i > 0 && sign_before(s, i)) {
                range_text += s[i-1];
            }
            range_text += s.substr(i, range_len);
            nums.push_back(range_text);
            i += range_len - 1; /* -1 because loop increments */
            continue;
        }
        char c = s[i];
        if (c >= '0' && c <= '9') {
            /* Include the preceding sign in the extracted number so it survives
             * the round-trip through pattern matching (digits_to_hash absorbs it) */
            if (!in_number && i > 0 && sign_before(s, i)) {
                current += s[i-1];
            }
            current += c;
            in_number = true;
        } else if (c == '.' && in_number && i + 1 < s.size() && s[i+1] >= '0' && s[i+1] <= '9') {
            current += c;
        } else if (c == ',' && in_number && i + 1 < s.size() && s[i+1] >= '0' && s[i+1] <= '9') {
            current += c; /* keep comma in extracted number for display formatting */
        } else {
            if (in_number) {
                nums.push_back(current);
                current.clear();
                in_number = false;
            }
        }
    }
    if (!current.empty()) nums.push_back(current);
    return nums;
}

/* ========== Helper: normalize {0},{1},... placeholders to # ==========
**
** ph_index 若不為 null,長度等於輸出的 '#' 數,記下每格來歷:>= 0 是 {N} 的 N;-1 是原文本
** 寫著的 '#'。編號只在這裡看得見(變成 '#' 後就消失),而中文常把 {1} 排在 {0} 前面。 */

static std::string normalize_placeholders(const std::string &s, std::vector<int> *ph_index = nullptr) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '{' && i + 2 < s.size()) {
            /* Check for {N} or {NN} pattern */
            size_t j = i + 1;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
            if (j > i + 1 && j < s.size()) {
                if (s[j] == '}') {
                    result += '#';
                    if (ph_index) ph_index->push_back(atoi(s.c_str() + i + 1));
                    i = j; /* skip past } */
                    continue;
                }
                /* GGG's format specs ({0:+d}, {0:.2f}) are the same placeholder as {0};
                ** left literal they hash to garbage ("{#:+d}"). Bounded so a stray '{'
                ** cannot scan on. */
                if (s[j] == ':') {
                    size_t k = s.find('}', j);
                    if (k != std::string::npos && k - j <= 8) {
                        result += '#';
                        if (ph_index) ph_index->push_back(atoi(s.c_str() + i + 1));
                        i = k; /* skip past } */
                        continue;
                    }
                }
            }
        }
        /* 不是佔位符的 '{' 照抄(GGG 的 "<grey>{(...)}" 之類),但原文自帶的 '#'
        ** 仍是一格,否則後面的編號會整排錯開。 */
        if (s[i] == '#' && ph_index) ph_index->push_back(-1);
        result += s[i];
    }
    return result;
}

/* ========== 數值填回:依編號,不依出現順序 ==========
**
** 一個樣板槽位的來歷。ph >= 0 表示它是 {N};否則 lit 是樣板裡寫死的數字文字
** (空字串代表樣板裡本來就是 '#',沒有值可對)。 */
struct TmplSlot {
    int         ph;
    std::string lit;
};

static std::vector<TmplSlot> template_slots(const std::string &tmpl) {
    std::vector<int> ph_index;
    std::string p = normalize_placeholders(tmpl, &ph_index);
    std::vector<std::string> hs;
    digits_to_hash(p, &hs);

    std::vector<TmplSlot> out;
    out.reserve(hs.size());
    size_t mi = 0;
    for (const std::string &t : hs) {
        if (!t.empty()) {                 /* 寫死在樣板裡的數字 */
            out.push_back({ -1, t });
            continue;
        }
        /* digits_to_hash 原樣抄過去的 '#',依序就是 normalize_placeholders 產生的
        ** 那幾格 —— 兩支掃描器都不會重排,所以按順序取即可對上。 */
        int n = (mi < ph_index.size()) ? ph_index[mi] : -1;
        mi++;
        out.push_back({ n, std::string() });
    }
    return out;
}

/* 兩個字面數字是不是同一個值。忽略正負號:digits_to_hash 會把緊貼數字的正負號吸進 '#',
** 英文寫 "+1" 而中文寫 "1" 的條目非常多。 */
static bool same_literal_value(const std::string &a, const std::string &b) {
    size_t ai = (!a.empty() && (a[0] == '+' || a[0] == '-')) ? 1 : 0;
    size_t bi = (!b.empty() && (b[0] == '+' || b[0] == '-')) ? 1 : 0;
    return a.compare(ai, std::string::npos, b, bi, std::string::npos) == 0;
}

/* 測試用的突變開關:打開就退回「照 '#' 出現順序填」的舊行為,讓「弄回舊行為測試就該紅」
** 可隨時重跑。只有 --placeholder-selftest 會碰它。 */
static bool s_legacy_fill = false;

int translation_debug_set_legacy_fill(int on) {
    bool prev = s_legacy_fill;
    s_legacy_fill = (on != 0);
    return prev ? 1 : 0;
}

/* dst_text 是已雜湊+正規化的目標樣板。對照一律用語言無關的鍵,配不上才退回位置:
**   1. {N} 對 {N} —— 唯一能表達「中文把 {1} 講在前面」的資訊
**   2. 字面數字對同值的字面數字(寫死的變體)
**   3. 剩下的照剩下的順序配(兩邊都無鍵時維持舊行為)
**   4. 配不到的字面數字照抄樣板;配不到的 {N} 留 '#'(寧可空著,也不要填錯數字)
** keep_dst_literals("keep_literals" 逐鍵裁定):第 3 步不把來源剩下的數字配給目標寫死的
** 數字,中文字面數字照 GGPK 顯示。只給裁定過的鍵用,套全部鍵會改壞英文才對的數字。 */
static FillPlan build_fill_plan_slots(const std::vector<TmplSlot> &src, const std::vector<TmplSlot> &dst,
                                      const std::string &dst_text, bool keep_dst_literals = false);

static FillPlan build_fill_plan(const std::string &src_tmpl, const std::string &dst_tmpl,
                                const std::string &dst_text) {
    return build_fill_plan_slots(template_slots(src_tmpl), template_slots(dst_tmpl), dst_text);
}

/* template_slots 是載入時建計畫最貴的一步(兩次正規化掃描),抽出來讓呼叫端對同一樣板只算一次。 */
static FillPlan build_fill_plan_slots(const std::vector<TmplSlot> &src, const std::vector<TmplSlot> &dst,
                                      const std::string &dst_text, bool keep_dst_literals) {
    FillPlan plan;
    size_t nhash = 0;
    for (char c : dst_text) if (c == '#') nhash++;

    /* 正規化後的 '#' 數與樣板槽位數對不上,代表我們對這一條的理解有洞。這時整份
    ** 對照表都不可信,退回舊的依序填 —— 錯也錯得和以前一樣,不會多生一種錯。 */
    if (s_legacy_fill || dst.size() != nhash) {
        plan.text = dst_text;
        plan.src.reserve(nhash);
        for (size_t i = 0; i < nhash; i++) plan.src.push_back((int)i);
        return plan;
    }

    std::vector<int>  from(dst.size(), -1);
    std::vector<char> taken(src.size(), 0);

    for (size_t d = 0; d < dst.size(); d++) {
        if (dst[d].ph < 0) continue;
        for (size_t s = 0; s < src.size(); s++) {
            /* 同一個 {0} 在目標出現兩次是合法的,所以不看 taken(它只給第 3 步標記「未認領」)。
            ** 已知代價:英文 {0}/{1}、中文兩格都寫 {0} 的條目會填兩次同值,那是資料缺陷。 */
            if (src[s].ph == dst[d].ph) { from[d] = (int)s; taken[s] = 1; break; }
        }
    }
    for (size_t d = 0; d < dst.size(); d++) {
        if (from[d] >= 0 || dst[d].ph >= 0 || dst[d].lit.empty()) continue;
        for (size_t s = 0; s < src.size(); s++) {
            if (taken[s] || src[s].ph >= 0 || src[s].lit.empty()) continue;
            if (same_literal_value(src[s].lit, dst[d].lit)) { from[d] = (int)s; taken[s] = 1; break; }
        }
    }
    {
        size_t s = 0;
        for (size_t d = 0; d < dst.size(); d++) {
            if (from[d] >= 0) continue;
            /* 裁定鍵:目標寫死的數字留給第 4 步照抄,不吃來源的數值 */
            if (keep_dst_literals && dst[d].ph < 0 && !dst[d].lit.empty()) continue;
            while (s < src.size() && taken[s]) s++;
            if (s >= src.size()) break;
            from[d] = (int)s;
            taken[s] = 1;
        }
    }

    plan.text.reserve(dst_text.size());
    plan.src.reserve(nhash);
    size_t k = 0;
    for (char c : dst_text) {
        if (c != '#') { plan.text += c; continue; }
        if (from[k] < 0 && dst[k].ph < 0 && !dst[k].lit.empty()) {
            /* 樣板自己寫死的數字,來源沒有對應的一格:照抄回去。舊版把它當成待填格
            ** 吃掉一個數值,於是後面每一格都往前挪一位。 */
            plan.text += dst[k].lit;
        } else {
            plan.text += '#';
            plan.src.push_back(from[k]);
        }
        k++;
    }
    return plan;
}

/* values 依序是輸入每一個 '#' 的原文(空字串 = 那一格輸入本來就寫著 '#',
** 沒有數值)。取不到值就留 '#',與舊版「數值用完了就不填」一致。 */
static std::string fill_plan(const FillPlan &plan, const std::vector<std::string> &values) {
    std::string result;
    result.reserve(plan.text.size());
    size_t k = 0;
    for (char c : plan.text) {
        if (c != '#') { result += c; continue; }
        int si = (k < plan.src.size()) ? plan.src[k] : -1;
        k++;
        if (si >= 0 && (size_t)si < values.size() && !values[si].empty()) result += values[si];
        else result += '#';
    }
    return result;
}

/* 輸入端的對照組:把一行實際文字拆成「每個 '#' 對應的原文」。與 extract_numbers 差在原文自己
** 寫著 '#' 的那一格也算一格(值是空的)。
** normalize_ph 必須跟該路徑算查表鍵時有沒有做 normalize_placeholders 一致,否則整排錯位:
** 正向 pattern 有做(translation_lookup 步驟 3),反向沒有(reverse_one_line_impl 步驟 5);
** 反向多切一刀,"<colour:...>{{{0}}}" 的 9001 會被當佔位符吃掉。
** 突變開關也要管到這裡:舊行為是 fill_numbers(樣板, extract_numbers(輸入)),兩半都得退回去。 */
static std::vector<std::string> hash_slot_values(const std::string &s, bool normalize_ph) {
    if (s_legacy_fill) return extract_numbers(s);
    std::vector<std::string> vals;
    digits_to_hash(normalize_ph ? normalize_placeholders(s) : s, &vals);
    return vals;
}


static void replace_all(std::string &str, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.size(), to);
        pos += to.size();
    }
}


static std::string normalize_chinese_synonyms(const std::string &s) {
    std::string result = s;


    /* Phase 1: Apply replacements and protect compounds */
    std::string protect_placeholder = "\x01\x02\x03\x04";

    for (auto &rule : s_synonym_rules) {
        switch (rule.type) {
        case SynonymRule::REPLACE:
            replace_all(result, rule.from, rule.to);
            break;
        case SynonymRule::REMOVE:
            replace_all(result, rule.from, "");
            break;
        case SynonymRule::PROTECT_REMOVE:
            /* Protect the compound, remove the component, then restore */
            if (result.find(rule.protect) != std::string::npos) {
                std::string ph = protect_placeholder;
                replace_all(result, rule.protect, ph);
                replace_all(result, rule.from, "");
                replace_all(result, ph, rule.protect);
            } else {
                replace_all(result, rule.from, "");
            }
            break;
        }
    }

    /* Phase 2: Strip punctuation */
    for (auto &p : s_strip_punctuation) {
        replace_all(result, p, "");
    }

    /* Phase 3: Strip measure words */
    for (auto &mw : s_strip_measure_words) {
        replace_all(result, mw.first, mw.second);
    }

    /* Phase 4: Normalize spaces around special chars */
    if (!s_strip_space_around.empty()) {
        std::string normalized;
        normalized.reserve(result.size());
        for (size_t i = 0; i < result.size(); i++) {
            if (result[i] == ' ') {
                bool near_special = false;
                for (auto &sc : s_strip_space_around) {
                    if (sc.size() == 1) {
                        if (i > 0 && result[i-1] == sc[0]) near_special = true;
                        if (i + 1 < result.size() && result[i+1] == sc[0]) near_special = true;
                    }
                }
                if (near_special) continue;
            }
            normalized += result[i];
        }
        result = normalized;
    }

    return result;
}


static std::string strip_brackets(const std::string &s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '[') {
            /* Find the pipe and closing bracket */
            size_t pipe = s.find('|', i + 1);
            size_t close = s.find(']', i + 1);
            if (pipe != std::string::npos && close != std::string::npos && pipe < close) {
                /* [X|Y] → keep Y */
                result += s.substr(pipe + 1, close - pipe - 1);
                i = close; /* skip past ] */
                continue;
            } else if (close != std::string::npos && (pipe == std::string::npos || pipe > close)) {
                /* [X] without pipe → keep X */
                result += s.substr(i + 1, close - i - 1);
                i = close;
                continue;
            }
        }
        result += s[i];
    }
    return result;
}

/* ========== 測試掛勾:直接驅動填值機制 ==========
**
** 出貨字典不見得有自檢要涵蓋的邊角(編號不從 0 開始、同一個 {0} 用兩次、寫死數字);這支掛勾
** 讓測試自給樣板與輸入,走的仍是共用的 build_fill_plan / fill_plan。
** 輸入必須雜湊成與 source 樣板相同的字串;對不起來回 nullptr,免得打錯的案例安靜「通過」。 */
const char* translation_debug_fill(const char *source_tmpl, const char *target_tmpl, const char *input) {
    static std::string out;
    out.clear();
    if (!source_tmpl || !target_tmpl || !input) return nullptr;

    std::string src = strip_brackets(source_tmpl);
    std::string dst = strip_brackets(target_tmpl);
    std::string src_hash = normalize_whitespace(digits_to_hash(normalize_placeholders(src)));
    std::string dst_hash = normalize_whitespace(digits_to_hash(normalize_placeholders(dst)));

    std::string in_plain = strip_brackets(input);
    std::string in_hash = normalize_whitespace(digits_to_hash(normalize_placeholders(in_plain)));
    if (to_ascii_lower(in_hash) != to_ascii_lower(src_hash)) return nullptr;

    /* 抽值走執行期同一支,突變開關才管得到這一半。 */
    std::vector<std::string> vals = hash_slot_values(in_plain, /*normalize_ph=*/true);

    out = fill_plan(build_fill_plan(src, dst, dst_hash), vals);
    return out.c_str();
}

/* 同一行文字,兩支掃描器的答案必須一致:digits_to_hash 回報的數字格(去掉原文本來就寫著
** '#' 的空格)應逐一等於 extract_numbers。回傳不一致的欄位數,0 代表相符。 */
int translation_debug_slot_mismatch(const char *text) {
    if (!text) return 0;
    std::vector<std::string> vals;
    digits_to_hash(normalize_placeholders(text), &vals);
    std::vector<std::string> nums = extract_numbers(normalize_placeholders(text));
    size_t j = 0, bad = 0;
    for (const std::string &v : vals) {
        /* 原文自帶的 '#' 不是數字:空字串,或吃掉了正負號的 "+#" / "-#" */
        if (v.empty() || v.back() == '#') continue;
        if (j >= nums.size() || nums[j] != v) bad++;
        j++;
    }
    if (j != nums.size()) bad += (nums.size() - j);
    return (int)bad;
}


static std::string determine_locale() {
    char envbuf[128];
    const char *env = win_env("POB_LOCALE", envbuf, sizeof(envbuf));
    if (env && strlen(env) > 0) {
        return std::string(env);
    }

    /* 2. Parse Data/Translate.json for first entry's "value" field */
    std::string jsonPath = get_exe_directory() + "Data\\Translate.json";
    std::ifstream file(jsonPath);
    if (file.is_open()) {
        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        /* Simple JSON parsing: find first "value": "..." */
        size_t pos = content.find("\"value\"");
        if (pos != std::string::npos) {
            pos = content.find('"', pos + 7);
            if (pos != std::string::npos) {
                pos++; /* skip opening quote */
                size_t end = content.find('"', pos);
                if (end != std::string::npos) {
                    return content.substr(pos, end - pos);
                }
            }
        }
    }

    return ""; /* No locale found */
}

/* ========== JSON Loading Functions ========== */

/* passives_by_id.json / affixes_by_id.json: { "entries": { "<id>": "<zh>" } }.
** Deliberately NOT load_json_translations -- that one derives patterns,
** reverse keys and glossary terms from English keys, and these keys are ids. */
static int load_json_id_dict(const std::string &filepath,
                             std::unordered_map<std::string, std::string> &sink,
                             const std::string &source) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return 0;
    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    njson doc;
    try {
        doc = njson::parse(raw);
    } catch (const njson::parse_error &e) {
        PobLog::Error("i18n", "dictionary file did not parse, its entries are all "
                              "missing: " + filepath + " - " + e.what());
        return 0;
    }
    if (!doc.contains("entries") || !doc["entries"].is_object()) return 0;
    int count = 0;
    for (auto it = doc["entries"].begin(); it != doc["entries"].end(); ++it) {
        if (!it.value().is_string()) continue;
        std::string zh = it.value().get<std::string>();
        if (it.key().empty() || zh.empty()) continue;
        sink[it.key()] = std::move(zh);   /* last-wins within the file */
        ++count;
    }
    /* Optional "templates": id -> the GGPK English of that id. Its presence
    ** marks the file as English-checked (see translation_lookup_by_id_en); an
    ** older pack without it keeps the plain id behaviour. */
    if (doc.contains("templates") && doc["templates"].is_object()) {
        auto &tsink = s_id_templates[source];   /* exists (maybe empty) = checked */
        for (auto it = doc["templates"].begin(); it != doc["templates"].end(); ++it) {
            if (!it.value().is_string() || it.key().empty()) continue;
            tsink[it.key()] = normalize_whitespace(it.value().get<std::string>());
        }
    }
    return count;
}

/* The distinct {N} numbers a template line uses. Splitting a multi-line entry per line is only
** safe when line i of the English and of the Chinese carry the SAME set (else one half drops the
** number and the other shows a raw {0}). Shared by the per-line registration and the stat index. */
static std::vector<int> placeholder_index_set(const std::string &s) {
    std::vector<int> ix;
    normalize_placeholders(s, &ix);
    std::vector<int> v;
    for (int n : ix) if (n >= 0) v.push_back(n);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

/* ---- stats_by_id.json ---- */

/* GGG writes a multi-line description as one string with a literal "\n"
** (backslash, n); the line POB hands us has a real newline there. */
static std::string stat_unescape_newlines(const std::string &s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { r += '\n'; i++; continue; }
        if (s[i] == '\r') continue;
        r += s[i];
    }
    return r;
}

/* "{}" / "{:+d}" are GGG's auto-numbered placeholders (first is {0}, next {1}, ...);
** normalize_placeholders only knows the numbered form, so number them here. */
static std::string stat_number_bare_placeholders(const std::string &s) {
    std::string r;
    r.reserve(s.size() + 4);
    int next = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '{' && i + 1 < s.size() && (s[i + 1] == '}' || s[i + 1] == ':')) {
            r += '{';
            r += std::to_string(next++);
            continue;
        }
        r += s[i];
    }
    return r;
}

/* Whitespace touching a line break is layout, not content. */
static std::string stat_trim_around_newlines(const std::string &s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == ' ' && ((i + 1 < s.size() && s[i + 1] == '\n') || (!r.empty() && r.back() == '\n')))
            continue;
        r += s[i];
    }
    return r;
}

/* The index key: same hashing every forward pattern uses (placeholders and
** numbers -> '#', signs and ranges folded in), case-folded, spaces collapsed.
** `s` must already be markup-stripped and newline-unescaped. */
static std::string stat_case_key(const std::string &s) {
    return stat_trim_around_newlines(normalize_whitespace(digits_to_hash(normalize_placeholders(s))));
}
static std::string stat_index_key(const std::string &s) {
    return to_ascii_lower(stat_case_key(s));
}

/* ".txt" added when missing: stat_desc_parents and gem:<scope> may name a
** file either way. */
static std::string stat_file_basename(std::string s) {
    if (s.size() < 4 || s.compare(s.size() - 4, 4, ".txt") != 0) s += ".txt";
    return s;
}

static int load_json_stat_dict(const std::string &filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return 0;
    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    njson doc;
    try {
        doc = njson::parse(raw);
    } catch (const njson::parse_error &e) {
        PobLog::Error("i18n", "dictionary file did not parse, its entries are all "
                              "missing: " + filepath + " - " + e.what());
        return 0;
    }
    if (!doc.contains("entries") || !doc["entries"].is_object() ||
        !doc.contains("templates") || !doc["templates"].is_object()) return 0;
    const njson &templates = doc["templates"];
    /* Optional (newer data packs): the description files' parent chain and
    ** fixed Chinese slots. Absent = the gem default chain / nothing fixed. */
    if (doc.contains("stat_desc_parents") && doc["stat_desc_parents"].is_object()) {
        for (auto pit = doc["stat_desc_parents"].begin(); pit != doc["stat_desc_parents"].end(); ++pit)
            if (pit->is_string())
                s_stat_parents[stat_file_basename(pit.key())] = stat_file_basename(pit->get<std::string>());
    }
    const njson *fixed_slots = nullptr;
    if (doc.contains("fixed_slots") && doc["fixed_slots"].is_object()) fixed_slots = &doc["fixed_slots"];
    /* Optional "keep_literals": {key: true} -- the Chinese template's written-out numbers are
    ** shown as GGPK wrote them (build_fill_plan_slots keep_dst_literals). */
    const njson *keep_literals = nullptr;
    if (doc.contains("keep_literals") && doc["keep_literals"].is_object()) keep_literals = &doc["keep_literals"];
    int count = 0;
    for (auto it = doc["entries"].begin(); it != doc["entries"].end(); ++it) {
        if (!it.value().is_string()) continue;
        const std::string &key = it.key();
        /* "stat_ids|cond...|src_file|dup_ix": cond may itself hold '|', so the
        ** file is the second field from the END. */
        const size_t p_dup = key.rfind('|');
        if (p_dup == std::string::npos || p_dup == 0) continue;
        const size_t p_file = key.rfind('|', p_dup - 1);
        if (p_file == std::string::npos) continue;
        const std::string src_file = key.substr(p_file + 1, p_dup - p_file - 1);
        const int file = stat_file_id(src_file);
        std::vector<std::string> ids;
        {
            const size_t p_ids = key.find('|');
            size_t b0 = 0;
            while (b0 < p_ids) {
                size_t sp = key.find(' ', b0);
                if (sp == std::string::npos || sp > p_ids) sp = p_ids;
                if (sp > b0) ids.push_back(key.substr(b0, sp - b0));
                b0 = sp + 1;
            }
        }
        auto tit = templates.find(key);
        if (tit == templates.end() || !tit->is_string()) continue;

        const std::string en = stat_number_bare_placeholders(
            stat_unescape_newlines(strip_brackets(tit->get<std::string>())));
        std::string zh = stat_number_bare_placeholders(
            stat_unescape_newlines(strip_brackets(it.value().get<std::string>())));
        if (en.empty() || zh.empty()) continue;

        /* "fixed_slots": {key: {"k": N}}: the Chinese {k} has no English counterpart (the value
        ** is fixed by the description's condition). Write N into the Chinese first. */
        if (fixed_slots) {
            auto fit = fixed_slots->find(key);
            if (fit != fixed_slots->end() && fit->is_object()) {
                for (auto kv = fit->begin(); kv != fit->end(); ++kv) {
                    std::string val;
                    if (kv->is_number_integer()) val = std::to_string(kv->get<long long>());
                    else if (kv->is_number()) {
                        char b[64];
                        snprintf(b, sizeof(b), "%g", kv->get<double>());
                        val = b;
                    } else if (kv->is_string()) val = kv->get<std::string>();
                    else continue;
                    const std::string open = "{" + kv.key();
                    for (size_t p = zh.find(open); p != std::string::npos; p = zh.find(open, p)) {
                        const size_t after = p + open.size();
                        size_t close = std::string::npos;
                        if (after < zh.size() && zh[after] == '}') close = after;
                        else if (after < zh.size() && zh[after] == ':') close = zh.find('}', after);
                        if (close == std::string::npos) { p = after; continue; }
                        zh.replace(p, close - p + 1, val);
                        p += val.size();
                    }
                }
            }
        }

        /* Name-slot entries go to s_stat_named only (see StatNamedCand). */
        if (doc.contains("name_slots") && doc["name_slots"].is_object()) {
            auto nit = doc["name_slots"].find(key);
            if (nit != doc["name_slots"].end() && nit->is_object()) {
                std::unordered_map<int, std::string> name_src;
                for (auto sit = nit->begin(); sit != nit->end(); ++sit)
                    if (sit->is_string()) name_src[atoi(sit.key().c_str())] = sit->get<std::string>();
                auto add_named = [&](const std::string &en_t, const std::string &zh_t) {
                    StatNamedCand nc;
                    nc.file = file;
                    nc.ids = ids;
                    nc.zh = zh_t;
                    nc.name_src = name_src;
                    /* tokenise the English: literals and {N} / {N:fmt} */
                    std::string lit;
                    for (size_t i = 0; i < en_t.size(); i++) {
                        if (en_t[i] == '{') {
                            size_t j = i + 1;
                            while (j < en_t.size() && en_t[j] >= '0' && en_t[j] <= '9') j++;
                            size_t close = en_t.find('}', j);
                            if (j > i + 1 && close != std::string::npos && (en_t[j] == '}' || en_t[j] == ':')) {
                                if (!lit.empty()) { nc.toks.push_back({ lit, -1 }); lit.clear(); }
                                nc.toks.push_back({ std::string(), atoi(en_t.c_str() + i + 1) });
                                i = close;
                                continue;
                            }
                        }
                        lit += en_t[i];
                    }
                    if (!lit.empty()) nc.toks.push_back({ lit, -1 });
                    s_stat_named.push_back(std::move(nc));
                };
                add_named(en, zh);
                /* A multi-line name template whose LAST line is a fixed tag on both sides is drawn
                ** by POB as the lines before it joined into one: register that form too (English
                ** lines 1..n-1 joined by a space -> Chinese lines 1..m-1), only when both last
                ** lines carry no placeholder and the rest keeps the same {N} set. */
                if (en.find('\n') != std::string::npos && zh.find('\n') != std::string::npos) {
                    const size_t el = en.rfind('\n'), zl = zh.rfind('\n');
                    const std::string en_last = en.substr(el + 1), zh_last = zh.substr(zl + 1);
                    std::string en_head = en.substr(0, el);
                    const std::string zh_head = zh.substr(0, zl);
                    if (en_last.find('{') == std::string::npos && zh_last.find('{') == std::string::npos &&
                        placeholder_index_set(en_head) == placeholder_index_set(zh_head)) {
                        for (char &ch : en_head) if (ch == '\n') ch = ' ';
                        add_named(en_head, zh_head);
                    }
                }
                ++count;
                continue;
            }
        }

        bool keep = false;
        if (keep_literals) {
            auto kit = keep_literals->find(key);
            keep = kit != keep_literals->end() && kit->is_boolean() && kit->get<bool>();
        }

        int nstats = 0;
        {
            const size_t p_ids = key.find('|');
            bool in_tok = false;
            for (size_t i = 0; i < p_ids; i++) {
                if (key[i] == ' ') in_tok = false;
                else if (!in_tok) { in_tok = true; nstats++; }
            }
        }

        auto add_cand = [&](const std::string &en_t, const std::string &zh_t) {
            StatCand c;
            c.file = file;
            c.ckey = stat_case_key(en_t);
            c.ids = ids;
            c.nstats = nstats;
            /* {N} numbers written with a sign format ("{0:+d}") */
            std::vector<int> plus_ph;
            for (size_t p = en_t.find('{'); p != std::string::npos; p = en_t.find('{', p + 1)) {
                size_t q = p + 1;
                while (q < en_t.size() && en_t[q] >= '0' && en_t[q] <= '9') q++;
                if (q > p + 1 && q + 1 < en_t.size() && en_t[q] == ':' && en_t[q + 1] == '+')
                    plus_ph.push_back(atoi(en_t.c_str() + p + 1));
            }
            /* The plan keeps the Chinese template's own spacing (the game shows GGG's double
            ** spaces); only the index KEY is whitespace-normalised. */
            const std::string zh_text = digits_to_hash(normalize_placeholders(zh_t));
            const std::vector<TmplSlot> en_slots = template_slots(en_t);
            for (size_t i = 0; i < en_slots.size(); i++) {
                const TmplSlot &t = en_slots[i];
                /* "+#" is a signed placeholder ("+{0}"), not a written-out number */
                if (t.ph < 0 && !t.lit.empty() && t.lit.back() != '#')
                    c.lits.emplace_back((int)i, t.lit);
            }
            c.plus_slot.assign(en_slots.size(), 0);
            for (size_t i = 0; i < en_slots.size(); i++)
                if (en_slots[i].ph >= 0 &&
                    std::find(plus_ph.begin(), plus_ph.end(), en_slots[i].ph) != plus_ph.end())
                    c.plus_slot[i] = 1;
            /* all three candidate forms below (whole, space-joined, per line)
            ** carry the key's ruling */
            c.plan = build_fill_plan_slots(en_slots, template_slots(zh_t), zh_text, keep);
            s_stat_index[stat_index_key(en_t)].push_back(std::move(c));
        };
        add_cand(en, zh);
        ++count;
        /* POB may draw a two-line description as ONE line joined by a space: index that form
        ** too; the answer keeps the Chinese line break. */
        if (en.find('\n') != std::string::npos) {
            std::string joined = en;
            for (char &ch : joined) if (ch == '\n') ch = ' ';
            add_cand(joined, zh);
        }

        /* A keystone's description is one multi-line template, but POB draws each line on its
        ** own. With equal line counts, English line i is Chinese line i, so each line is a
        ** candidate too (every indexed file does this) -- only when both lines use the same {N}
        ** set. If Chinese moved the value to the other line, the English line would translate to
        ** one that silently DROPS the value and nothing catches that, so such pairs are never indexed. */
        if (en.find('\n') != std::string::npos) {
            auto split = [](const std::string &s) {
                std::vector<std::string> v;
                size_t b0 = 0;
                for (;;) {
                    size_t e = s.find('\n', b0);
                    v.push_back(s.substr(b0, e == std::string::npos ? std::string::npos : e - b0));
                    if (e == std::string::npos) break;
                    b0 = e + 1;
                }
                return v;
            };
            const std::vector<std::string> en_lines = split(en), zh_lines = split(zh);
            if (en_lines.size() == zh_lines.size()) {
                for (size_t i = 0; i < en_lines.size(); i++) {
                    if (normalize_whitespace(en_lines[i]).empty() ||
                        normalize_whitespace(zh_lines[i]).empty()) continue;
                    if (placeholder_index_set(en_lines[i]) != placeholder_index_set(zh_lines[i])) continue;
                    add_cand(en_lines[i], zh_lines[i]);
                }
            }
        }
    }
    return count;
}

/* stat_wrappers.json: { "entries": { "<English with one {0}>": "<Chinese>" } }.
** Never merged; feeds s_stat_wrappers only. */
static int load_json_stat_wrappers(const std::string &filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return 0;
    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    njson doc;
    try {
        doc = njson::parse(raw);
    } catch (const njson::parse_error &e) {
        PobLog::Error("i18n", "dictionary file did not parse, its entries are all "
                              "missing: " + filepath + " - " + e.what());
        return 0;
    }
    if (!doc.contains("entries") || !doc["entries"].is_object()) return 0;
    int count = 0;
    for (auto it = doc["entries"].begin(); it != doc["entries"].end(); ++it) {
        if (!it.value().is_string()) continue;
        const std::string en = stat_unescape_newlines(it.key());
        const std::string zh = stat_unescape_newlines(it.value().get<std::string>());
        const size_t p = en.find("{0}");
        if (p == std::string::npos || en.find("{0}", p + 3) != std::string::npos) continue;
        if (zh.find("{0}") == std::string::npos) continue;
        s_stat_wrappers.push_back({ en.substr(0, p), en.substr(p + 3), zh });
        ++count;
    }
    return count;
}

/* ---- ggpk_groups.json ---- */

/* The key a POB line / a catalog template line is compared by: the stat
** index's own (markup stripped, numbers and {N} -> '#', case folded). */
static std::string group_line_key(const std::string &s, bool is_template) {
    std::string in = strip_brackets(s);
    in.erase(std::remove(in.begin(), in.end(), '\r'), in.end());
    if (is_template) in = stat_number_bare_placeholders(in);
    return stat_index_key(in);
}

static std::vector<std::string> group_split_lines(const std::string &s) {
    std::vector<std::string> v;
    size_t b0 = 0;
    for (;;) {
        size_t e = s.find('\n', b0);
        std::string part = s.substr(b0, e == std::string::npos ? std::string::npos : e - b0);
        part.erase(std::remove(part.begin(), part.end(), '\r'), part.end());
        v.push_back(std::move(part));
        if (e == std::string::npos) break;
        b0 = e + 1;
    }
    return v;
}

static int load_json_group_catalog(const std::string &filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return 0;
    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    njson doc;
    try {
        doc = njson::parse(raw);
    } catch (const njson::parse_error &e) {
        PobLog::Error("i18n", "dictionary file did not parse, its entries are all "
                              "missing: " + filepath + " - " + e.what());
        return 0;
    }
    int count = 0;
    auto en_lines = [](const njson &v, std::vector<std::string> &out) {
        out.clear();
        if (!v.contains("en") || !v["en"].is_array()) return false;
        for (const auto &x : v["en"]) {
            if (!x.is_string()) return false;
            out.push_back(x.get<std::string>());
        }
        return out.size() >= 2;
    };
    std::vector<std::string> en;
    if (doc.contains("stats") && doc["stats"].is_object()) {
        for (auto it = doc["stats"].begin(); it != doc["stats"].end(); ++it) {
            if (!it->is_object() || !en_lines(*it, en)) continue;
            GroupStat g;
            for (const std::string &l : en) g.keys.push_back(group_line_key(l, /*is_template=*/true));
            if (g.keys[0].empty()) continue;
            auto &v = s_group_stat_heads[g.keys[0]];
            bool dup = false;
            for (const GroupStat &o : v) if (o.keys == g.keys) { dup = true; break; }
            if (!dup) { v.push_back(std::move(g)); ++count; }
        }
    }
    if (doc.contains("texts") && doc["texts"].is_object()) {
        /* Every text by its English first (whatever the line count): the same English with two
        ** different GGPK Chinese texts is ambiguous and never grouped. */
        std::unordered_map<std::string, std::vector<GroupText>> by_en;
        std::vector<std::string> order;
        for (auto it = doc["texts"].begin(); it != doc["texts"].end(); ++it) {
            if (!it->is_object() || !en_lines(*it, en)) continue;
            if (!it->contains("zh") || !(*it)["zh"].is_string()) continue;
            GroupText g;
            g.zh = (*it)["zh"].get<std::string>();
            g.zh.erase(std::remove(g.zh.begin(), g.zh.end(), '\r'), g.zh.end());
            if (normalize_whitespace(g.zh).empty()) continue;
            std::string joined;
            for (const std::string &l : en) {
                g.lines.push_back(normalize_whitespace(l));
                joined += g.lines.back();
                joined += '\n';
            }
            if (g.lines[0].empty()) continue;
            auto &v = by_en[joined];
            if (v.empty()) order.push_back(joined);
            v.push_back(std::move(g));
        }
        for (const std::string &joined : order) {
            const std::vector<GroupText> &v = by_en[joined];
            bool same = true;
            for (const GroupText &g : v) same = same && g.zh == v[0].zh;
            if (!same) continue;
            /* same line count: the per-line dictionaries own it (see above) */
            if (group_split_lines(v[0].zh).size() == v[0].lines.size()) continue;
            s_group_text_heads[v[0].lines[0]].push_back(v[0]);
            ++count;
        }
    }
    /* longest first: a 3-line description beats the 2-line one it starts with */
    for (auto &kv : s_group_stat_heads)
        std::stable_sort(kv.second.begin(), kv.second.end(),
                         [](const GroupStat &a, const GroupStat &b) { return a.keys.size() > b.keys.size(); });
    for (auto &kv : s_group_text_heads)
        std::stable_sort(kv.second.begin(), kv.second.end(),
                         [](const GroupText &a, const GroupText &b) { return a.lines.size() > b.lines.size(); });
    return count;
}

/* One forward pattern in, under its case-preserved key `cased` (normalised, not lower-cased).
** overwrite = the merged map's rule for this entry (a purpose-built entry overwrites; a line
** derived from a multi-line key only fills an empty slot). A second case form of a key starts
** keeping every form's own plan (s_forward_pattern_cased). */
static void put_forward_pattern(const std::string &cased, FillPlan &&plan, bool overwrite) {
    const std::string lk = to_ascii_lower(cased);
    auto it = s_forward_pattern.find(lk);
    if (it == s_forward_pattern.end()) {
        s_forward_pattern.emplace(lk, std::move(plan));
        s_forward_pattern_case_of[lk] = cased;
        return;
    }
    std::string &owner = s_forward_pattern_case_of[lk];
    if (owner != cased) {
        s_forward_pattern_cased.emplace(owner, it->second);   /* the current form keeps its plan */
        if (overwrite) s_forward_pattern_cased[cased] = plan;
        else s_forward_pattern_cased.emplace(cased, plan);
    } else if (overwrite) {
        auto c = s_forward_pattern_cased.find(cased);
        if (c != s_forward_pattern_cased.end()) c->second = plan;
    }
    if (overwrite) {
        it->second = std::move(plan);
        owner = cased;
    }
}

/* The forward plan for an input's pattern: its exact case form when two forms
** collided (s_forward_pattern_cased), else the lower-case map. `cased` is the
** input after normalize_placeholders + digits_to_hash, NOT lower-cased. */
static const FillPlan *find_forward_pattern(const std::string &cased) {
    if (!s_forward_pattern_cased.empty()) {
        auto c = s_forward_pattern_cased.find(cased);
        if (c != s_forward_pattern_cased.end()) return &c->second;
    }
    auto it = s_forward_pattern.find(to_ascii_lower(cased));
    return it == s_forward_pattern.end() ? nullptr : &it->second;
}

static int load_json_translations(const std::string &filepath, bool is_base_items,
                                  std::unordered_map<std::string, std::string> *sink = nullptr,
                                  std::unordered_map<std::string, FillPlan> *pattern_sink = nullptr,
                                  bool source_only = false,
                                  const std::unordered_set<std::string> *exclude = nullptr,
                                  const std::unordered_set<std::string> *exact_only = nullptr) {
    /* source_only (meta.json "source_only"): the file feeds ONLY its source dictionary (exact
    ** and pattern sinks); nothing reaches the merged map, forward patterns, reverse maps or the
    ** glossary, so unmarked lookups and paste never see it.
    ** exclude (display_exclude.json, this file's list): those keys skip every forward table,
    ** merged and source copy alike (see s_display_exclude); reverse entries and the base-type
    ** list are built as before.
    ** exact_only (exact_only.json, this file's list): exact tables only (see s_exact_only). */
    const bool merge = !source_only;
    std::unordered_set<std::string> listed_hit;   /* listed keys that are keys of this file */
    int twin_skipped = 0, line_skipped = 0;
    std::unordered_set<std::string> eo_hit;       /* exact_only keys that are keys of this file */
    int eo_twins = 0;
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return 0;
    /* 整檔讀進記憶體再解析:nlohmann 走 istream 是逐字元取,7 MB 的 stats.json
    ** 差了一倍。內容完全相同(binary 開檔,BOM 由 lexer 自己跳)。 */
    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    /* bare file name, e.g. "stats.json" — recorded as the origin of every key
    ** this call wins, so the trace can name who shadowed whom */
    std::string origin = filepath;
    {
        size_t slash = origin.find_last_of("\\/");
        if (slash != std::string::npos) origin = origin.substr(slash + 1);
    }

    njson doc;
    const tr_clock::time_point t_parse = tr_clock::now();
    try {
        doc = njson::parse(raw);
    } catch (const njson::parse_error &e) {
        char msg[512];
        snprintf(msg, sizeof(msg), "[pob-proxy] JSON parse error in %s: %s\n",
                 filepath.c_str(), e.what());
        OutputDebugStringA(msg);
        /* A broken dictionary is ONE FILE quietly contributing zero entries (a whole category
        ** reverts to English), not an exception out of translation_init; the outer catch
        ** almost never fires, so log here. */
        PobLog::Error("i18n", "dictionary file did not parse, its entries are all "
                              "missing: " + filepath + " - " + e.what());
        return 0;
    }

    s_init_stats.parse_ms += tr_ms_since(t_parse);
    const tr_clock::time_point t_build = tr_clock::now();

    if (!doc.contains("entries") || !doc["entries"].is_object()) return 0;

    /* Check is_base_items flag from JSON (overrides parameter if present) */
    if (doc.contains("is_base_items") && doc["is_base_items"].is_boolean()) {
        is_base_items = doc["is_base_items"].get<bool>();
    }

    /* Entries GGG ships as ONE string containing its own "\n". The game and a pasted item show
    ** separate lines, so the joined dictionary key matches neither half.
    ** Registered per line IN ADDITION to the joined form, only when the sides line up (same line
    ** count, same placeholder/number count per line; a mismatch means the break moved and
    ** pairing would be positional guessing). Existing entries are never overwritten: a
    ** purpose-built single-line entry beats one derived by splitting.
    ** Two spellings of the break exist: stats.json has the literal chars '\' 'n', ui.json a
    ** real 0x0A. Both split. */
    auto next_break = [](const std::string &s, size_t from, size_t &len) -> size_t {
        size_t a = s.find('\n', from);
        size_t b = s.find("\\n", from);
        if (a != std::string::npos && (b == std::string::npos || a < b)) { len = 1; return a; }
        if (b != std::string::npos) { len = 2; return b; }
        len = 0;
        return std::string::npos;
    };
    auto has_break = [&next_break](const std::string &s) {
        size_t len = 0;
        return next_break(s, 0, len) != std::string::npos;
    };
    auto split_lines_nl = [&next_break](const std::string &s) {
        std::vector<std::string> out;
        size_t p = 0;
        while (true) {
            size_t len = 0;
            size_t nl = next_break(s, p, len);
            if (nl == std::string::npos) { out.push_back(s.substr(p)); break; }
            out.push_back(s.substr(p, nl - p));
            p = nl + len;
        }
        return out;
    };
    auto slot_count = [](const std::string &s) {
        /* digits_to_hash collapses each number to one '#'; count those plus any
        ** literal {N} the entry still carries. */
        std::string h = digits_to_hash(normalize_placeholders(s));
        int n = 0;
        for (char c : h) if (c == '#') n++;
        return n;
    };

    int count = 0;
    for (auto &[key, val] : doc["entries"].items()) {
        /* Support both flat ("key": "value") and structured ("key": {"翻譯": "value", ...}) formats */
        std::string value;
        std::string color_prefix;
        if (val.is_string()) {
            value = val.get<std::string>();
        } else if (val.is_object()) {
            /* Structured format: { "翻譯": "...", "色碼標籤": "^x...", "分類": "...", ... } */
                if (val.contains("\xe7\xbf\xbb\xe8\xad\xaf")) {
                auto &trans = val["\xe7\xbf\xbb\xe8\xad\xaf"];
                if (!trans.is_string()) continue;
                value = trans.get<std::string>();
            } else {
                continue;
            }
            /* Read color prefix if present */
            if (val.contains("\xe8\x89\xb2\xe7\xa2\xbc\xe6\xa8\x99\xe7\xb1\xa4")) {
                auto &clr = val["\xe8\x89\xb2\xe7\xa2\xbc\xe6\xa8\x99\xe7\xb1\xa4"];
                if (clr.is_string()) {
                    color_prefix = clr.get<std::string>();
                }
            }
        } else {
            continue;
        }
        if (key.empty() || value.empty()) continue;
        bool fwd = true;
        if (exclude) {
            if (exclude->count(key)) {
                listed_hit.insert(key);
                fwd = false;
            } else if (key.find('[') != std::string::npos) {
                /* "[Curse|Cursed] Enemies..." is the listed "Cursed Enemies..." again: its forward
                ** pattern is built from the stripped key */
                const std::string bare = strip_brackets(key);
                if (bare != key && exclude->count(bare)) { twin_skipped++; fwd = false; }
            }
        }
        bool eo = false;
        if (exact_only) {
            if (exact_only->count(key)) {
                eo_hit.insert(key);
                eo = true;
            } else if (key.find('[') != std::string::npos) {
                const std::string bare = strip_brackets(key);
                if (bare != key && exact_only->count(bare)) { eo_twins++; eo = true; }
            }
        }

        /* Apply color prefix: prepend to both key and value for lookup matching.
         * POB sends "^x7F7F7FArmour:" and expects "^x7F7F7F護甲:" back. */
        std::string full_key = color_prefix.empty() ? key : (color_prefix + key);
        std::string full_value = color_prefix.empty() ? value : (color_prefix + value);

        std::string clean_key = strip_brackets(full_key);
        std::string clean_value = strip_brackets(full_value);

        /* Insert-or-overwrite (last wins, as always) in ONE hash lookup; the
        ** duplicate diagnostic reads the old value before it is replaced. */
        if (merge && fwd) {
            auto [it, inserted] = s_translations.try_emplace(full_key, full_value);
            if (!inserted) {
                if (it->second != full_value) {
                    char msg[512];
                    snprintf(msg, sizeof(msg), "[pob-proxy] JSON duplicate key: \"%.80s\" old=\"%.60s\" new=\"%.60s\" in %s\n",
                             full_key.c_str(), it->second.c_str(), full_value.c_str(), filepath.c_str());
                    OutputDebugStringA(msg);
                }
                it->second = full_value;
            }
            if (eo) s_exact_only_keys.insert(full_key);
            else s_exact_only_keys.erase(full_key);
        }
        if (sink && fwd) (*sink)[full_key] = full_value;
        if (merge) {
            s_reverse[full_value] = full_key;
            s_reverse_origin[full_value] = origin;
            if (clean_value != full_value) {
                s_reverse[clean_value] = clean_key;
                s_reverse_origin[clean_value] = origin;
            }
        }

        if (merge && is_base_items) {
            s_reverse_bases[clean_value] = clean_key;
            /* Item.lua strips any " (...)" from the base name before appending it to the title,
            ** so the clean form is what reaches us; the full key is recorded too, at no cost. */
            s_base_items_en.insert(clean_key);
            s_base_items_en.insert(full_key);
            if (clean_value != full_value) {
                s_reverse_bases[full_value] = full_key;
            }
            std::string variant = clean_value;
            replace_all(variant, "\xe5\xb7\x96", "\xe5\xb2\xa9"); /* 巖→岩 */
            if (variant != clean_value) {
                s_reverse_bases[variant] = clean_key;
            }
        }

        /* exact-only: the exact tables above are all it gets */
        if (eo) {
            count++;
            continue;
        }

        /* Build pattern maps (use clean keys without color prefix for pattern matching) */
        std::string norm_value = normalize_whitespace(digits_to_hash(normalize_placeholders(clean_value)));
        std::string norm_key = normalize_whitespace(digits_to_hash(normalize_placeholders(clean_key)));
        /* 兩個樣板的槽位各算一次,給下面最多三份計畫共用(template_slots 是純函式,
        ** 結果與每次重算逐位元相同)。 */
        std::vector<TmplSlot> slots_key, slots_value;
        bool slots_done = false;
        auto ensure_slots = [&]() {
            if (slots_done) return;
            slots_done = true;
            slots_key = template_slots(clean_key);
            slots_value = template_slots(clean_value);
        };
        FillPlan reverse_plan;   /* value → key,norm_value 與 fuzzy_value 共用 */
        bool reverse_plan_done = false;
        auto ensure_reverse_plan = [&]() {
            if (reverse_plan_done) return;
            reverse_plan_done = true;
            ensure_slots();
            reverse_plan = build_fill_plan_slots(slots_value, slots_key, norm_key);
        };
        if (norm_value != clean_value || norm_key != clean_key) {
            if (merge) {
                ensure_reverse_plan();
                s_reverse_pattern[norm_value] = reverse_plan;
                s_reverse_pattern_origin[norm_value] = origin;
                register_compact_pattern(norm_value, reverse_plan, origin);
            } else {
                ensure_slots();
            }
            /* A forward pattern fills the Chinese {N} from the line's values. When the Chinese uses
            ** a {N} the English key lacks, the only feed is a number the English wrote out as
            ** wording -- an unverifiable guess, so no forward pattern is made (the exact entry is
            ** still blocked by the placeholder guard at lookup time).
            ** The test is a COUNT, not the numbering: only when the English has fewer placeholder
            ** occurrences than the Chinese has distinct {N}. Enough placeholders numbered
            ** differently (key {0} / value {1}: data renumbering slips) keep the pattern and its
            ** in-order fallback. */
            bool fwd_ok = true;
            {
                std::vector<int> key_ix;
                normalize_placeholders(clean_key, &key_ix);
                size_t key_ph = 0;
                for (int n : key_ix) if (n >= 0) key_ph++;
                if (key_ph < placeholder_index_set(clean_value).size()) fwd_ok = false;
            }
            if (fwd_ok) {
                std::string fwd_key = to_ascii_lower(norm_key);
                FillPlan fwd_plan = build_fill_plan_slots(slots_key, slots_value, norm_value);
                /* Source copy follows the merged map's rule here: overwrite. */
                if (pattern_sink && fwd) (*pattern_sink)[fwd_key] = fwd_plan;
                if (merge && fwd) put_forward_pattern(norm_key, std::move(fwd_plan), /*overwrite=*/true);
            }
        }
        std::string fuzzy_value = merge ? normalize_chinese_synonyms(norm_value) : norm_value;
        if (fuzzy_value != norm_value) {
            /* 同一份計畫:模糊化只改查表用的鍵,不動槽位順序(synonyms.json 的規則
            ** 都不會刪掉 '#',所以模糊鍵命中時輸入的格數仍與樣板一致)。 */
            ensure_reverse_plan();
            s_reverse_pattern[fuzzy_value] = reverse_plan;
            s_reverse_pattern_origin[fuzzy_value] = origin;
        }

        /* Per-line registration for entries carrying GGG's own newline. */
        if (has_break(clean_key)) {
            std::vector<std::string> klines = split_lines_nl(clean_key);
            std::vector<std::string> vlines = split_lines_nl(clean_value);
            bool aligned = klines.size() == vlines.size() && klines.size() > 1;
            for (size_t i = 0; aligned && i < klines.size(); i++) {
                if (klines[i].empty() || vlines[i].empty()) aligned = false;
                else if (slot_count(klines[i]) != slot_count(vlines[i])) aligned = false;
                else if (placeholder_index_set(klines[i]) != placeholder_index_set(vlines[i])) aligned = false;
            }
            if (aligned) {
                for (size_t i = 0; i < klines.size(); i++) {
                    const std::string &k = klines[i], &v = vlines[i];
                    /* a line that is itself a listed key of this file stays out
                    ** (an unlisted multi-line key must not bring it back) */
                    const bool line_fwd = fwd && !(exclude && exclude->count(k));
                    if (fwd && !line_fwd) line_skipped++;
                    if (line_fwd && merge && !s_translations.count(k)) {
                        s_translations[k] = v;
                    }
                    /* The source copy ignores the merged map but keeps the merged rule inside the
                    ** file: a derived line never replaces an existing entry, and a later
                    ** purpose-built entry overwrites it. Same as the pattern sink below. */
                    if (line_fwd && sink && !sink->count(k)) (*sink)[k] = v;
                    if (merge && !s_reverse.count(v)) {
                        s_reverse[v] = k;
                        s_reverse_origin[v] = origin;
                    }
                    std::string nv = normalize_whitespace(digits_to_hash(normalize_placeholders(v)));
                    std::string nk = normalize_whitespace(digits_to_hash(normalize_placeholders(k)));
                    if (nv != v || nk != k) {
                        if (merge && !s_reverse_pattern.count(nv)) {
                            s_reverse_pattern[nv] = build_fill_plan(v, k, nk);
                            s_reverse_pattern_origin[nv] = origin;
                            register_compact_pattern(nv, s_reverse_pattern[nv], origin);
                        }
                        std::string lk = to_ascii_lower(nk);
                        if (line_fwd && merge) put_forward_pattern(nk, build_fill_plan(k, v, nv), /*overwrite=*/false);
                        /* Source copy keeps this spot's rule too: first one wins. */
                        if (line_fwd && pattern_sink && !pattern_sink->count(lk))
                            (*pattern_sink)[lk] = build_fill_plan(k, v, nv);
                    }
                }
            }
        }
        count++;
    }

    s_init_stats.build_ms += tr_ms_since(t_build);
    s_init_stats.files++;
    if (exclude) {
        /* what the list actually touched in THIS file: a source-only file never merges (only its
        ** source copy loses keys); a merged file without a source dictionary has no source copy */
        const char *where = !merge ? "its source copy (source-only file, never merged)"
                          : sink   ? "the merged forward tables and its source copy"
                                   : "the merged forward tables";
        char msg[480];
        snprintf(msg, sizeof(msg), "[pob-proxy] display_exclude %s: %d of %d listed keys are keys of this file, "
                 "kept out of %s (+%d [X|Y]-markup twins, +%d derived lines)\n", origin.c_str(),
                 (int)listed_hit.size(), (int)exclude->size(), where, twin_skipped, line_skipped);
        OutputDebugStringA(msg);
        if (listed_hit.size() != exclude->size())
            PobLog::Diag("i18n", "display_exclude.json lists " + std::to_string(exclude->size()) + " keys for " +
                                 origin + "; " + std::to_string(listed_hit.size()) + " of them are keys of that file " +
                                 "(the rest match nothing, or only lines derived from its multi-line keys)");
    }
    if (exact_only) {
        char msg[400];
        snprintf(msg, sizeof(msg), "[pob-proxy] exact_only %s: %d of %d listed keys are keys of this file, "
                 "matched whole-sentence only (+%d [X|Y]-markup twins)\n", origin.c_str(),
                 (int)eo_hit.size(), (int)exact_only->size(), eo_twins);
        OutputDebugStringA(msg);
        if (eo_hit.size() != exact_only->size())
            PobLog::Diag("i18n", "exact_only.json lists " + std::to_string(exact_only->size()) + " keys for " +
                                 origin + "; " + std::to_string(eo_hit.size()) + " of them are keys of that file");
    }
    return count;
}

static bool load_json_meta(const std::string &filepath,
                           std::vector<std::string> &load_order,
                           std::vector<std::string> &source_only,
                           std::vector<std::string> &source_dicts,
                           bool &source_lookup) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;

    njson doc;
    try {
        doc = njson::parse(file);
    } catch (...) {
        /* meta.json decides the merge order. Losing it silently changes which file wins a
        ** shadowed key (a few WRONG translations, hard to notice), so say it out loud. */
        PobLog::Error("i18n", "meta.json did not parse; dictionary load order falls "
                              "back to the default and shadowed keys may resolve to "
                              "a different file");
        return false;
    }

    if (doc.contains("load_order") && doc["load_order"].is_array()) {
        for (auto &item : doc["load_order"]) {
            if (item.is_string())
                load_order.push_back(item.get<std::string>());
        }
    }

    /* Optional: files in load_order that feed only their source dictionary. Absent = every file
    ** merges. Kept for compatibility; shipped data uses source_dicts. */
    if (doc.contains("source_only") && doc["source_only"].is_array()) {
        for (auto &item : doc["source_only"]) {
            if (item.is_string())
                source_only.push_back(item.get<std::string>());
        }
    }

    /* Optional: files NOT in load_order that feed only their source dictionary or the id table
    ** (*_by_id). Keeping them out of load_order is the point: an older engine reads load_order
    ** only, so it never merges an affix-name file or takes numeric id keys for English. */
    if (doc.contains("source_dicts") && doc["source_dicts"].is_array()) {
        for (auto &item : doc["source_dicts"]) {
            if (item.is_string())
                source_dicts.push_back(item.get<std::string>());
        }
    }

    /* Optional: true = this locale's data is organised for per-source lookup. Absent = only
    ** "gems" (exact entries) can be selected, everything else answers as unmarked. */
    source_lookup = doc.contains("source_lookup") && doc["source_lookup"].is_boolean() &&
                    doc["source_lookup"].get<bool>();
    s_meta_source = (doc.contains("source") && doc["source"].is_string()) ? doc["source"].get<std::string>()
                                                                           : std::string();

    if (doc.contains("incomplete_translation_whitelist") &&
        doc["incomplete_translation_whitelist"].is_array()) {
        for (auto &item : doc["incomplete_translation_whitelist"]) {
            if (item.is_string())
                s_whitelist_terms.push_back(item.get<std::string>());
        }
    }

    if (doc.contains("glossary_blacklist") && doc["glossary_blacklist"].is_array()) {
        for (auto &item : doc["glossary_blacklist"]) {
            if (item.is_string())
                s_glossary_blacklist.push_back(item.get<std::string>());
        }
    }

    return true;
}

static bool load_json_item_metadata(const std::string &filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;

    njson doc;
    try {
        doc = njson::parse(file);
    } catch (...) {
        return false;
    }

    auto load_mappings = [](const njson &arr, std::vector<HeaderMapping_Dyn> &vec) {
        if (!arr.is_array()) return;
        for (auto &item : arr) {
            if (item.contains("zh") && item.contains("en") &&
                item["zh"].is_string() && item["en"].is_string()) {
                vec.push_back({item["zh"].get<std::string>(),
                               item["en"].get<std::string>()});
            }
        }
    };

    if (doc.contains("headers"))        load_mappings(doc["headers"], s_item_headers_vec);
    if (doc.contains("rarity_values"))  load_mappings(doc["rarity_values"], s_rarity_values_vec);
    if (doc.contains("item_classes"))   load_mappings(doc["item_classes"], s_item_classes_vec);
    if (doc.contains("influence_tags")) load_mappings(doc["influence_tags"], s_influence_tags_vec);
    if (doc.contains("status_lines"))   load_mappings(doc["status_lines"], s_status_lines_vec);
    if (doc.contains("mod_annotations")) load_mappings(doc["mod_annotations"], s_mod_annotations_vec);
    if (doc.contains("composite_prefixes") && doc["composite_prefixes"].is_array()) {
        for (auto &item : doc["composite_prefixes"]) {
            if (!item.contains("zh") || !item.contains("en") ||
                !item["zh"].is_string() || !item["en"].is_string()) continue;
            CompositePrefix c;
            c.chinese = item["zh"].get<std::string>();
            c.english = item["en"].get<std::string>();
            c.english_enchant = (item.contains("en_enchant") && item["en_enchant"].is_string())
                                    ? item["en_enchant"].get<std::string>() : c.english;
            s_composite_prefixes_vec.push_back(c);
        }
    }

    if (doc.contains("affix_names") && doc["affix_names"].is_object()) {
        for (auto it = doc["affix_names"].begin(); it != doc["affix_names"].end(); ++it) {
            if (it.value().is_string())
                s_affix_names[it.key()] = it.value().get<std::string>();
        }
    }
    if (doc.contains("header_values") && doc["header_values"].is_object()) {
        for (auto it = doc["header_values"].begin(); it != doc["header_values"].end(); ++it) {
            load_mappings(it.value(), s_header_values[it.key()]);
        }
    }

    /* The annotation scan consumes matches, so a marker that is a prefix of a longer one must
    ** never be tried first; re-sort here so a hand-edited file cannot break it silently. */
    std::stable_sort(s_mod_annotations_vec.begin(), s_mod_annotations_vec.end(),
                     [](const HeaderMapping_Dyn &a, const HeaderMapping_Dyn &b) {
                         return a.chinese.size() > b.chinese.size();
                     });

    if (doc.contains("header_words") && doc["header_words"].is_object()) {
        for (auto it = doc["header_words"].begin(); it != doc["header_words"].end(); ++it) {
            auto &vec = s_header_words[it.key()];
            load_mappings(it.value(), vec);
            std::stable_sort(vec.begin(), vec.end(), [](const HeaderMapping_Dyn &a, const HeaderMapping_Dyn &b) {
                return a.chinese.size() > b.chinese.size();
            });
        }
    }
    if (doc.contains("header_names") && doc["header_names"].is_array()) {
        for (auto &item : doc["header_names"]) {
            if (item.is_string()) s_header_names.insert(item.get<std::string>());
        }
    }
    if (doc.contains("unscalable_markers") && doc["unscalable_markers"].is_array()) {
        for (auto &item : doc["unscalable_markers"]) {
            if (item.is_string() && !item.get<std::string>().empty())
                s_unscalable_markers.push_back(item.get<std::string>());
        }
    }

    if (doc.contains("skip_patterns") && doc["skip_patterns"].is_array()) {
        for (auto &item : doc["skip_patterns"]) {
            if (item.is_string()) s_skip_patterns_vec.push_back(item.get<std::string>());
        }
    }
    if (doc.contains("mod_suffixes") && doc["mod_suffixes"].is_array()) {
        for (auto &item : doc["mod_suffixes"]) {
            if (item.is_string()) s_mod_suffixes_vec.push_back(item.get<std::string>());
        }
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "[pob-proxy] Loaded item_metadata: %d headers, %d classes, %d influence, %d status, %d annotations\n",
             (int)s_item_headers_vec.size(), (int)s_item_classes_vec.size(),
             (int)s_influence_tags_vec.size(), (int)s_status_lines_vec.size(),
             (int)s_mod_annotations_vec.size());
    OutputDebugStringA(msg);

    return true;
}

static bool load_json_synonyms(const std::string &filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;

    njson doc;
    try {
        doc = njson::parse(file);
    } catch (...) {
        return false;
    }

    /* Load replacements */
    if (doc.contains("replacements") && doc["replacements"].is_array()) {
        for (auto &item : doc["replacements"]) {
            if (item.contains("from") && item.contains("to") &&
                item["from"].is_string() && item["to"].is_string()) {
                SynonymRule rule;
                rule.type = SynonymRule::REPLACE;
                rule.from = item["from"].get<std::string>();
                rule.to = item["to"].get<std::string>();
                s_synonym_rules.push_back(std::move(rule));
            }
        }
    }

    /* Load removals */
    if (doc.contains("removals") && doc["removals"].is_array()) {
        for (auto &item : doc["removals"]) {
            if (item.is_string()) {
                SynonymRule rule;
                rule.type = SynonymRule::REMOVE;
                rule.from = item.get<std::string>();
                s_synonym_rules.push_back(std::move(rule));
            }
        }
    }

    /* Load protected compounds */
    if (doc.contains("protected_compounds") && doc["protected_compounds"].is_array()) {
        for (auto &item : doc["protected_compounds"]) {
            if (item.contains("compound") && item.contains("component") &&
                item["compound"].is_string() && item["component"].is_string()) {
                SynonymRule rule;
                rule.type = SynonymRule::PROTECT_REMOVE;
                rule.from = item["component"].get<std::string>();
                rule.protect = item["compound"].get<std::string>();
                s_synonym_rules.push_back(std::move(rule));
            }
        }
    }

    /* Load strip_punctuation */
    if (doc.contains("strip_punctuation") && doc["strip_punctuation"].is_array()) {
        for (auto &item : doc["strip_punctuation"]) {
            if (item.is_string()) s_strip_punctuation.push_back(item.get<std::string>());
        }
    }

    /* Load strip_space_around */
    if (doc.contains("strip_space_around") && doc["strip_space_around"].is_array()) {
        for (auto &item : doc["strip_space_around"]) {
            if (item.is_string()) s_strip_space_around.push_back(item.get<std::string>());
        }
    }

    /* Load strip_measure_words */
    if (doc.contains("strip_measure_words") && doc["strip_measure_words"].is_array()) {
        for (auto &item : doc["strip_measure_words"]) {
            if (item.contains("pattern") && item.contains("replacement") &&
                item["pattern"].is_string() && item["replacement"].is_string()) {
                s_strip_measure_words.emplace_back(
                    item["pattern"].get<std::string>(),
                    item["replacement"].get<std::string>());
            }
        }
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "[pob-proxy] Loaded synonyms: %d rules, %d punctuation, %d space_around\n",
             (int)s_synonym_rules.size(), (int)s_strip_punctuation.size(),
             (int)s_strip_space_around.size());
    OutputDebugStringA(msg);

    return true;
}

/* A per-file key list ({"version":1,"entries":{file:[keys]}}: display_exclude.json
** -> s_display_exclude, exact_only.json -> s_exact_only). Absent file: empty list. A file that
** does not parse, or an unknown version, is ignored as a whole (logged). */
using KeyLists = std::unordered_map<std::string, std::unordered_set<std::string>>;
static int load_json_key_lists(const std::string &filepath, const char *name, const char *ignored,
                               KeyLists &out) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return 0;
    njson doc;
    try {
        doc = njson::parse(file);
    } catch (const njson::parse_error &e) {
        PobLog::Error("i18n", std::string(name) + " did not parse, " + ignored + ": " + filepath + " - " + e.what());
        return 0;
    }
    if (!doc.is_object() || !doc.contains("version") || !doc["version"].is_number_integer() ||
        doc["version"].get<int>() != 1 || !doc.contains("entries") || !doc["entries"].is_object()) {
        PobLog::Error("i18n", std::string(name) + " is not version 1 {\"entries\":{file:[keys]}}, " + ignored + ": " + filepath);
        return 0;
    }
    int n = 0;
    for (auto &[fname, keys] : doc["entries"].items()) {
        if (!keys.is_array()) continue;
        auto &set = out[fname];
        for (auto &k : keys)
            if (k.is_string() && set.insert(k.get<std::string>()).second) n++;
    }
    return n;
}
static const std::unordered_set<std::string> *key_list_for(const KeyLists &lists, const std::string &filename) {
    auto it = lists.find(filename);
    return it == lists.end() || it->second.empty() ? nullptr : &it->second;
}

/* Try to load JSON translation files. Returns true if JSON mode activated. */
static bool try_load_json(const std::string &base_dir) {
    /* Layout: Data/{game}/{locale}/ (poe1 and poe2 in separate folders). POB_LOCALE "" or "poe1"
     * defaults to "zh-rTW". */
    std::string effective_locale;
    if (s_locale.empty() || s_locale == "poe1") {
        effective_locale = "zh-rTW";
    } else {
        effective_locale = s_locale;
    }

    /* Game segment: poe1 (default) or poe2, selected via POB_GAME env. */
    char gamebuf[64];
    const char *game_env = win_env("POB_GAME", gamebuf, sizeof(gamebuf));
    std::string game = (game_env && *game_env) ? game_env : "poe1";

    /* Try paths in priority order */
    std::string json_dir;
    std::string meta_path;
    bool found = false;

    /* POB_ZH_DATADIR: the dictionary folder for THIS game, kept outside the install so a
     * translator can edit a working copy and see it rendered by the real POB, and updates
     * cannot overwrite it. It points at the folder that directly contains the <locale>
     * sub-folders (what you get by copying Data\poe1 somewhere), NOT at a Data root.
     * The launcher only sets it when usable; the fallback below is for a hand-set wrong
     * value, where shipping no translations would be worse than ignoring it.
     * The whole dictionary path is ANSI (GetEnvironmentVariableA, std::ifstream), so the
     * folder has to be spellable in the system codepage. */
    char dirbuf[1024];
    const char *data_env = win_env("POB_ZH_DATADIR", dirbuf, sizeof(dirbuf));
    if (data_env && *data_env) {
        std::string root = data_env;
        if (root.back() != '\\' && root.back() != '/') root += '\\';
        json_dir = root + effective_locale + "\\";
        meta_path = json_dir + "meta.json";
        found = GetFileAttributesA(meta_path.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!found) {
            /* Leave a trace: falling back silently is how "my external folder is
             * being ignored" becomes unexplainable. */
            char why[1400];
            snprintf(why, sizeof(why),
                     "[pob-proxy] POB_ZH_DATADIR has no %s dictionary (%s); using built-in\n",
                     effective_locale.c_str(), meta_path.c_str());
            OutputDebugStringA(why);
        }
    }

    if (!found) {
        const char *candidates[] = {
            "Data\\",                /* current layout: Data/{game}/{locale}/ */
            "Data\\Translate\\",     /* legacy layout */
            "..\\Data\\Translate\\", /* legacy pobcharm4-ui layout */
        };
        for (const char *prefix : candidates) {
            json_dir = base_dir + prefix + game + "\\" + effective_locale + "\\";
            meta_path = json_dir + "meta.json";
            if (GetFileAttributesA(meta_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                found = true;
                break;
            }
        }
    }

    if (!found) return false;

    char msg[256];
    snprintf(msg, sizeof(msg), "[pob-proxy] Found JSON translations at: %s\n", json_dir.c_str());
    OutputDebugStringA(msg);

    /* Load meta.json */
    std::vector<std::string> load_order;
    std::vector<std::string> source_only;
    std::vector<std::string> source_dicts;
    bool source_lookup = false;
    if (!load_json_meta(meta_path, load_order, source_only, source_dicts, source_lookup)) {
        OutputDebugStringA("[pob-proxy] Failed to parse meta.json\n");
        return false;
    }
    if (!s_game_from_env) {
        s_rules = s_meta_source == "poe2" ? &kGameRulesPoe2 : &kGameRulesPoe1;
    } else if (!s_meta_source.empty() && s_meta_source != s_rules->game) {
        /* The launcher picks the folder by the same POB_GAME, so this is a hand-set
        ** POB_ZH_DATADIR (or a data pack) holding the other game's dictionaries:
        ** the rules follow the game being run, the words come from that folder. */
        PobLog::Error("i18n", std::string("POB_GAME=") + s_rules->game + " but meta.json source=" +
                                  s_meta_source + " (" + meta_path + "); running " + s_rules->game +
                                  "'s rules -- this dictionary folder is probably the other game's");
    }

    /* Load synonyms.json */
    load_json_synonyms(json_dir + "synonyms.json");

    /* Load item_metadata.json */
    load_json_item_metadata(json_dir + "item_metadata.json");

    /* Load display_exclude.json (before any flat dictionary: it gates them) */
    if (const int xn = load_json_key_lists(json_dir + kDisplayExcludeFile, kDisplayExcludeFile,
                                           "nothing is excluded", s_display_exclude)) {
        snprintf(msg, sizeof(msg), "[pob-proxy] Loaded %d display-exclude keys from %s\n", xn, kDisplayExcludeFile);
        OutputDebugStringA(msg);
    }
    /* exact_only.json: same shape, same timing (it gates the same files) */
    if (const int en = load_json_key_lists(json_dir + kExactOnlyFile, kExactOnlyFile,
                                           "nothing is restricted", s_exact_only)) {
        snprintf(msg, sizeof(msg), "[pob-proxy] Loaded %d exact-only keys from %s\n", en, kExactOnlyFile);
        OutputDebugStringA(msg);
    }

    /* Load translation files in order */
    int totalEntries = 0;
    int fileCount = 0;
    DictSwitches switches;
    switches.read();
    for (auto &filename : load_order) {
        if (switches.skip(filename)) continue;
        std::string filepath = json_dir + filename;
        /* The stat file never merges (its keys are stat ids): index it only
        ** when something could ask, i.e. with source_lookup. */
        if (filename == kStatDictFile || filename == kStatWrapperFile) {
            if (!source_lookup) continue;
            int count = filename == kStatDictFile ? load_json_stat_dict(filepath)
                                                  : load_json_stat_wrappers(filepath);
            if (count > 0) {
                totalEntries += count;
                fileCount++;
            }
            continue;
        }
        /* Id-keyed files go to their own table and nowhere else. */
        {
            const SourceDict *idd = nullptr;
            for (const SourceDict &sd : kIdDicts)
                if (filename == sd.file) { idd = &sd; break; }
            if (idd) {
                /* Without source_lookup nothing can ever ask for an id: skip
                ** the file (it never merges either way). */
                if (!source_lookup) continue;
                int count = load_json_id_dict(filepath, s_id_dicts[idd->name], idd->name);
                if (count > 0) {
                    snprintf(msg, sizeof(msg), "[pob-proxy] Loaded %d id entries from %s\n",
                             count, filename.c_str());
                    OutputDebugStringA(msg);
                    totalEntries += count;
                    fileCount++;
                }
                continue;
            }
        }
        /* Files backing a source dictionary get a second copy of their own entries, untouched by
         * later files. Without source_lookup only gems (exact entries) can be selected, so no
         * other copy is built. */
        std::unordered_map<std::string, std::string> *sink = nullptr;
        std::unordered_map<std::string, FillPlan> *pattern_sink = nullptr;
        for (const SourceDict &sd : kSourceDicts) {
            if (filename == sd.file) {
                if (!source_lookup && filename != std::string(kLegacySource) + ".json") break;
                sink = &s_sources[sd.name];
                if (source_lookup) pattern_sink = &s_source_patterns[sd.name];
                /* create now: never rehash later */
                s_source_cache[sd.name];
                s_item_title_source_cache[sd.name];
                break;
            }
        }
        /* A file in source_dicts never merges, even when load_order lists it
        ** too: source_dicts says what the file IS, load_order only when to load. */
        const bool in_source_dicts =
            std::find(source_dicts.begin(), source_dicts.end(), filename) != source_dicts.end();
        if (in_source_dicts) {
            snprintf(msg, sizeof(msg), "[pob-proxy] %s is in both load_order and source_dicts; "
                     "loaded as source-only\n", filename.c_str());
            OutputDebugStringA(msg);
            PobLog::Diag("i18n", std::string(filename) + " is listed in both load_order and "
                                 "source_dicts; loaded as source-only (not merged)");
        }
        const bool only_source = in_source_dicts ||
            std::find(source_only.begin(), source_only.end(), filename) != source_only.end();
        /* source-only with nowhere to go (no flag, not gems): nothing to load */
        if (only_source && !sink) continue;
        int count = load_json_translations(filepath, false, sink, pattern_sink, only_source,
                                           key_list_for(s_display_exclude, filename),
                                           key_list_for(s_exact_only, filename));
        switches.loaded(filename, count);
        if (count > 0) {
            snprintf(msg, sizeof(msg), "[pob-proxy] Loaded %d entries from %s\n",
                     count, filename.c_str());
            OutputDebugStringA(msg);
            totalEntries += count;
            fileCount++;
        }
    }

    /* source_dicts: never merged. Files also in load_order were handled above (as source-only);
    ** only the ones outside load_order load here, and none without source_lookup. */
    for (auto &filename : source_dicts) {
        if (!source_lookup) break;
        if (std::find(load_order.begin(), load_order.end(), filename) != load_order.end())
            continue;
        std::string filepath = json_dir + filename;
        const SourceDict *idd = nullptr;
        for (const SourceDict &sd : kIdDicts)
            if (filename == sd.file) { idd = &sd; break; }
        int count = 0;
        if (filename == kStatDictFile) {
            count = load_json_stat_dict(filepath);
        } else if (filename == kStatWrapperFile) {
            count = load_json_stat_wrappers(filepath);
        } else if (idd) {
            count = load_json_id_dict(filepath, s_id_dicts[idd->name], idd->name);
        } else {
            const SourceDict *src = nullptr;
            for (const SourceDict &sd : kSourceDicts)
                if (filename == sd.file) { src = &sd; break; }
            if (!src) continue;   /* no source owns it: nothing could ever read it */
            s_source_cache[src->name];
            s_item_title_source_cache[src->name];
            count = load_json_translations(filepath, false, &s_sources[src->name],
                                           &s_source_patterns[src->name], /*source_only=*/true,
                                           key_list_for(s_display_exclude, filename),
                                           key_list_for(s_exact_only, filename));
        }
        if (count > 0) {
            snprintf(msg, sizeof(msg), "[pob-proxy] Loaded %d source-only entries from %s\n",
                     count, filename.c_str());
            OutputDebugStringA(msg);
            fileCount++;
        }
    }
    /* The line-group catalog: loaded by name (like synonyms.json), only when
    ** something could ask for it -- every lookup it serves needs source_lookup. */
    if (source_lookup) {
        const int gc = load_json_group_catalog(json_dir + kGroupFile);
        if (gc > 0) {
            snprintf(msg, sizeof(msg), "[pob-proxy] Loaded %d line groups from %s\n", gc, kGroupFile);
            OutputDebugStringA(msg);
            fileCount++;
        }
    }
    s_source_lookup = source_lookup;

    if (totalEntries == 0) {
        OutputDebugStringA("[pob-proxy] No JSON entries loaded\n");
        return false;
    }

    snprintf(msg, sizeof(msg), "[pob-proxy] JSON mode: loaded %d entries from %d files\n",
             totalEntries, fileCount);
    OutputDebugStringA(msg);
    if (!switches.items.empty()) {
        std::string line = "dictionary switches (" + json_dir + "): ";
        for (size_t i = 0; i < switches.notes.size(); i++)
            line += (i ? "; " : "") + switches.notes[i];
        if (switches.notes.empty()) line += "no switched file is in load_order";
        line += "; total " + std::to_string(totalEntries) + " entries from " +
                std::to_string(fileCount) + " files, " + std::to_string(s_translations.size()) + " keys";
        PobLog::Diag("i18n", line);
    }

    return true;
}

/* ========== Public API ========== */

void translation_wait_ready(void) {
    /* 第一次真的要用字典時留一筆:等了多久就是載入落在關鍵路徑上的量(0 = 完全
    ** 藏在 POB 自己的載入後面)。static bool 在快路徑上只多一個分支。 */
    static bool s_first_use_traced = false;
    if (s_ready.load(std::memory_order_acquire)) {
        /* worker 已經結束但還沒被收:join 只等它最後那行 trace,微秒級。沒收的話每次
        ** Restart 都會漏一個 thread 物件與 OS handle,因為 init_async 會直接覆寫指標。 */
        if (s_init_thread) {
            if (s_init_thread->joinable()) s_init_thread->join();
            delete s_init_thread;
            s_init_thread = nullptr;
        }
        if (!s_first_use_traced && s_initialized) {
            s_first_use_traced = true;
            startup_trace_mark("first dictionary use: %s", translation_get_init_stats());
        }
        return;
    }
    s_first_use_traced = true;
    const tr_clock::time_point t0 = tr_clock::now();
    /* 這是主執行緒唯一會為字典停下來的地方(等背景 worker 讀完 Data\*.json)。
    ** 卡在這裡的話畫面上什麼都還沒有,麵包屑是唯一看得出來的線索。 */
    HangWatch::Scope hw_wait("dict-wait");
    if (s_init_thread) {
        if (s_init_thread->joinable()) s_init_thread->join();
        delete s_init_thread;
        s_init_thread = nullptr;
    }
    /* join 之後一定 ready(worker 每個出口都設);若沒有 thread 卻不 ready,只可能
    ** 是 thread 建立失敗,那時 init_async 已經同步補跑過了。 */
    s_ready.store(true, std::memory_order_release);
    s_init_stats.wait_ms = tr_ms_since(t0);
    startup_trace_mark("translation dictionaries ready: %s", translation_get_init_stats());
}

void translation_init(void) {
    translation_wait_ready();
    if (s_initialized) return;
    s_initialized = true;
    s_init_stats.async = false; /* F3 reload 走這裡,統計行不能再說 in background */
    do_init();
}

void translation_init_async(void) {
    translation_wait_ready();
    if (s_initialized) return;
    s_initialized = true;
    s_ready.store(false, std::memory_order_release);
    s_init_stats = {};
    s_init_stats.async = true;
    try {
        s_init_thread = new std::thread([]() {
            /* 只接 C++ 例外。引擎是 /EHa,catch (...) 會連 access violation 一起吞掉,
            ** 把半建的 map 交給主執行緒 —— 那種錯誤寧可讓它誠實地崩在這裡。 */
            try {
                do_init();
            } catch (const std::exception &e) {
                char why[300];
                snprintf(why, sizeof(why), "[pob-proxy] translation_init threw (%s); dictionaries left as loaded so far\n", e.what());
                OutputDebugStringA(why);
                /* OutputDebugString reaches only a debugger; "half the interface in English" looks
                ** like a translation gap, so without this line the two cannot be told apart. */
                PobLog::Error("i18n", std::string("dictionary load threw: ") + e.what() +
                                          "; only the entries loaded so far are active");
            }
            s_ready.store(true, std::memory_order_release);
            /* 不經 translation_get_init_stats():它的 static 緩衝是主執行緒的 */
            startup_trace_mark("translation worker finished: %.0f ms (json parse %.0f, table build %.0f)",
                               s_init_stats.total_ms, s_init_stats.parse_ms, s_init_stats.build_ms);
        });
    } catch (...) {
        /* 開不了執行緒(資源耗盡):退回同步,行為與以前完全相同 */
        s_init_thread = nullptr;
        do_init();
        s_ready.store(true, std::memory_order_release);
    }
}

static void do_init(void) {
    const bool async = s_init_stats.async;
    s_init_stats = {};
    s_init_stats.async = async;
    const tr_clock::time_point t_init = tr_clock::now();

    /* The game is known before any dictionary is, passthrough mode included:
    ** POB_GAME, or (unset) meta.json "source" once try_load_json reads it. */
    {
        char gamebuf[64];
        const char *game_env = win_env("POB_GAME", gamebuf, sizeof(gamebuf));
        s_game_from_env = game_env && *game_env;
        s_rules = (s_game_from_env && strcmp(game_env, "poe2") == 0) ? &kGameRulesPoe2 : &kGameRulesPoe1;
    }

    s_locale = determine_locale();
    if (s_locale.empty()) {
        OutputDebugStringA("[pob-proxy] No translation locale configured, passthrough mode\n");
        return;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "[pob-proxy] Loading translations for locale: %s\n", s_locale.c_str());
    OutputDebugStringA(msg);

    std::string base_dir = get_exe_directory();

    /* Load JSON translations */
    if (!try_load_json(base_dir)) {
        OutputDebugStringA("[pob-proxy] Failed to load JSON translations\n");
        return;
    }
    OutputDebugStringA("[pob-proxy] Using JSON translation mode\n");
    /* Term glossary for substring replacement (longest-first, keyed lowercase, value as-is).
     * Several keys can collapse onto one term ("Breach:" in ui.json and "Breach" in
     * passives.json, or spellings differing only in case); s_translations is unordered, so
     * the pick is a rule on the keys themselves, not hash order:
     *   1. a key without a trailing colon beats one with it ("Breach" -> 裂痕, not 裂痕：);
     *   2. otherwise the lexicographically smallest original key.
     * A trailing colon is ':' or the full-width '：', on the key and, when the key had one, on
     * its translation too. */
    {
        auto strip_colon = [](std::string &s) -> bool {
            if (!s.empty() && s.back() == ':') { s.pop_back(); return true; }
            if (s.size() >= 3 && (unsigned char)s[s.size()-3] == 0xEF &&
                (unsigned char)s[s.size()-2] == 0xBC && (unsigned char)s[s.size()-1] == 0x9A) {
                s.erase(s.size() - 3);
                return true;
            }
            return false;
        };
        struct Pick { const std::string *key; bool colon; std::string trans; };
        std::unordered_map<std::string, Pick> picks;
        for (auto &kv : s_translations) {
            if (!is_glossary_candidate(kv.first)) continue;
            if (s_exact_only_keys.count(kv.first)) continue;   /* exact_only.json: whole sentence only */
            /* Strip trailing colon for glossary matching (e.g., "Total Mana:" → "Total Mana") */
            std::string term = kv.first;
            std::string trans = kv.second;
            const bool colon = strip_colon(term);
            if (colon) strip_colon(trans);
            if (term.size() < 4) continue;
            std::string term_lower = to_ascii_lower(term);
            auto it = picks.find(term_lower);
            if (it == picks.end()) {
                picks.emplace(std::move(term_lower), Pick{ &kv.first, colon, std::move(trans) });
                continue;
            }
            Pick &cur = it->second;
            const bool better = (cur.colon != colon) ? !colon : (kv.first < *cur.key);
            if (better) cur = Pick{ &kv.first, colon, std::move(trans) };
        }
        s_term_glossary.reserve(picks.size());
        for (auto &kv : picks) s_term_glossary.emplace_back(kv.first, std::move(kv.second.trans));
    }
    /* Longest terms first; equal lengths by term, so overlapping replacements never depend on
     * hash order (terms are unique after the pick above: a total order). */
    std::sort(s_term_glossary.begin(), s_term_glossary.end(),
        [](const auto &a, const auto &b) {
            if (a.first.size() != b.first.size()) return a.first.size() > b.first.size();
            return a.first < b.first;
        });

    s_init_stats.total_ms = tr_ms_since(t_init);
    snprintf(msg, sizeof(msg), "[pob-proxy] Loaded %d translations (JSON mode, %d pattern, %d glossary, %d base types) in %.0f ms (parse %.0f, build %.0f, %d files)\n",
             (int)s_translations.size(),
             (int)s_reverse_pattern.size(), (int)s_term_glossary.size(), (int)s_reverse_bases.size(),
             s_init_stats.total_ms, s_init_stats.parse_ms, s_init_stats.build_ms, s_init_stats.files);
    OutputDebugStringA(msg);

    /* Diagnostic: verify key patterns exist in map */
    {
        /* 每次使用減少 #% 充能 */
        const char *test_patterns[] = {
            "\xe6\xaf\x8f\xe6\xac\xa1\xe4\xbd\xbf\xe7\x94\xa8\xe6\xb8\x9b\xe5\xb0\x91 #% \xe5\x85\x85\xe8\x83\xbd",
            "#% \xe6\x9b\xb4\xe5\xb0\x91\xe6\x8c\x81\xe7\xba\x8c\xe6\x99\x82\xe9\x96\x93",
            "\xe5\xa2\x9e\xe5\x8a\xa0 #% \xe5\x85\xa8\xe5\x9f\x9f\xe6\x9a\xb4\xe6\x93\x8a\xe7\x8e\x87",
            nullptr
        };
        const char *test_names[] = {
            "reduced_charges_per_use",
            "less_duration",
            "increased_global_crit",
        };
        for (int i = 0; test_patterns[i]; i++) {
            auto it = s_reverse_pattern.find(test_patterns[i]);
            if (it != s_reverse_pattern.end()) {
                snprintf(msg, sizeof(msg), "[pob-proxy] DIAG: pattern '%s' FOUND -> '%.*s'\n",
                         test_names[i], (int)(it->second.text.size() > 80 ? 80 : it->second.text.size()),
                         it->second.text.c_str());
            } else {
                snprintf(msg, sizeof(msg), "[pob-proxy] DIAG: pattern '%s' NOT FOUND\n", test_names[i]);
            }
            OutputDebugStringA(msg);
        }
        /* Check exact match for Immunity to Freeze */
        const char *imm_cn = "\xe6\x95\x88\xe6\x9e\x9c\xe6\x8c\x81\xe7\xba\x8c\xe6\x99\x82\xe9\x96\x93\xef\xbc\x8c\xe5\x85\x8d\xe7\x96\xab\xe5\x86\xb0\xe7\xb7\xa9\xe5\x92\x8c\xe5\x86\xb0\xe5\x87\x8d";
        auto it2 = s_reverse.find(imm_cn);
        if (it2 != s_reverse.end()) {
            snprintf(msg, sizeof(msg), "[pob-proxy] DIAG: exact 'immunity_freeze_chill' FOUND -> '%.*s'\n",
                     (int)(it2->second.size() > 80 ? 80 : it2->second.size()), it2->second.c_str());
        } else {
            snprintf(msg, sizeof(msg), "[pob-proxy] DIAG: exact 'immunity_freeze_chill' NOT FOUND\n");
        }
        OutputDebugStringA(msg);
    }

}

void translation_shutdown(void) {
    translation_wait_ready();
    s_translations.clear();
    s_reverse.clear();
    s_reverse_pattern.clear();
    s_reverse_bases.clear();
    s_base_items_en.clear();
    s_forward_pattern.clear();
    s_forward_pattern_cased.clear();
    s_forward_pattern_case_of.clear();
    s_term_glossary.clear();
    s_lookup_cache.clear();
    s_item_title_cache.clear();
    /* Drop the source dictionaries too, and point the active handles back at
     * the defaults — a reload must not leave them dangling into freed maps. */
    s_active_source = nullptr;
    s_active_source_patterns = nullptr;
    s_active_source_name.clear();
    s_lookup_cache_active = &s_lookup_cache;
    s_item_title_cache_active = &s_item_title_cache;
    s_sources.clear();
    s_source_patterns.clear();
    s_source_cache.clear();
    s_item_title_source_cache.clear();
    s_id_dicts.clear();
    s_id_templates.clear();
    s_stat_index.clear();
    s_stat_named.clear();
    s_stat_files.clear();
    s_stat_file_ids.clear();
    s_stat_parents.clear();
    s_stat_wrappers.clear();
    s_stat_cache.clear();
    s_stat_colored_cache.clear();
    s_stat_row_cache.clear();
    s_group_stat_heads.clear();
    s_group_text_heads.clear();
    s_slash_group_cache.clear();
    s_source_lookup = false;
    s_rules = &kGameRulesPoe1;
    s_game_from_env = false;
    s_meta_source.clear();
    s_locale.clear();
    /* Clear JSON-mode dynamic data */
    s_item_headers_vec.clear();
    s_rarity_values_vec.clear();
    s_item_classes_vec.clear();
    s_influence_tags_vec.clear();
    s_status_lines_vec.clear();
    s_skip_patterns_vec.clear();
    s_mod_suffixes_vec.clear();
    s_mod_annotations_vec.clear();
    s_composite_prefixes_vec.clear();
    s_affix_names.clear();
    s_header_values.clear();
    s_header_words.clear();
    s_unscalable_markers.clear();
    s_reverse_pattern_compact.clear();
    s_reverse_pattern_compact_origin.clear();
    s_reverse_pattern_compact_ambiguous.clear();
    s_reverse_pattern_compact_src.clear();
    s_header_names.clear();
    s_reverse_origin.clear();
    s_reverse_pattern_origin.clear();
    s_synonym_rules.clear();
    s_strip_punctuation.clear();
    s_strip_space_around.clear();
    s_strip_measure_words.clear();
    s_whitelist_terms.clear();
    s_glossary_blacklist.clear();
    s_display_exclude.clear();
    s_exact_only.clear();
    s_exact_only_keys.clear();
    s_initialized = false;
    OutputDebugStringA("[pob-proxy] Translation manager shutdown\n");
}

void translation_reload(void) {
    translation_shutdown();
    translation_init();
    OutputDebugStringA("[pob-proxy] Translation reloaded (F3)\n");
}

const char *translation_game(void) {
    translation_wait_ready();
    return s_rules->game;
}

const char *translation_meta_source(void) {
    translation_wait_ready();
    return s_meta_source.c_str();
}

/* ========== Dev helper: log untranslated strings ==========
**
** Writes every unique untranslated string to <exe dir>\translate_misses.log (truncated on each
** run). Disable with environment POB_TR_LOG=0.
**   MISS|<text>  forward lookup failed (English shown as-is in the UI)
**   REV |<text>  paste line containing CJK that could not be reversed
*/
static void log_translation_miss(const char *tag, const std::string &text) {
    static FILE *s_fp = nullptr;
    static bool s_checked = false;
    static std::unordered_set<std::string> s_seen;

    if (!s_checked) {
        s_checked = true;
        char envbuf[16];
        const char *env = win_env("POB_TR_LOG", envbuf, sizeof(envbuf));
        if (!(env && env[0] == '0')) {
            std::string path = get_exe_directory() + "translate_misses.log";
            s_fp = fopen(path.c_str(), "wb"); /* truncate: latest session only */
            if (s_fp) {
                fprintf(s_fp, "# untranslated strings (locale=%s) - unique per session, POB_TR_LOG=0 to disable\n",
                        s_locale.c_str());
                fflush(s_fp);
            }
        }
    }
    if (!s_fp || text.size() > 400) return;
    if (!s_seen.insert(text).second) return; /* already logged this session */
    fprintf(s_fp, "%s|%s\n", tag, text.c_str());
    /* Flush periodically rather than per line: a text-heavy view can log hundreds of misses
     * in one frame, and per-line fflush means blocking disk writes on the render thread.
     * The editor reads this log after POB exits (the CRT flushes the remainder). */
    static int s_since_flush = 0;
    if (++s_since_flush >= 64) { fflush(s_fp); s_since_flush = 0; }
}

/* Remove SimpleGraphic colour escapes (^7 / ^xRRGGBB) from a string. */
static std::string strip_color_codes(const std::string &s) {
    std::string plain;
    plain.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '^' && i + 1 < s.size()) {
            if ((s[i+1] == 'x' || s[i+1] == 'X') && i + 7 < s.size()) { i += 7; continue; }
            if (s[i+1] >= '0' && s[i+1] <= '9') { i += 1; continue; }
        }
        plain += s[i];
    }
    return plain;
}

/* Length of the SimpleGraphic colour escape starting at `i`, or 0. Deliberately as permissive
** about "^x" as strip_color_codes above (no hex validation): the two must agree on what an
** escape is, or the segment and strip paths cut the same string differently. */
static int colour_escape_len(const std::string &s, size_t i) {
    if (i >= s.size() || s[i] != '^' || i + 1 >= s.size()) return 0;
    const char n = s[i + 1];
    if ((n == 'x' || n == 'X') && i + 7 < s.size()) return 8;
    if (n >= '0' && n <= '9') return 2;
    return 0;
}

/* One run of text plus the colour escape(s) that immediately precede it. */
struct ColourSegment {
    std::string colour; /* escapes, verbatim; empty for a run that starts the string */
    std::string text;   /* everything up to the next escape */
};

/* Split "A^x808080 (B)" into {"", "A"}, {"^x808080", " (B)"}. Consecutive
** escapes stay glued to the run they introduce, so re-joining colour+text in
** order reproduces the input byte for byte. */
static std::vector<ColourSegment> split_colour_segments(const std::string &s) {
    std::vector<ColourSegment> out;
    ColourSegment cur;
    size_t i = 0;
    while (i < s.size()) {
        const int esc = colour_escape_len(s, i);
        if (esc > 0) {
            /* An escape closes the current run, unless no text was collected yet (leading
            ** escapes belong to the run about to start). */
            if (!cur.text.empty()) { out.push_back(cur); cur = ColourSegment(); }
            cur.colour.append(s, i, esc);
            i += esc;
            continue;
        }
        cur.text += s[i++];
    }
    if (!cur.colour.empty() || !cur.text.empty()) out.push_back(cur);
    return out;
}

/* True when every ASCII letter in `s` belongs to a meta.json whitelist term
** ("DPS", "PoB", ...), i.e. the run is meant to stay in English rather than
** being an untranslated gap. Same intent as the glossary's coverage rule. */
static bool covered_by_whitelist(const std::string &s) {
    if (s.empty()) return false;
    std::vector<bool> covered(s.size(), false);
    for (const std::string &wl : s_whitelist_terms) {
        if (wl.empty()) continue;
        size_t from = 0;
        while (true) {
            const size_t p = s.find(wl, from);
            if (p == std::string::npos) break;
            from = p + 1;
            /* Whole-word only, as the glossary: the list holds single-letter gem codes ("B", "R"),
            ** so otherwise "Bag" would count as whitelisted and never be translated. */
            const size_t end = p + wl.size();
            if (p > 0 && is_ascii_alpha(s[p - 1])) continue;
            if (end < s.size() && is_ascii_alpha(s[end])) continue;
            for (size_t i = p; i < end; i++) covered[i] = true;
        }
    }
    for (size_t i = 0; i < s.size(); i++) {
        if (is_ascii_alpha(s[i]) && !covered[i]) return false;
    }
    return true;
}

/* A forward miss is only dictionary-actionable when it actually carries an
** English word: skip numbers, punctuation, colour codes and CJK passthrough. */
static bool forward_miss_worth_logging(const std::string &input) {
    std::string plain = strip_color_codes(input);
    if (plain.size() < 3) return false;
    /* Skip PoE account names like "SomeName#1234" (user data, not translatable) */
    {
        size_t hash = plain.find('#');
        if (hash != std::string::npos && hash > 0 && hash + 1 < plain.size()) {
            bool acct = true;
            for (size_t i = 0; i < hash && acct; i++) {
                char c = plain[i];
                if (!is_ascii_alpha(c) && !(c >= '0' && c <= '9') && c != '_') acct = false;
            }
            for (size_t i = hash + 1; i < plain.size() && acct; i++) {
                if (!(plain[i] >= '0' && plain[i] <= '9')) acct = false;
            }
            if (acct) return false;
        }
    }
    int run = 0;
    bool word = false;
    for (unsigned char c : plain) {
        if (c >= 0x80) return false; /* contains CJK: already translated or user text */
        if (is_ascii_alpha((char)c)) { if (++run >= 3) word = true; }
        else run = 0;
    }
    return word;
}

/* Translate a generated item title word by word.
**
** Rare / cluster-jewel titles are a word-pool pair plus the base type (Item.lua:1572), never a
** dictionary key. The glossary refuses keys under four characters (is_glossary_candidate, to keep
** "and"/"for" out of prose); relaxing that HERE is safe because the only caller first proves the
** string ends in a real base type. All-or-nothing: half-translated is worse than English.
*/
static bool translate_title_words(const std::string &title, std::string &out) {
    if (title.empty() || title.find(' ') == std::string::npos) return false;
    std::string result;
    size_t start = 0;
    for (;;) {
        size_t sp = title.find(' ', start);
        std::string word = title.substr(start, sp == std::string::npos ? std::string::npos : sp - start);
        if (word.empty()) return false;
        /* Same two exact layers translation_lookup consults first, same order (a source
        ** dictionary must still win). NOT a recursive translation_lookup: the pattern and glossary
        ** paths are for sentences and must not fire on a bare word. */
        const std::string *zh = nullptr;
        if (s_active_source) {
            auto sit = s_active_source->find(word);
            if (sit != s_active_source->end()) zh = &sit->second;
        }
        if (!zh) {
            auto tw = s_sources.find("title_words");
            if (tw != s_sources.end()) {
                auto wit = tw->second.find(word);
                if (wit != tw->second.end()) zh = &wit->second;
            }
        }
        if (!zh) {
            auto it = s_translations.find(word);
            if (it == s_translations.end()) return false;
            zh = &it->second;
        }
        if (!result.empty()) result += ' ';
        result += *zh;
        if (sp == std::string::npos) break;
        start = sp + 1;
    }
    out = std::move(result);
    return true;
}

/* Unfilled-placeholder guard. A result carrying "{N}" / "{}" / "{N:fmt}" that the English never
** had is a template whose value was never filled (POB sent half of a multi-line stat, or a
** dictionary value kept GGG's {0} while its key was written with a real number). That is worse
** than showing the English, so every step treats it as a miss. An English that itself has a
** '{' (stats.json keys like "{0} (action speed modifier)") is exempt. */
static bool leaks_placeholder(const std::string &out, const std::string &in, bool check_hash = true) {
    /* A '#' is fill_plan's "no value for this slot": a stray one the English did not have is an
    ** incompletely filled answer, the same failure as a raw {0}. Only a FILLED answer can be
    ** incomplete, so callers handing over a dictionary entry as written (exact steps and
    ** composites of such entries) pass check_hash = false: a translator's own '#' is text. */
    if (check_hash && in.find('#') == std::string::npos && out.find('#') != std::string::npos) return true;
    if (in.find('{') != std::string::npos) return false;
    for (size_t i = 0; i + 1 < out.size(); i++)
        if (out[i] == '{' && (out[i + 1] == '}' || (out[i + 1] >= '0' && out[i + 1] <= '9')))
            return true;
    return false;
}

/* Real newline <-> the literal "\n" GGG multi-line keys are stored with. */
static std::string newlines_to_literal(const std::string &s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\r') continue;
        if (c == '\n') r += "\\n";
        else r += c;
    }
    return r;
}
static std::string literal_to_newlines(const std::string &s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { r += '\n'; i++; continue; }
        r += s[i];
    }
    return r;
}

/* "/"-joined multi-line description: POB's crafting drop-downs join a mod's lines with "/"
** (ItemsTab.lua), so a two-line GGPK description arrives as "<line 1>/<line 2>" and piecewise
** translation yields "<whole sentence>/". When the pieces are line-group catalog descriptions
** (ggpk_groups.json), translate each whole through the stat lookup (ctx from
** translation_set_slash_context: "item" / "passive") and join GGPK's Chinese lines with the
** row's separator again. A row may mix groups and pieces in no group (looked up whole); it is
** answered only when every piece has a translation and at least one group was found. Runs
** before the exact map; own cache ("" = not such a string), since exact hits are never cached
** and DrawString asks every frame. nullptr = not a catalog description (or no answer). */
static const char *slash_group_lookup(const std::string &input) {
    if (!s_source_lookup || s_group_stat_heads.empty() || input.find('/') == std::string::npos ||
        input.find('\n') != std::string::npos)
        return nullptr;
    const std::string ctx = s_slash_ctx.empty() ? std::string("item") : s_slash_ctx;
    std::string ck = ctx;
    ck += '\x1f';
    ck += input;
    auto cit = s_slash_group_cache.find(ck);
    if (cit != s_slash_group_cache.end()) return cit->second.empty() ? nullptr : cit->second.c_str();
    std::string result;
    /* pieces trimmed; the separator as written around the first '/' ("/" in
    ** the crafting rows, " / " in the mastery popup) joins the Chinese lines */
    std::vector<std::string> parts;
    std::string sep_text;
    size_t start = 0;
    while (true) {
        size_t sep = input.find('/', start);
        std::string piece = input.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        const size_t b = piece.find_first_not_of(' '), e = piece.find_last_not_of(' ');
        parts.push_back(b == std::string::npos ? std::string() : piece.substr(b, e - b + 1));
        if (sep == std::string::npos) break;
        if (sep_text.empty()) {
            size_t l = sep, r = sep + 1;
            while (l > start && input[l - 1] == ' ') l--;
            while (r < input.size() && input[r] == ' ') r++;
            sep_text = input.substr(l, r - l);
        }
        start = sep + 1;
    }
    std::vector<std::string> gout;
    std::vector<char> ghit;
    if (parts.size() >= 2 && group_lines_core(parts, ctx.c_str(), gout, ghit) >= 1) {
        std::vector<std::string> zh_lines;
        bool all = true;
        for (size_t i = 0; i < parts.size() && all; i++) {
            std::string z;
            if (ghit[i]) {
                z = gout[i];
            } else {
                /* a piece in no group: its own whole lookup (it has no '/', so
                ** this never comes back here); copied before the next lookup */
                const char *r = parts[i].empty() ? nullptr : translation_lookup(parts[i].c_str());
                if (!r || parts[i] == r) { all = false; break; }
                z = r;
            }
            if (z.empty()) continue;   /* a line whose text went into the line before */
            for (std::string &zl : group_split_lines(z)) zh_lines.push_back(std::move(zl));
        }
        if (all) {
            for (const std::string &zl : zh_lines) {
                if (!result.empty()) result += sep_text;
                result += zl;
            }
            if (leaks_placeholder(result, input)) result.clear();
        }
    }
    if (s_slash_group_cache.size() >= kSlashCacheMax) s_slash_group_cache.clear();
    auto ins = s_slash_group_cache.emplace(std::move(ck), std::move(result)).first;
    return ins->second.empty() ? nullptr : ins->second.c_str();
}

/* POB's own trailing decorations on a mod line:
**   " (Prefix)" / " (Suffix)"            crafting / custom-modifier rows (ItemsTab)
**   " ^8(Prefix)" / " ^8(Suffix)"        bench-crafted rows
**   " ^8(Not supported in PoB yet)"      main.notSupportedTooltipText, any unsupported line
** True when `s` ends in one of them after its separator (spaces and/or one colour escape; a
** bare "Word(Prefix)" is not POB's). body_end = where the separator starts; word = the
** parenthesised text. */
static bool split_pob_tail(const std::string &s, size_t &body_end, std::string &word) {
    static const char *const kWords[] = { "Prefix", "Suffix", "Not supported in PoB yet" };
    if (s.size() < 4 || s.back() != ')') return false;
    const size_t lp = s.rfind('(');
    if (lp == std::string::npos || lp == 0) return false;
    const std::string inner = s.substr(lp + 1, s.size() - lp - 2);
    bool known = false;
    for (const char *w : kWords) known = known || inner == w;
    if (!known) return false;
    size_t st = lp;
    if (st >= 2 && s[st - 2] == '^' && s[st - 1] >= '0' && s[st - 1] <= '9') {
        st -= 2;
    } else if (st >= 8 && s[st - 8] == '^' && s[st - 7] == 'x') {
        bool hex = true;
        for (size_t i = st - 6; i < st; i++) hex = hex && isxdigit((unsigned char)s[i]);
        if (hex) st -= 8;
    }
    while (st > 0 && s[st - 1] == ' ') st--;
    if (st == 0 || st == lp) return false;
    body_end = st;
    word = inner;
    return true;
}

/* Step -1 of translation_lookup: a line carrying one of POB's trailing decorations is the mod
** line + the decoration, never a sentence of its own. Priority (GGPK wording first):
**   1. (Prefix) / (Suffix) only: the body through the stat lookup (ctx "item"; leading colour kept)
**   2. the body through the whole pipeline below -- the same answer the undecorated line gets.
**      The "not supported" note goes straight here: it ends lines of every context and what
**      reaches the engine with it has an unknown ctx.
**   3. no translation for the body at all: nullptr, and the caller goes on with the WHOLE string
**      (old flat keys carrying the decoration, the trailing-note step), so nothing turns English.
** A hit for the body always wins over any whole-string key. The decoration goes back as written
** (spaces and colour escape included): "Prefix"/"Suffix" as the dictionary words them, POB's
** "Not supported in PoB yet" stays English. Answers live in the lookup cache. */
/* tail = " (Prefix)" / " ^8(Suffix)" as split_pob_tail cut it, word = the parenthesised text:
** replaced by the dictionary's (active source first, then merged); as written when neither has it. */
static void translate_affix_tail(std::string &tail, const std::string &word) {
    const std::string *zw = nullptr;
    if (s_active_source) {
        auto sit = s_active_source->find(word);
        if (sit != s_active_source->end() && !sit->second.empty()) zw = &sit->second;
    }
    if (!zw) {
        auto it = s_translations.find(word);
        if (it != s_translations.end() && !it->second.empty()) zw = &it->second;
    }
    if (zw) tail.replace(tail.size() - 1 - word.size(), word.size(), *zw);
}

static const char *decorated_tail_lookup(const char *english) {
    const std::string in(english);
    size_t body_end = 0;
    std::string word;
    if (!split_pob_tail(in, body_end, word)) return nullptr;
    auto cit = s_lookup_cache_active->find(in);
    if (cit != s_lookup_cache_active->end()) return cit->second.empty() ? nullptr : cit->second.c_str();
    const std::string body = in.substr(0, body_end);
    std::string tail = in.substr(body_end);
    std::string zb;
    const bool affix_tag = word == "Prefix" || word == "Suffix";
    if (affix_tag) {
        if (const char *z = translation_lookup_stat_ids_colored(body.c_str(), "item", nullptr)) zb = z;
    }
    if (zb.empty()) {
        const char *z = translation_lookup(body.c_str());
        if (z && body != z) zb = z;   /* copied now: the next lookup may rehash the caches */
    }
    if (zb.empty()) return nullptr;
    if (affix_tag) translate_affix_tail(tail, word);
    auto ins = s_lookup_cache_active->emplace(in, zb + tail).first;
    return ins->second.c_str();
}

/* translation_keep_english (see the header). Registered from the Lua thread,
** read by every lookup; the count lets a lookup skip the lock while nothing is
** registered. Not cleared by translation_shutdown / reload on purpose. */
static std::unordered_set<std::string> s_keep_english;
static std::mutex s_keep_english_mu;
static std::atomic<size_t> s_keep_english_n{0};
static const size_t kKeepEnglishCap = 50000;

void translation_keep_english(const char *s) {
    if (!s || !*s) return;
    std::lock_guard<std::mutex> lock(s_keep_english_mu);
    if (s_keep_english.size() >= kKeepEnglishCap) return;
    if (s_keep_english.insert(s).second) s_keep_english_n.store(s_keep_english.size());
}

/* translation_set_keep_english_scope (see the header): the tooltip being drawn. */
static std::unordered_set<std::string> s_keep_scope;
static std::atomic<size_t> s_keep_scope_n{0};

void translation_set_keep_english_scope(const char *const *s, int n) {
    std::lock_guard<std::mutex> lock(s_keep_english_mu);
    s_keep_scope.clear();
    for (int i = 0; s && i < n; i++)
        if (s[i] && *s[i]) s_keep_scope.insert(s[i]);
    s_keep_scope_n.store(s_keep_scope.size());
}

static bool is_keep_english(const char *s) {
    if (s_keep_english_n.load() == 0 && s_keep_scope_n.load() == 0) return false;
    std::lock_guard<std::mutex> lock(s_keep_english_mu);
    return s_keep_english.count(s) > 0 || s_keep_scope.count(s) > 0;
}

/* translation_set_draw_override (see the header). The Chinese values are interned in
** s_draw_override_pool (node-based: addresses never move) and never freed, so a pointer
** handed out stays valid after the next set replaced the map; the pool is bounded (a value
** past the cap keeps the ordinary lookup). Not touched by translation_reload. */
static std::unordered_map<std::string, const std::string *> s_draw_override;
static std::unordered_set<std::string> s_draw_override_pool;
static std::mutex s_draw_override_mu;
static std::atomic<size_t> s_draw_override_n{0};
static const size_t kDrawOverridePoolCap = 200000;

void translation_set_draw_override(const char *const *keys, const char *const *vals, int n) {
    std::lock_guard<std::mutex> lock(s_draw_override_mu);
    s_draw_override.clear();
    for (int i = 0; keys && vals && i < n; i++) {
        if (!keys[i] || !*keys[i] || !vals[i] || !*vals[i]) continue;
        auto pit = s_draw_override_pool.find(vals[i]);
        if (pit == s_draw_override_pool.end()) {
            if (s_draw_override_pool.size() >= kDrawOverridePoolCap) continue;
            pit = s_draw_override_pool.emplace(vals[i]).first;
        }
        s_draw_override[keys[i]] = &*pit;
    }
    s_draw_override_n.store(s_draw_override.size());
}

static const char *draw_override_find(const char *s) {
    if (s_draw_override_n.load() == 0) return nullptr;
    std::lock_guard<std::mutex> lock(s_draw_override_mu);
    auto it = s_draw_override.find(s);
    return it == s_draw_override.end() ? nullptr : it->second->c_str();
}

const char* translation_lookup(const char *english) {
    if (!english || !s_translation_enabled) return nullptr;
    /* a display string registered to stay English (a unique's flavour text) */
    if (is_keep_english(english)) return nullptr;
    /* a row the control being drawn translated by id (its stat ids, a gem's /
    ** an ascendancy's id): ahead of every map and cache below */
    if (const char *ov = draw_override_find(english)) return ov;
    translation_wait_ready();
    if (s_translations.empty()) return nullptr;

    /* -1. POB's trailing decoration: the body first (see decorated_tail_lookup) */
    if (const char *dz = decorated_tail_lookup(english)) return dz;

    /* One-time deferred diagnostic: check lua-utf8.dll load status.
     * This runs on the first lookup (by which time Lua is fully initialized). */
    static bool s_diag_done = false;
    if (!s_diag_done) {
        s_diag_done = true;
        HMODULE hUtf8 = GetModuleHandleA("lua-utf8");
        if (hUtf8) {
            OutputDebugStringA("[pob-proxy] lua-utf8.dll is loaded (IME input should work)\n");
        } else {
            OutputDebugStringA("[pob-proxy] WARNING: lua-utf8.dll NOT loaded! Chinese IME input will be blocked.\n");
        }
    }

    /* 0. Source dictionary: the file POB said the string came from wins over the merged map. */
    /* An exact entry exists for this very sentence but needs a value it was not given (the
    ** guard below blocked it): the sentence is KNOWN, so the glossary (step 4) must not
    ** stitch it together from halves. Pattern and the other structural steps still run. */
    bool exact_blocked = false;
    if (s_active_source) {
        auto sit = s_active_source->find(english);
        if (sit != s_active_source->end()) {
            if (!leaks_placeholder(sit->second, english, /*check_hash=*/false)) return sit->second.c_str();
            exact_blocked = true;
        }
    }

    /* 0.5 Source pattern: the marked file's own value template beats the merged exact map too
     *     (another file's literal line must not override what the sourced file means). Same entry
     *     condition and fill plan as step 3; results go to s_lookup_cache_active, consulted
     *     first (it only holds strings that missed the exact layers). */
    if (s_active_source_patterns && !s_active_source_patterns->empty()) {
        std::string sinput(english);
        std::string spattern = to_ascii_lower(digits_to_hash(normalize_placeholders(sinput)));
        if (spattern != to_ascii_lower(sinput) || sinput.find('#') != std::string::npos) {
            auto cit = s_lookup_cache_active->find(sinput);
            if (cit != s_lookup_cache_active->end()) {
                return cit->second.empty() ? nullptr : cit->second.c_str();
            }
            auto pit = s_active_source_patterns->find(spattern);
            if (pit != s_active_source_patterns->end()) {
                std::string result = fill_plan(pit->second, hash_slot_values(sinput, /*normalize_ph=*/true));
                if (!leaks_placeholder(result, sinput)) {
                    auto [iter, _] = s_lookup_cache_active->emplace(std::move(sinput), std::move(result));
                    return iter->second.c_str();
                }
            }
        }
    }

    /* 0.7 A "/"-joined multi-line description is translated whole, ahead of
     *     the exact map (see slash_group_lookup). */
    if (const char *sg = slash_group_lookup(english)) return sg;

    /* 1. Exact match (fast path, no cache needed — already O(1)) */
    auto it = s_translations.find(english);
    if (it != s_translations.end()) {
        if (!leaks_placeholder(it->second, english, /*check_hash=*/false)) return it->second.c_str();
        exact_blocked = true;
    }

    /* 2. Check lookup cache (avoids repeated pattern/glossary scans) */
    {
        auto cit = s_lookup_cache_active->find(english);
        if (cit != s_lookup_cache_active->end()) {
            return cit->second.empty() ? nullptr : cit->second.c_str();
        }
    }

    /* 3. Pattern match: normalize digits and placeholders, then lookup (case-insensitive) */
    std::string input(english);
    /* Every answer-producing step goes through here: a leaking answer (leaks_placeholder) is a
    ** miss for that step -- nothing cached, the next step gets its turn.
    ** check_hash: true for a filled pattern (a stray '#' = unfilled slot); false for an exact
    ** entry as written and for composites of already-checked answers. */
    auto accept = [&input](std::string &&result, bool check_hash) -> const char * {
        if (leaks_placeholder(result, input, check_hash)) return nullptr;
        auto [iter, _] = s_lookup_cache_active->emplace(input, std::move(result));
        return iter->second.c_str();
    };
    std::string pattern = to_ascii_lower(digits_to_hash(normalize_placeholders(input)));
    /* The second clause is not redundant. POB's crafting UI writes a mod's rollable value as a
    ** literal '#' ("#% increased Armour during Effect"), which is ALREADY the hashed form, so
    ** hashing changes nothing and the first clause is false -- the pattern map would be skipped. */
    if ((pattern != to_ascii_lower(input) || input.find('#') != std::string::npos)
        && !s_forward_pattern.empty()) {
        const FillPlan *plan = find_forward_pattern(digits_to_hash(normalize_placeholders(input)));
        if (plan) {
            /* 輸入的第 i 個 '#' 一定對得上樣板的第 i 個 '#' —— 兩者雜湊後是同一
            ** 個字串,這正是它命中的原因。計畫就是照這個索引寫的。 */
            std::string result = fill_plan(*plan, hash_slot_values(input, /*normalize_ph=*/true));
            /* Diagnostic: log pattern hits (first 20 only) */
            static int s_hit_count = 0;
            if (s_hit_count < 20) {
                char msg[512];
                snprintf(msg, sizeof(msg),
                    "[pob-proxy] PATTERN HIT: \"%.120s\" -> \"%.120s\"\n",
                    english, result.c_str());
                OutputDebugStringA(msg);
                s_hit_count++;
            }
            if (const char *r = accept(std::move(result), /*check_hash=*/true)) return r;
        } else {
            /* Diagnostic: log pattern misses (first 50 only) */
            static int s_miss_count = 0;
            if (s_miss_count < 50) {
                char msg[512];
                snprintf(msg, sizeof(msg),
                    "[pob-proxy] PATTERN MISS: len=%d input=\"%.120s\" pat=\"%.120s\"\n",
                    (int)pattern.size(), english, pattern.c_str());
                OutputDebugStringA(msg);
                s_miss_count++;
            }
        }
    }

    /* 3.3 Whole multi-line stat: GGG stores a two-line description as ONE key with a literal
     *     "\n", and POB may send it with a real newline. Try that key (exact, then pattern)
     *     before 3.35 splits the block -- a half line can match some other, shorter template.
     *     The answer's literal "\n" comes back as a real newline. */
    if (input.find('\n') != std::string::npos) {
        const std::string esc = newlines_to_literal(input);
        auto eit = s_translations.find(esc);
        if (eit != s_translations.end()) {
            if (const char *r = accept(literal_to_newlines(eit->second), /*check_hash=*/false)) return r;
            exact_blocked = true;
        }
        const std::string epat = to_ascii_lower(digits_to_hash(normalize_placeholders(esc)));
        if (epat != to_ascii_lower(esc) || esc.find('#') != std::string::npos) {
            if (const FillPlan *plan = find_forward_pattern(digits_to_hash(normalize_placeholders(esc)))) {
                std::string result = fill_plan(*plan, hash_slot_values(esc, /*normalize_ph=*/true));
                if (const char *r = accept(literal_to_newlines(result), /*check_hash=*/true)) return r;
            }
        }
    }

    /* 3.35 Multi-line blocks: POB's LabelControls are whole [[...]] literals and EditControl's
     *     unfocused view is every line glued into one string (ConfigTab.lua:85) -- ONE DrawString
     *     call, while the dictionary keys are per line.
     *     Each line is translated INDEPENDENTLY through the full pipeline: translated lines are
     *     replaced, the rest kept verbatim. All-or-nothing holds WITHIN a line, not across lines
     *     (the custom-mods box mixes recognised mods with the player's own notes; demanding the
     *     whole block would drag every mod back to English).
     *     Runs BEFORE the colour-segment step so 3.4 only sees single lines (a line holds no
     *     '\n', so no recursion back here). A miss starting with the "- " bullet is retried
     *     without it (zh-rCN keys carry no bullet). */
    if (input.find('\n') != std::string::npos) {
        std::vector<std::string> lines;
        size_t start = 0;
        while (true) {
            size_t sep = input.find('\n', start);
            if (sep == std::string::npos) { lines.push_back(input.substr(start)); break; }
            lines.push_back(input.substr(start, sep - start));
            start = sep + 1;
        }
        bool translated_any = false;
        std::string joined;
        joined.reserve(input.size() + 64);
        for (size_t li = 0; li < lines.size(); li++) {
            const std::string &line = lines[li];
            std::string body = line;
            if (!body.empty() && body.back() == '\r') body.pop_back();
            bool has_alpha = false;
            for (char c : body) if (is_ascii_alpha(c)) { has_alpha = true; break; }
            std::string out_line = line;
            if (has_alpha && !covered_by_whitelist(body)) {
                const char *zh = translation_lookup(body.c_str());
                std::string zh_copy;
                if (zh && body != zh) {
                    zh_copy = zh;
                } else if (!zh && body.compare(0, 2, "- ") == 0) {
                    if (const char *bz = translation_lookup(body.c_str() + 2)) {
                        if (body.compare(2, std::string::npos, bz) != 0) {
                            zh_copy = "- ";
                            zh_copy += bz;
                        }
                    }
                }
                if (!zh_copy.empty()) {
                    translated_any = true;
                    out_line = std::move(zh_copy);
                    if (!line.empty() && line.back() == '\r') out_line += '\r';
                }
            }
            if (li) joined += '\n';
            joined += out_line;
        }
        /* No line translated: fall through untouched, so the whole-block
        ** behaviour for colour/glossary inputs is exactly what it was. */
        if (translated_any) {
            if (const char *r_ = accept(std::move(joined), /*check_hash=*/false)) return r_;
        }
    }

    /* 3.4 Colour segments: POB glues colour escapes straight onto words ("^x7070FFKinetic Blast
     *     ^720/20", "111,337^x808080 (Guard)"), which defeats every matcher above and the glossary
     *     word boundaries. Translate each coloured run on its own and put the runs back; stripping
     *     the codes would return a colourless string (the caller can re-attach only the LEADING
     *     escape).
     *     A segment carries no colour ESCAPE, so the recursive lookup cannot come back here. The
     *     entry check must be for a real escape, not the '^' character: a bare-caret string (the
     *     Notes tab text has a literal "(^)") re-entering as one segment identical to its input
     *     recursed until stack overflow (0xC00000FD). */
    bool input_has_escape = false;
    for (size_t i = 0; i + 1 < input.size(); i++) {
        if (colour_escape_len(input, i) > 0) { input_has_escape = true; break; }
    }
    if (input_has_escape) {
        std::vector<ColourSegment> segs = split_colour_segments(input);
        std::string rebuilt;
        rebuilt.reserve(input.size() + 32);
        bool translated_any = false;
        bool all_covered = true;
        for (const ColourSegment &seg : segs) {
            /* Split the run into <leading space><core><trailing space>: POB puts the escape before
            ** the space (" (Guard)") and no dictionary key carries the space. Newlines count as
            ** space: EditControl's unfocused view sends each line as its own segment with a
            ** trailing "\n" (ConfigTab.lua:85). */
            const size_t b = seg.text.find_first_not_of(" \t\r\n");
            const size_t e = seg.text.find_last_not_of(" \t\r\n");
            const std::string core = (b == std::string::npos)
                                         ? std::string()
                                         : seg.text.substr(b, e - b + 1);
            bool has_alpha = false;
            for (char c : core) if (is_ascii_alpha(c)) { has_alpha = true; break; }

            /* Nothing to translate (digits, punctuation, Chinese already), or a run the whitelist
            ** keeps in English ("B", "DPS"): covered, emitted exactly as it arrived. */
            if (!has_alpha || covered_by_whitelist(core)) {
                rebuilt += seg.colour;
                rebuilt += seg.text;
                continue;
            }
            const char *zh = translation_lookup(core.c_str());
            if (!zh || core == zh) { all_covered = false; break; }
            std::string zh_copy(zh); /* the next lookup may rehash the cache */
            translated_any = true;
            /* A translation that starts with its own escape is choosing the
            ** colour; emitting the segment's escape first would be dead bytes
            ** at best and would fight the dictionary at worst. */
            if (colour_escape_len(zh_copy, 0) == 0) rebuilt += seg.colour;
            rebuilt.append(seg.text, 0, (b == std::string::npos) ? seg.text.size() : b);
            rebuilt += zh_copy;
            if (b != std::string::npos) rebuilt.append(seg.text, e + 1, std::string::npos);
        }
        if (translated_any && all_covered) {
            if (const char *r_ = accept(std::move(rebuilt), /*check_hash=*/false)) return r_;
        }

        /* Segment translation did not cover the line: fall back to whole-line behaviour (loses
        ** the colours, but a coloured English line is worse than a colourless Chinese one).
        ** Only for a line that carries an English word: with no letters the fallback only
        ** punctuation-normalises ("^7^xFF99220 ^7(0)" came back as "^70（0）"). */
        std::string stripped = strip_color_codes(input);
        bool stripped_has_alpha = false;
        for (char c : stripped) if (is_ascii_alpha(c)) { stripped_has_alpha = true; break; }
        if (stripped_has_alpha && stripped != input && !stripped.empty()) {
            if (const char *r = translation_lookup(stripped.c_str())) {
                std::string copy(r); /* copy before emplace can rehash the cache */
                if (const char *r_ = accept(std::move(copy), /*check_hash=*/false)) return r_;
            }
            /* clean text also missed: fall through with the original input */
        }
    }

    /* 3.45 POB's raw item-text tags ("{enchant}{rune}...", "{variant:2}...", "{range:0.5}...",
     *      "{crafted}...") on a line in the "Edit item text" box or unique database: no
     *      dictionary key has them, so the glossary would take the line apart word by word.
     *      Translate the body alone and put the tags back verbatim: they are POB syntax and the
     *      box is saved straight back into the item, so a translated tag corrupts it. */
    if (input.size() > 2 && input[0] == '{') {
        size_t p = 0;
        while (p < input.size() && input[p] == '{') {
            const size_t close = input.find('}', p + 1);
            if (close == std::string::npos) break;
            p = close + 1;
        }
        /* the body cannot start with '{', so this branch cannot recurse */
        if (p > 0 && p < input.size()) {
            const std::string body = input.substr(p);
            if (const char *body_zh = translation_lookup(body.c_str())) {
                if (body != body_zh) {
                    std::string combined = input.substr(0, p);
                    combined += body_zh;
                    if (const char *r_ = accept(std::move(combined), /*check_hash=*/false)) return r_;
                }
            }
        }
    }

    /* 3.46 A wholly bracketed run "[...]" with no '|' (not POB's "[Tag|Shown]" markup) and no other
     *      bracket: the custom-modifier list draws an essence's mod as "<name>   ^8[32% increased
     *      Movement Speed] (Prefix)" and the run reaches here alone (3.4). The inside is a whole
     *      stat line: translate it through the full pipeline, put it back between the brackets
     *      (the glossary would match a term INSIDE "32%"). No bracket inside, so no recursion. */
    if (input.size() > 2 && input.front() == '[' && input.back() == ']') {
        const std::string inner = input.substr(1, input.size() - 2);
        if (inner.find_first_of("[]|") == std::string::npos) {
            /* A placeholder in brackets is not a stat line: GGG's all-capitals
            ** tags ("[UNUSED]", "[DNT]") and the glossary blacklist's keys stay
            ** English as they are ([UNUSED] 維持英文). */
            bool letters = false, lower = false;
            for (char ch : inner) {
                if (ch >= 'a' && ch <= 'z') lower = true;
                if (is_ascii_alpha(ch)) letters = true;
            }
            bool placeholder = letters && !lower;
            for (const std::string &bad : s_glossary_blacklist)
                if (!placeholder && inner == bad) placeholder = true;
            if (placeholder) {
                s_lookup_cache_active->emplace(input, std::string());
                return nullptr;
            }
            if (const char *inner_zh = translation_lookup(inner.c_str())) {
                if (inner != inner_zh) {
                    std::string combined = "[";
                    combined += inner_zh;
                    combined += "]";
                    if (const char *r_ = accept(std::move(combined), /*check_hash=*/false)) return r_;
                }
            }
        }
    }

    /* 3.5 Trailing parenthetical note: a line ending in " (<note>)" ("(Unused)",
     *     ...) defeats the exact and pattern matches above. Split the note
     *     off, translate the body recursively, translate the note when the
     *     dictionary has it, and stitch the two back together.
     *     POB's own decorations -- (Prefix) / (Suffix) / (Not supported in PoB yet) -- are
     *     not notes here: decorated_tail_lookup (step -1) owns them (this step would split at
     *     the last " (", often one INSIDE the body: "Life (5-6) gained ^8(Not supported...)"). */
    size_t pob_tail_end = 0;
    std::string pob_tail_word;
    if (input.size() > 4 && input.back() == ')' && !split_pob_tail(input, pob_tail_end, pob_tail_word)) {
        size_t open = input.rfind(" (");
        if (open != std::string::npos && open > 0) {
            std::string suffix = input.substr(open + 2, input.size() - open - 3);
            bool suffix_has_alpha = false;
            for (char c : suffix) {
                if (is_ascii_alpha(c)) { suffix_has_alpha = true; break; }
            }
            if (suffix_has_alpha) {
                std::string body = input.substr(0, open);
                if (const char *body_zh = translation_lookup(body.c_str())) {
                    std::string combined(body_zh); /* copy before the next lookup can rehash the cache */
                    /* Ask for the bracketed form FIRST: "(Unused)" is a real key, the bare "Unused"
                    ** is not and would reach the glossary's placeholder affix "[UNUSED]". */
                    const std::string bracketed = "(" + suffix + ")";
                    if (const char *whole_zh = translation_lookup(bracketed.c_str())) {
                        if (bracketed != whole_zh) {
                            combined += " ";
                            combined += whole_zh;
                            if (const char *r_ = accept(std::move(combined), /*check_hash=*/false)) return r_;
                        }
                    }
                    const char *suffix_zh = translation_lookup(suffix.c_str());
                    combined += " (";
                    combined += suffix_zh ? suffix_zh : suffix.c_str();
                    combined += ")";
                    if (const char *r_ = accept(std::move(combined), /*check_hash=*/false)) return r_;
                }
            }
        }
    }

    /* 3.7 Comma-joined lists POB builds at runtime ("Arcane Cloak, Automation, Sigil of Power"):
     *     translate each segment through the full pipeline; join only when EVERY segment
     *     translates (a line is either fully translated or fully English). */
    if (input.find(", ") != std::string::npos) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            size_t sep = input.find(", ", start);
            if (sep == std::string::npos) { parts.push_back(input.substr(start)); break; }
            parts.push_back(input.substr(start, sep - start));
            start = sep + 2;
        }
        /* "<title>, <base type>" names every rare and cluster jewel (Item.lua:1572). Confirming the
        ** LAST segment is a real base type licenses the relaxed per-word fallback for the rest;
        ** that check keeps the relaxation out of prose. */
        const bool item_name = parts.size() >= 2 && s_base_items_en.count(parts.back()) > 0;
        bool all_ok = parts.size() >= 2;
        std::string joined;
        for (size_t i = 0; i < parts.size(); i++) {
            const std::string &p = parts[i];
            if (p.empty()) { all_ok = false; break; }
            std::string seg;
            if (const char *r = translation_lookup(p.c_str())) {
                seg = r; /* copied now: the next lookup may rehash the caches */
            } else if (!(item_name && i + 1 < parts.size() && translate_title_words(p, seg))) {
                all_ok = false;
                break;
            }
            if (!joined.empty()) joined += ", ";
            joined += seg;
        }
        if (all_ok) {
            if (const char *r_ = accept(std::move(joined), /*check_hash=*/false)) return r_;
        }
    }

    /* 4. Term glossary: substring replacement for composite stat strings ("+5 Total Mana
     *    (+10.0%)" → "+5 最大魔力 (+10.0%)"). Only applied when the glossary plus the meta.json
     *    English whitelist covers EVERY letter of the input; partial coverage stays fully English
     *    (a mixed-language line is worse than an untranslated one). */
    if (!exact_blocked && input.size() >= 4 && !s_term_glossary.empty() &&
        input.find("Community Fork") == std::string::npos) {
        struct Span { size_t pos, len; const std::string *to; };
        std::vector<Span> spans;     /* glossary hits, to be replaced */
        std::vector<Span> allowed;   /* whitelist hits, kept as-is */

        auto overlaps_any = [](const std::vector<Span> &v, size_t pos, size_t len) {
            for (auto &s : v) {
                if (pos < s.pos + s.len && s.pos < pos + len) return true;
            }
            return false;
        };
        /* A term is a whole word AND a whole number: one beginning with a digit cannot start
        ** inside a number ("2% increased..." inside "32% ..." left a stray "3"), nor one ending
        ** with a digit stop inside one. */
        auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
        auto boundary_ok = [&input, &is_digit](size_t pos, size_t len) {
            if (pos > 0 && is_ascii_alpha(input[pos - 1])) return false;
            size_t end = pos + len;
            if (end < input.size() && is_ascii_alpha(input[end])) return false;
            /* "1,234": a thousands comma between digits is part of the number */
            if (len > 0 && pos > 0 && is_digit(input[pos]) &&
                (is_digit(input[pos - 1]) || input[pos - 1] == '.' ||
                 (input[pos - 1] == ',' && pos > 1 && is_digit(input[pos - 2]))))
                return false;
            if (len > 0 && end < input.size() && is_digit(input[end - 1]) &&
                (is_digit(input[end]) ||
                 ((input[end] == '.' || input[end] == ',') && end + 1 < input.size() && is_digit(input[end + 1]))))
                return false;
            return true;
        };

        /* Index input positions by lowercased byte: each term probes only the positions of its
         * first char instead of rescanning the string per term (thousands of terms; a full scan
         * froze the render thread for seconds on the About popup's long changelog text). */
        std::vector<size_t> posByChar[256];
        for (size_t i = 0; i < input.size(); i++) {
            unsigned char c = (unsigned char)input[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            posByChar[c].push_back(i);
        }
        auto match_at = [&input](size_t pos, const std::string &needle_lower) {
            if (pos + needle_lower.size() > input.size()) return false;
            for (size_t j = 0; j < needle_lower.size(); j++) {
                char hc = input[pos + j];
                if (hc >= 'A' && hc <= 'Z') hc += 32;
                if (hc != needle_lower[j]) return false;
            }
            return true;
        };

        /* Glossary terms, longest first (s_term_glossary is pre-sorted) */
        for (auto &term : s_term_glossary) {
            if (term.first.empty()) continue;
            const std::vector<size_t> &cand = posByChar[(unsigned char)term.first[0]];
            size_t minPos = 0; /* same-term occurrences must not overlap */
            for (size_t pos : cand) {
                if (pos < minPos) continue;
                if (!match_at(pos, term.first)) continue;
                if (!boundary_ok(pos, term.first.size())) continue;
                if (overlaps_any(spans, pos, term.first.size())) continue;
                spans.push_back({ pos, term.first.size(), &term.second });
                minPos = pos + term.first.size();
            }
        }
        /* Whitelisted tokens (case-sensitive) count as covered but are not replaced */
        for (auto &wl : s_whitelist_terms) {
            size_t from = 0;
            while (true) {
                size_t pos = input.find(wl, from);
                if (pos == std::string::npos) break;
                from = pos + 1;
                if (!boundary_ok(pos, wl.size())) continue;
                if (overlaps_any(spans, pos, wl.size())) continue;
                allowed.push_back({ pos, wl.size(), nullptr });
            }
        }

        if (!spans.empty()) {
            bool covered = true;
            for (size_t i = 0; i < input.size() && covered; i++) {
                if (!is_ascii_alpha(input[i])) continue;
                bool in_span = false;
                for (auto &s : spans) {
                    if (i >= s.pos && i < s.pos + s.len) { in_span = true; break; }
                }
                if (!in_span) {
                    for (auto &s : allowed) {
                        if (i >= s.pos && i < s.pos + s.len) { in_span = true; break; }
                    }
                }
                covered = in_span;
            }
            if (covered) {
                std::sort(spans.begin(), spans.end(),
                    [](const Span &a, const Span &b) { return a.pos < b.pos; });
                std::string result;
                result.reserve(input.size() + 16);
                size_t cur = 0;
                for (auto &s : spans) {
                    result.append(input, cur, s.pos - cur);
                    result += *s.to;
                    cur = s.pos + s.len;
                }
                result.append(input, cur, std::string::npos);
                if (const char *r_ = accept(std::move(result), /*check_hash=*/false)) return r_;
            }
        }
    }

    /* 4.5 Drop-down rows, last resort: POB lays the Timeless jewel / tattoo / runegraft pickers out
     *     as "<name><48 spaces><stat text>" (TreeTab.lua); the part after the spaces is cut off
     *     on screen and only feeds the list's search box (hidden search text stays English).
     *     When nothing above covered the row, translate only the NAME and keep everything from
     *     the spaces on. Only for that shape (a run of 48+ spaces); last on purpose; the name
     *     holds no such run, so no recursion. */
    if (!exact_blocked) {
        const size_t gap = input.find(std::string(48, ' '));
        if (gap != std::string::npos && gap > 0) {
            const std::string name = input.substr(0, gap);
            bool has_alpha = false;
            for (char ch : name) if (is_ascii_alpha(ch)) { has_alpha = true; break; }
            if (has_alpha && !covered_by_whitelist(name)) {
                if (const char *zh = translation_lookup(name.c_str())) {
                    if (name != zh) {
                        std::string row(zh);
                        row.append(input, gap, std::string::npos);
                        if (const char *r_ = accept(std::move(row), /*check_hash=*/false)) return r_;
                    }
                }
            }
        }
    }

    /* Cache negative result to avoid repeated lookups */
    if (forward_miss_worth_logging(input)) {
        log_translation_miss("MISS", input);
    }
    s_lookup_cache_active->emplace(input, std::string());
    return nullptr;
}

const char* translation_lookup_item_title(const char *english) {
    if (!english || !*english || !s_translation_enabled) return nullptr;
    translation_wait_ready();
    if (s_translations.empty()) return nullptr;

    /* Deliberately NOT s_lookup_cache: a relaxed result must never become visible to the general
    ** path (later callers, including prose) -- the leak this separate entry point prevents. */
    std::string input(english);
    /* Per-source like s_lookup_cache: the answer below goes through the active
    ** source, so it must not be served to an unmarked caller (or another source). */
    std::unordered_map<std::string, std::string> *title_cache = s_item_title_cache_active;
    auto cit = title_cache->find(input);
    if (cit != title_cache->end()) {
        return cit->second.empty() ? nullptr : cit->second.c_str();
    }

    std::string result;
    /* Uniques and any title the dictionary knows whole go through the normal
    ** pipeline first, so this changes nothing for them. */
    if (const char *r = translation_lookup(english)) {
        result = r;
    } else if (!translate_title_words(input, result)) {
        result.clear();
    }
    auto [iter, _] = title_cache->emplace(std::move(input), std::move(result));
    return iter->second.empty() ? nullptr : iter->second.c_str();
}

/* Select the source dictionary consulted before the merged map (null / "" = plain merged lookup).
** Returns the PREVIOUS selection so the caller can restore it (nesting-safe). An unknown name
** deliberately means "no source" (an older data pack may lack the file; the app must still render). */
const char* translation_set_source(const char *name) {
    translation_wait_ready();
    static std::string prev_holder;
    /* Copy the argument BEFORE touching prev_holder: restoring a nested scope passes back the
    ** pointer returned last time, which aliases prev_holder. */
    const std::string want = (name && *name) ? std::string(name) : std::string();
    prev_holder = s_active_source_name;

    s_active_source = nullptr;
    s_active_source_patterns = nullptr;
    s_active_source_name.clear();
    s_lookup_cache_active = &s_lookup_cache;
    s_item_title_cache_active = &s_item_title_cache;

    /* Without meta.json "source_lookup" only the legacy gems source exists,
    ** and only its exact entries -- the behaviour before per-source patterns
    ** and the other sources were added. Anything else is "no source". */
    const bool allowed = s_source_lookup || want == kLegacySource;
    if (!want.empty() && allowed) {
        auto it = s_sources.find(want);
        if (it != s_sources.end()) {
            s_active_source = &it->second;
            s_active_source_name = want;
            auto pit = s_source_patterns.find(s_active_source_name);
            if (s_source_lookup && pit != s_source_patterns.end())
                s_active_source_patterns = &pit->second;
            auto cit = s_source_cache.find(s_active_source_name);
            if (cit != s_source_cache.end()) s_lookup_cache_active = &cit->second;
            auto tit = s_item_title_source_cache.find(s_active_source_name);
            if (tit != s_item_title_source_cache.end()) s_item_title_cache_active = &tit->second;
        }
    }
    return prev_holder.empty() ? nullptr : prev_holder.c_str();
}

const char* translation_get_source(void) {
    translation_wait_ready();
    return s_active_source_name.empty() ? nullptr : s_active_source_name.c_str();
}

/* Pointer into s_id_dicts: stable until the next reload (translation_shutdown),
** the same lifetime contract translation_lookup's results already have. */
const char* translation_lookup_by_id(const char *source, const char *id) {
    translation_wait_ready();
    /* F2 off answers nothing, like every other lookup; a locale without
    ** "source_lookup" has no id lookups at all (see s_source_lookup). */
    if (!s_translation_enabled || !s_source_lookup) return nullptr;
    if (!source || !*source || !id || !*id) return nullptr;
    auto sit = s_id_dicts.find(source);
    if (sit == s_id_dicts.end()) return nullptr;
    auto it = sit->second.find(id);
    if (it == sit->second.end()) return nullptr;
    return it->second.c_str();
}

/* translation_lookup_by_id plus an English check: a Timeless jewel / tattoo renames a node but
** keeps its id, so when the id file carries "templates" the answer counts only if its GGPK
** English is the English being shown. Same source_lookup gates via the plain call. */
const char* translation_lookup_by_id_en(const char *source, const char *id, const char *en) {
    const char *zh = translation_lookup_by_id(source, id);
    if (!zh || !en) return zh;
    auto tit = s_id_templates.find(source);
    if (tit == s_id_templates.end()) return zh;   /* older pack: no English to check */
    auto eit = tit->second.find(id);
    if (eit == tit->second.end()) return nullptr;
    return eit->second == normalize_whitespace(en) ? zh : nullptr;
}

/* One stat line through stats_by_id.json (see translation_manager.h).
**
** 1. The line hashes like an English template (markup stripped, numbers -> '#'); that key names
**    the candidate stat entries.
** 2. ctx picks the description files (StatTier): item -> stat_descriptions.txt; passive ->
**    passive_skill_stat_descriptions.txt; gem -> the skill/gem files. passive and gem fall back
**    to stat_descriptions.txt only when their own files have no candidate. A candidate whose
**    English writes out a number the line does not carry is a different stat.
** 2b. With stat id hints (translation_lookup_stat_ids) only candidates sharing a stat id stay,
**    and the sign format decides between "{N:+d}" and "{N}" templates (no sign printed cannot
**    come from "+d"; a '+' prefers "+d").
** 3. Every remaining candidate is filled with the line's values (by {N} index); answers still
**    showing a placeholder are dropped. All the same -> that answer.
** 4. Tie-break: keep candidates whose stat count equals the number of values on the line. All
**    the same -> that answer.
** 4b. With stat id hints: candidates whose written-out numbers equal the line's values exactly
**    win. All the same -> that answer; otherwise ambiguous -> nullptr.
** The pointer is into s_stat_cache: valid until the next reload. */
static std::string stat_lookup_uncached(const std::string &line, const std::vector<int> &chain,
                                        const std::vector<std::string> *hint,
                                        bool *had_cands = nullptr, bool hash_lits = false) {
    if (had_cands) *had_cands = false;
    std::string in = s_rules->stat_keep_input_brackets ? line : strip_brackets(line);
    in.erase(std::remove(in.begin(), in.end(), '\r'), in.end());
    auto it = s_stat_index.find(stat_index_key(in));
    if (it == s_stat_index.end()) return std::string();

    const std::vector<std::string> vals = hash_slot_values(in, /*normalize_ph=*/true);
    /* hash_lits (group / "/" lookups): POB's crafting rows write EVERY number as '#', even
    ** ones the English template writes out ("up to a maximum of 100%" -> "#%"); a written
    ** '#' then matches the template's number and the answer shows that number. */
    auto is_hash = [](const std::string &v) { return v.empty() || v.back() == '#'; };
    auto lits_ok = [&](const StatCand &c) {
        for (const auto &l : c.lits) {
            if ((size_t)l.first >= vals.size()) return false;
            if (hash_lits && is_hash(vals[l.first])) continue;
            if (!same_literal_value(vals[l.first], l.second)) return false;
        }
        return true;
    };
    auto ids_ok = [&](const StatCand &c) {
        if (!hint) return true;
        for (const std::string &id : c.ids)
            if (std::find(hint->begin(), hint->end(), id) != hint->end()) return true;
        return false;
    };
    /* A slot printed by "{N:+d}" always carries a sign; a bare value there
    ** means the line came from a plain "{N}" template. */
    auto sign_ok = [&](const StatCand &c) {
        if (!hint) return true;
        for (size_t i = 0; i < c.plus_slot.size() && i < vals.size(); i++) {
            if (!c.plus_slot[i] || vals[i].empty()) continue;
            const char s0 = vals[i][0];
            if (s0 != '+' && s0 != '-') return false;
        }
        return true;
    };
    std::vector<const StatCand *> cands;
    const std::string in_ckey = stat_case_key(in);
    auto collect = [&](int want_file, bool exact_case) {
        for (const StatCand &c : it->second)
            if (c.file == want_file && (!exact_case || c.ckey == in_ckey) &&
                lits_ok(c) && ids_ok(c) && sign_ok(c))
                cands.push_back(&c);
    };
    /* Walk the chain twice: first only candidates whose English has the
    ** line's exact case (anywhere in the chain), then any case. */
    for (int pass = 0; pass < 2 && cands.empty(); pass++) {
        for (int f : chain) {
            collect(f, pass == 0);
            if (!cands.empty()) break;
        }
    }
    if (cands.empty()) return std::string();

    /* A value printed with '+' prefers the candidates that print it with "+d". */
    if (hint && cands.size() > 1) {
        auto plus_score = [&](const StatCand *c) {
            int n = 0;
            for (size_t i = 0; i < c->plus_slot.size() && i < vals.size(); i++)
                if (c->plus_slot[i] && !vals[i].empty() && vals[i][0] == '+') n++;
            return n;
        };
        int best = 0;
        for (const StatCand *c : cands) { const int s = plus_score(c); if (s > best) best = s; }
        if (best > 0) {
            std::vector<const StatCand *> keep;
            for (const StatCand *c : cands) if (plus_score(c) == best) keep.push_back(c);
            cands.swap(keep);
        }
    }

    std::vector<std::string> outs;
    outs.reserve(cands.size());
    for (const StatCand *c : cands) {
        std::vector<std::string> cv = vals;
        /* With stat ids: a written '#' in a "{N:+d}" slot (POB's tier merge lost the sign)
        ** is written as that template writes a value, "+#". */
        if (hint)
            for (size_t i = 0; i < c->plus_slot.size() && i < cv.size(); i++)
                if (c->plus_slot[i] && cv[i].empty()) cv[i] = "+#";
        if (hash_lits)
            for (const auto &l : c->lits)
                if ((size_t)l.first < cv.size() && is_hash(cv[l.first])) cv[l.first] = l.second;
        outs.push_back(fill_plan(c->plan, cv));
    }
    /* A candidate whose filled answer still shows a placeholder the line did
    ** not have is no answer at all (leaks_placeholder): drop it. */
    {
        size_t k = 0;
        for (size_t i = 0; i < cands.size(); i++) {
            if (leaks_placeholder(outs[i], in)) continue;
            if (k != i) {
                cands[k] = cands[i];
                outs[k] = std::move(outs[i]);
            }
            k++;
        }
        cands.resize(k);
        outs.resize(k);
        if (cands.empty()) return std::string();
    }
    /* The line IS one of these stats, even if they disagree below: a
    ** name-slot template must not answer for it (stat_lookup_full). */
    if (had_cands) *had_cands = true;
    auto all_same = [](const std::vector<std::string> &v) {
        for (size_t i = 1; i < v.size(); i++) if (v[i] != v[0]) return false;
        return !v.empty();
    };
    if (all_same(outs)) return outs[0];

    /* Values that fill a {N} in EVERY candidate. A number some candidate writes
    ** out in its English ("for 4 seconds") is wording there, not a stat value,
    ** so it does not count toward the line's stat count. */
    std::vector<char> literal_somewhere(vals.size(), 0);
    for (const StatCand *c : cands)
        for (const auto &l : c->lits)
            if ((size_t)l.first < vals.size()) literal_somewhere[l.first] = 1;
    int nvals = 0;
    for (size_t i = 0; i < vals.size(); i++) {
        const std::string &v = vals[i];
        if (v.empty() || v.back() == '#') continue;   /* a written '#' is no value */
        if (literal_somewhere[i]) continue;
        nvals++;
    }
    std::vector<std::string> kept;
    for (size_t i = 0; i < cands.size(); i++)
        if (cands[i]->nstats == nvals) kept.push_back(outs[i]);
    if (all_same(kept)) return kept[0];

    /* 4a. Nobody matched that count (one candidate writes the line's only number out, the
    ** others take it as {0}). Count per candidate instead -- the values that fill one of ITS
    ** {N} -- and keep the candidates whose stat count equals their own. */
    if (kept.empty()) {
        for (size_t i = 0; i < cands.size(); i++) {
            std::vector<char> lit(vals.size(), 0);
            for (const auto &l : cands[i]->lits)
                if ((size_t)l.first < vals.size()) lit[l.first] = 1;
            int own = 0;
            for (size_t j = 0; j < vals.size(); j++) {
                const std::string &v = vals[j];
                if (v.empty() || v.back() == '#' || lit[j]) continue;
                own++;
            }
            if (cands[i]->nstats == own) kept.push_back(outs[i]);
        }
        if (all_same(kept)) return kept[0];
    }

    /* 4b (stat id hints only): written-out numbers equal to the line's values,
    ** character for character ("Fires 1 Projectile" written as such beats
    ** "Fires {0:+d} Projectile" fed a 1). */
    if (hint) {
        std::vector<std::string> exact;
        for (size_t i = 0; i < cands.size(); i++) {
            const StatCand *c = cands[i];
            if (c->lits.empty()) continue;
            bool same = true;
            for (const auto &l : c->lits)
                if ((size_t)l.first >= vals.size() || vals[l.first] != l.second) { same = false; break; }
            if (same) exact.push_back(outs[i]);
        }
        if (all_same(exact)) return exact[0];
    }
    return std::string();
}

/* Match `in` against a name-slot template, token by token. A number slot captures exactly one
** number (sign / range included); a name slot any non-empty text without surrounding spaces.
** Backtracks over where each slot ends. caps[N] = the text of {N}. */
static bool stat_match_named(const StatNamedCand &c, const std::string &in, size_t ti, size_t pos,
                             std::unordered_map<int, std::string> &caps) {
    if (ti == c.toks.size()) return pos == in.size();
    const StatNamedTok &t = c.toks[ti];
    if (t.ph < 0) {
        /* Case-insensitive: POB lower-cases some words of these lines
        ** ("Passive Skills in radius of X can be allocated ..."). */
        if (pos + t.lit.size() > in.size()) return false;
        for (size_t k = 0; k < t.lit.size(); k++) {
            char a = in[pos + k], b = t.lit[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) return false;
        }
        return stat_match_named(c, in, ti + 1, pos + t.lit.size(), caps);
    }
    const bool is_name = c.name_src.count(t.ph) > 0;
    for (size_t end = pos + 1; end <= in.size(); end++) {
        const std::string cap = in.substr(pos, end - pos);
        if (is_name) {
            if (cap.front() == ' ' || cap.back() == ' ') continue;
        } else if (digits_to_hash(cap) != "#") {
            continue;
        }
        auto prev = caps.find(t.ph);
        if (prev != caps.end() && prev->second != cap) continue;   /* {N} used twice */
        const bool had = prev != caps.end();
        caps[t.ph] = cap;
        if (stat_match_named(c, in, ti + 1, end, caps)) return true;
        if (!had) caps.erase(t.ph);
    }
    return false;
}

/* A passive / skill name in Chinese, for a name slot whose source is `src`
** (stats_by_id.json "name_slots"):
**   passives            -> passives source exact, then the merged map
**   gems                -> gem_names, gems, then the merged map
**   gem_names*          -> that source, gem_names, then the merged map (never gems.json:
**                          its "…輔助" names double the template's 輔助)
** A name ending in " Support" is looked up without it (the template already says 輔助);
** only the NAME is stripped, the line is never re-hashed with it. Empty when no match. */
static std::string stat_translate_name(const std::string &src, const std::string &en_in) {
    auto exact = [](const char *source, const std::string &key) -> std::string {
        auto sit = s_sources.find(source);
        if (sit == s_sources.end()) return std::string();
        auto it = sit->second.find(key);
        return (it != sit->second.end()) ? it->second : std::string();
    };
    const bool gem_kind = src == "gems" || src.compare(0, 9, "gem_names") == 0;
    std::string en = en_in;
    static const std::string kSupport = " Support";
    if (gem_kind && en.size() > kSupport.size() &&
        en.compare(en.size() - kSupport.size(), kSupport.size(), kSupport) == 0)
        en.erase(en.size() - kSupport.size());
    if (gem_kind) {
        std::string r;
        if (src != "gems" && !(r = exact(src.c_str(), en)).empty()) return r;
        if (!(r = exact("gem_names", en)).empty()) return r;
        if (src == "gems" && !(r = exact("gems", en)).empty()) return r;
    } else {
        const std::string r = exact(src.c_str(), en);
        if (!r.empty()) return r;
    }
    auto it = s_translations.find(en);
    /* An exact entry as written: a '#' in it is text, only {N} blocks. */
    if (it != s_translations.end() && !it->second.empty() &&
        !leaks_placeholder(it->second, en, /*check_hash=*/false))
        return it->second;
    return std::string();
}

/* Name-slot candidates (s_stat_named) for a line the hash index missed. Same
** ctx tiers, same stat id narrowing; a name that has no Chinese drops the
** candidate (no half-English answer); disagreeing answers -> no answer. */
static std::string stat_lookup_named(const std::string &line, const std::vector<int> &chain,
                                     const std::vector<std::string> *hint) {
    if (s_stat_named.empty()) return std::string();
    std::string in = s_rules->stat_keep_input_brackets ? line : strip_brackets(line);
    in.erase(std::remove(in.begin(), in.end(), '\r'), in.end());
    auto run = [&](int want_file) {
        std::vector<std::string> outs;
        for (const StatNamedCand &c : s_stat_named) {
            if (c.file != want_file) continue;
            if (hint) {
                bool shared = false;
                for (const std::string &id : c.ids)
                    if (std::find(hint->begin(), hint->end(), id) != hint->end()) { shared = true; break; }
                if (!shared) continue;
            }
            std::unordered_map<int, std::string> caps;
            if (!stat_match_named(c, in, 0, 0, caps)) continue;
            /* fill the Chinese template */
            std::string out;
            bool ok = true;
            for (size_t i = 0; i < c.zh.size() && ok; i++) {
                if (c.zh[i] == '{') {
                    size_t j = i + 1;
                    while (j < c.zh.size() && c.zh[j] >= '0' && c.zh[j] <= '9') j++;
                    size_t close = c.zh.find('}', j);
                    if (j > i + 1 && close != std::string::npos && (c.zh[j] == '}' || c.zh[j] == ':')) {
                        const int n = atoi(c.zh.c_str() + i + 1);
                        auto cap = caps.find(n);
                        if (cap == caps.end()) { ok = false; break; }
                        auto src = c.name_src.find(n);
                        if (src != c.name_src.end()) {
                            const std::string zh_name = stat_translate_name(src->second, cap->second);
                            if (zh_name.empty()) { ok = false; break; }
                            out += zh_name;
                        } else {
                            out += cap->second;
                        }
                        i = close;
                        continue;
                    }
                }
                out += c.zh[i];
            }
            /* Number slots copy the line's own text, names are dictionary
            ** entries as written: a '#' here is data, only {N} blocks (same
            ** rule as exact entries). */
            if (ok && !leaks_placeholder(out, in, /*check_hash=*/false)) outs.push_back(std::move(out));
        }
        return outs;
    };
    std::vector<std::string> outs;
    for (int f : chain) {
        outs = run(f);
        if (!outs.empty()) break;
    }
    if (outs.empty()) return std::string();
    for (size_t i = 1; i < outs.size(); i++) if (outs[i] != outs[0]) return std::string();
    return outs[0];
}

/* The whole stat lookup: hash index, then name-slot templates, then wrappers
** (stat_wrappers.json: "Added Small Passive Skills grant: {0}" -- the inside
** is looked up as a stat in the same ctx and put into the Chinese wrapper). */
static std::string stat_lookup_full(const std::string &line, const std::vector<int> &chain,
                                    const std::vector<std::string> *hint, int depth = 0,
                                    bool hash_lits = false) {
    bool had_cands = false;
    std::string r = stat_lookup_uncached(line, chain, hint, &had_cands, hash_lits);
    if (!r.empty()) return r;
    /* The hash index knows this exact sentence (it had usable candidates, they
    ** just disagreed): a generic name-slot template or a wrapper must not
    ** answer for it instead. */
    if (had_cands) return std::string();
    r = stat_lookup_named(line, chain, hint);
    if (!r.empty()) return r;
    if (depth > 0) return std::string();   /* one wrapper level */
    for (const StatWrapper &w : s_stat_wrappers) {
        if (line.size() <= w.prefix.size() + w.suffix.size()) continue;
        if (line.compare(0, w.prefix.size(), w.prefix) != 0) continue;
        if (line.compare(line.size() - w.suffix.size(), w.suffix.size(), w.suffix) != 0) continue;
        const std::string inner = line.substr(w.prefix.size(), line.size() - w.prefix.size() - w.suffix.size());
        const std::string zi = stat_lookup_full(inner, chain, hint, depth + 1, hash_lits);
        if (zi.empty()) continue;
        std::string out = w.zh;
        const size_t p = out.find("{0}");
        out.replace(p, 3, zi);
        return out;
    }
    return std::string();
}

/* ctx -> chain of description file ids (see s_stat_files). false = unknown
** ctx. nullptr / "" mean item. Files with no entries are left out. */
static bool stat_ctx_chain(const char *ctx, std::vector<int> &chain) {
    chain.clear();
    auto add = [&](const std::string &name) {
        auto it = s_stat_file_ids.find(name);
        if (it != s_stat_file_ids.end() &&
            std::find(chain.begin(), chain.end(), it->second) == chain.end())
            chain.push_back(it->second);
    };
    auto gem_default = [&]() { for (const char *f : kStatGemDefaultChain) add(f); };
    if (!ctx || !*ctx || strcmp(ctx, "item") == 0) {
        add(kStatFileItem);
    } else if (strcmp(ctx, "passive") == 0) {
        add(kStatFilePassive);
        add(kStatFileItem);
        add(kStatFilePassiveAura);
    } else if (strcmp(ctx, "gem") == 0) {
        gem_default();
    } else if (strncmp(ctx, "gem:", 4) == 0) {
        std::string f = stat_file_basename(ctx + 4);
        if (ctx[4] == '\0' || s_stat_parents.find(f) == s_stat_parents.end()) {
            gem_default();
        } else {
            for (int guard = 0; guard < 32; guard++) {
                add(f);
                auto pit = s_stat_parents.find(f);
                if (pit == s_stat_parents.end()) break;
                f = pit->second;
            }
        }
    } else {
        return false;
    }
    return true;
}

/* Space-separated stat ids -> list; empty when none. */
static std::vector<std::string> stat_split_ids(const char *stat_ids) {
    std::vector<std::string> v;
    if (!stat_ids) return v;
    const char *p = stat_ids;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *b = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (p > b) v.emplace_back(b, p);
    }
    return v;
}

/* Shared by translation_lookup_stat and translation_lookup_stat_ids; hash_lits
** (see stat_lookup_uncached) only from the line-group lookups. */
static const char *stat_lookup_cached(const char *line, const char *ctx, const char *stat_ids,
                                      bool hash_lits = false) {
    translation_wait_ready();
    if (!s_translation_enabled || !s_source_lookup) return nullptr;
    if (!line || !*line || s_stat_index.empty()) return nullptr;
    std::vector<int> chain;
    if (!stat_ctx_chain(ctx, chain)) return nullptr;
    const std::vector<std::string> ids = stat_split_ids(stat_ids);

    std::string ck = hash_lits ? "H\x1f" : "";
    for (int f : chain) { ck += std::to_string(f); ck += ','; }
    ck += '\x1f';
    for (const std::string &id : ids) { ck += id; ck += ' '; }
    ck += '\x1f';
    ck += line;
    auto cit = s_stat_cache.find(ck);
    if (cit == s_stat_cache.end())
        cit = s_stat_cache.emplace(std::move(ck),
                                   stat_lookup_full(line, chain, ids.empty() ? nullptr : &ids, 0,
                                                    hash_lits)).first;
    return cit->second.empty() ? nullptr : cit->second.c_str();
}

const char* translation_lookup_stat_ids(const char *line, const char *ctx, const char *stat_ids) {
    return stat_lookup_cached(line, ctx, stat_ids);
}

const char* translation_lookup_stat(const char *line, const char *ctx) {
    return stat_lookup_cached(line, ctx, nullptr);
}

/* Length of the colour escape at the start of s ("^xRRGGBB" = 8, "^N" = 2),
** 0 when it does not start with one. Same two forms POB's DrawString knows. */
static size_t stat_leading_colour_len(const char *s) {
    if (!s || s[0] != '^') return 0;
    if (s[1] >= '0' && s[1] <= '9') return 2;
    if (s[1] == 'x') {
        for (int i = 2; i < 8; i++)
            if (!isxdigit((unsigned char)s[i])) return 0;   /* also stops at '\0' */
        return 8;
    }
    return 0;
}

const char* translation_lookup_stat_ids_colored(const char *line, const char *ctx, const char *stat_ids) {
    const size_t cl = stat_leading_colour_len(line);
    if (cl == 0) return stat_lookup_cached(line, ctx, stat_ids);
    const char *zh = stat_lookup_cached(line + cl, ctx, stat_ids);
    if (!zh) return nullptr;
    const std::string colour(line, cl);
    std::string out = colour;
    for (const char *p = zh; *p; ++p) {
        out += *p;
        if (*p == '\n') out += colour;
    }
    std::string ck(ctx ? ctx : "");
    ck += '\x1f';
    for (const std::string &id : stat_split_ids(stat_ids)) { ck += id; ck += ' '; }
    ck += '\x1f';
    ck += line;
    /* Keep the first answer: rewriting the slot would free a buffer a caller of the previous
    ** call may still hold (same lifetime contract as translation_lookup_stat). */
    auto ins = s_stat_colored_cache.try_emplace(std::move(ck), std::move(out));
    return ins.first->second.c_str();
}

const char* translation_lookup_stat_colored(const char *line, const char *ctx) {
    return translation_lookup_stat_ids_colored(line, ctx, nullptr);
}

/* The row without its (Prefix) / (Suffix) tail and leading colour: one line through the stat
** lookup with the ids, else its "/"-joined lines (slash_group_lookup's split): pieces a GGPK
** multi-line description covers come whole from the line groups, others from the stat lookup.
** "" unless every piece has an answer. */
static std::string stat_row_body(const std::string &body, const char *ctx, const char *stat_ids) {
    if (body.empty()) return std::string();
    if (const char *z = stat_lookup_cached(body.c_str(), ctx, stat_ids)) return z;
    if (body.find('/') == std::string::npos || body.find('\n') != std::string::npos) return std::string();
    std::vector<std::string> parts;
    std::string sep_text;
    size_t start = 0;
    while (true) {
        size_t sep = body.find('/', start);
        std::string piece = body.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        const size_t b = piece.find_first_not_of(' '), e = piece.find_last_not_of(' ');
        parts.push_back(b == std::string::npos ? std::string() : piece.substr(b, e - b + 1));
        if (sep == std::string::npos) break;
        if (sep_text.empty()) {
            size_t l = sep, r = sep + 1;
            while (l > start && body[l - 1] == ' ') l--;
            while (r < body.size() && body[r] == ' ') r++;
            sep_text = body.substr(l, r - l);
        }
        start = sep + 1;
    }
    std::vector<std::string> gout;
    std::vector<char> ghit;
    group_lines_core(parts, ctx, gout, ghit);   /* fills gout / ghit even with no group */
    std::vector<std::string> zh_lines;
    for (size_t i = 0; i < parts.size(); i++) {
        std::string z;
        if (i < ghit.size() && ghit[i]) {
            z = gout[i];
        } else {
            const char *r = parts[i].empty() ? nullptr : stat_lookup_cached(parts[i].c_str(), ctx, stat_ids);
            if (!r) return std::string();
            z = r;
        }
        if (z.empty()) continue;   /* a line whose text went into the line before */
        for (std::string &zl : group_split_lines(z)) zh_lines.push_back(std::move(zl));
    }
    std::string result;
    for (const std::string &zl : zh_lines) {
        if (!result.empty()) result += sep_text;
        result += zl;
    }
    if (leaks_placeholder(result, body)) result.clear();
    return result;
}

const char* translation_lookup_stat_row(const char *row, const char *ctx, const char *stat_ids) {
    if (!row || !*row || !stat_ids || stat_split_ids(stat_ids).empty()) return nullptr;
    translation_wait_ready();
    if (!s_translation_enabled || !s_source_lookup) return nullptr;
    std::string ck(ctx ? ctx : "");
    ck += '\x1f';
    for (const std::string &id : stat_split_ids(stat_ids)) { ck += id; ck += ' '; }
    ck += '\x1f';
    ck += row;
    auto cit = s_stat_row_cache.find(ck);
    if (cit != s_stat_row_cache.end()) return cit->second.empty() ? nullptr : cit->second.c_str();

    std::string in(row), tail, word;
    size_t body_end = 0;
    if (split_pob_tail(in, body_end, word) && (word == "Prefix" || word == "Suffix")) {
        tail = in.substr(body_end);
        in.resize(body_end);
    } else {
        word.clear();
    }
    const size_t cl = stat_leading_colour_len(in.c_str());
    const std::string colour = in.substr(0, cl);
    std::string zb = stat_row_body(in.substr(cl), ctx, stat_ids);
    std::string out;
    if (!zb.empty()) {
        out = colour;
        for (char c : zb) {
            out += c;
            if (c == '\n') out += colour;
        }
        if (!word.empty()) {
            translate_affix_tail(tail, word);
            out += tail;
        }
    }
    auto ins = s_stat_row_cache.emplace(std::move(ck), std::move(out)).first;
    return ins->second.empty() ? nullptr : ins->second.c_str();
}

/* ---- line groups (ggpk_groups.json) ----
** in: the lines as POB draws them (colour lead and "not supported" note already removed).
** At each line the catalog's candidates starting there (longest first) are tried:
**   text -> every line equals the catalog's English line (whitespace normalised)
**   stat -> every line has the catalog line's key: lines joined with "\n" through the
**           stat lookup (ctx as translation_lookup_stat); a miss is no group
** A group of k English / m Chinese lines: line j gets Chinese line j, lines past m get ""
** (whole sentence on the first line when m == 1); when m > k the last English line carries
** the remaining Chinese lines joined by "\n". A Chinese answer with an empty first line is no
** group. Lines in no group keep hit[i] == 0. Returns the number of groups. */
static int group_lines_core(const std::vector<std::string> &in, const char *ctx,
                            std::vector<std::string> &out, std::vector<char> &hit) {
    const size_t n = in.size();
    out.assign(n, std::string());
    hit.assign(n, 0);
    if (n < 2 || !s_translation_enabled || !s_source_lookup) return 0;
    if (s_group_stat_heads.empty() && s_group_text_heads.empty()) return 0;
    std::vector<std::string> keys(n), norms(n);
    for (size_t i = 0; i < n; i++) {
        keys[i] = group_line_key(in[i], /*is_template=*/false);
        norms[i] = normalize_whitespace(in[i]);
    }
    int groups = 0;
    size_t i = 0;
    while (i < n) {
        size_t best = 0;
        std::vector<std::string> zl;
        auto tit = s_group_text_heads.find(norms[i]);
        if (tit != s_group_text_heads.end()) {
            for (const GroupText &g : tit->second) {
                const size_t k = g.lines.size();
                if (k < 2 || i + k > n) continue;
                bool ok = true;
                for (size_t j = 1; j < k && ok; j++) ok = norms[i + j] == g.lines[j];
                if (!ok) continue;
                zl = group_split_lines(g.zh);
                best = k;
                break;
            }
        }
        auto sit = s_group_stat_heads.find(keys[i]);
        if (sit != s_group_stat_heads.end()) {
            for (const GroupStat &g : sit->second) {
                const size_t k = g.keys.size();
                if (k <= best || i + k > n) continue;
                bool ok = true;
                for (size_t j = 1; j < k && ok; j++) ok = keys[i + j] == g.keys[j];
                if (!ok) continue;
                std::string joined = in[i];
                for (size_t j = 1; j < k; j++) { joined += '\n'; joined += in[i + j]; }
                const char *zh = stat_lookup_cached(joined.c_str(), ctx, nullptr, /*hash_lits=*/true);
                if (!zh) continue;
                zl = group_split_lines(zh);
                best = k;
                break;
            }
        }
        if (best < 2 || zl.empty() || normalize_whitespace(zl[0]).empty()) { i++; continue; }
        for (size_t j = 0; j < best; j++) {
            hit[i + j] = 1;
            if (j + 1 == best && zl.size() > best) {
                std::string rest = zl[j];
                for (size_t r = j + 1; r < zl.size(); r++) { rest += '\n'; rest += zl[r]; }
                out[i + j] = std::move(rest);
            } else {
                out[i + j] = j < zl.size() ? zl[j] : std::string();
            }
        }
        groups++;
        i += best;
    }
    return groups;
}

int translation_group_lines(const char *const *lines, int n, const char *ctx, const char **out) {
    translation_wait_ready();
    static std::vector<std::string> s_out;
    static std::vector<char> s_hit;
    if (!lines || !out || n <= 0) return 0;
    std::vector<std::string> in;
    in.reserve((size_t)n);
    for (int i = 0; i < n; i++) { out[i] = nullptr; in.emplace_back(lines[i] ? lines[i] : ""); }
    const int groups = group_lines_core(in, ctx, s_out, s_hit);
    for (int i = 0; i < n && groups > 0; i++)
        if (s_hit[(size_t)i]) out[i] = s_out[(size_t)i].c_str();
    return groups;
}

const char* translation_set_slash_context(const char *ctx) {
    static std::string prev_holder;
    /* copy the argument first: restoring passes back the pointer this
    ** returned last time, which aliases prev_holder (as translation_set_source) */
    std::string want = (ctx && *ctx) ? std::string(ctx) : std::string();
    if (want == "item") want.clear();
    prev_holder = s_slash_ctx;
    s_slash_ctx = std::move(want);
    return prev_holder.empty() ? nullptr : prev_holder.c_str();
}

const char* translation_debug_sources(void) {
    translation_wait_ready();
    static std::string buf;
    auto names = [](const auto &m) {
        std::vector<std::string> v;
        for (const auto &kv : m) v.push_back(kv.first);
        std::sort(v.begin(), v.end());
        std::string out;
        for (const std::string &n : v) { if (!out.empty()) out += ","; out += n; }
        return out;
    };
    buf = "exact=" + names(s_sources) + ";pattern=" + names(s_source_patterns) +
          ";id=" + names(s_id_dicts);
    return buf.c_str();
}

const char* translation_get_locale(void) {
    translation_wait_ready();
    return s_locale.c_str();
}

const char* translation_get_init_stats(void) {
    /* 不 wait:wait_ready 自己會在 trace 行裡呼叫這個 */
    static char buf[200];
    snprintf(buf, sizeof(buf), "init %.0f ms%s (json parse %.0f, table build %.0f, %d files), main thread waited %.0f ms",
             s_init_stats.total_ms, s_init_stats.async ? " in background" : "",
             s_init_stats.parse_ms, s_init_stats.build_ms, s_init_stats.files, s_init_stats.wait_ms);
    return buf;
}

int translation_get_count(void) {
    translation_wait_ready();
    return (int)s_translations.size();
}

/* ========== Helper: find a mapping in dynamic vector ========== */

/* Returns english string for a matching chinese key, or nullptr */
static const char* find_header_mapping(const std::string &chinese,
                                       const std::vector<HeaderMapping_Dyn> &vec) {
    for (auto &m : vec) {
        if (chinese == m.chinese) return m.english.c_str();
    }
    return nullptr;
}

/* Returns english string for a matching chinese key that is a prefix of key_part */
static const char* find_header_prefix(const std::string &key_part,
                                      std::string &matched_hdr,
                                      const std::vector<HeaderMapping_Dyn> &vec) {
    for (auto &m : vec) {
        if (key_part.size() > m.chinese.size() &&
            key_part.compare(0, m.chinese.size(), m.chinese) == 0 &&
            (key_part[m.chinese.size()] == ' ' || key_part[m.chinese.size()] == '(')) {
            matched_hdr = m.chinese;
            return m.english.c_str();
        }
    }
    return nullptr;
}

/* ========== Helper: try to translate item header "key: value" lines ========== */

static std::string strip_suffix(const std::string &line, std::string &suffix);

static bool try_header_translate(const std::string &line, std::string &out) {
    /* Check for "key: value" pattern */
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;

    std::string key_part = line.substr(0, colon);
    std::string rest = line.substr(colon); /* includes ": value" */

    /* An unknown property key is dropped by the section grammar (reverse_one_line_impl
    ** step 7b), not by a list kept here. */
    const char *eng = find_header_mapping(key_part, s_item_headers_vec);
    if (eng) {
        /* Headers whose VALUE also needs translating (範圍: 大 → Radius: Large).
        ** Scoped per header on purpose — 大/中/小 are far too generic to put in
        ** a global value map. */
        auto hv = s_header_values.find(eng);
        if (hv != s_header_values.end()) {
            std::string value_part = line.substr(colon + 1);
            size_t vs = value_part.find_first_not_of(" \t");
            if (vs != std::string::npos) value_part = value_part.substr(vs);
            size_t ve = value_part.find_last_not_of(" \t\r\n");
            if (ve != std::string::npos) value_part = value_part.substr(0, ve + 1);
            std::string value_suffix;
            const char *val_eng = find_header_mapping(value_part, hv->second);
            if (!val_eng) {
                /* "範圍: 可變的 (augmented)" -- the value, then GGG's modifier note */
                std::string bare = strip_suffix(value_part, value_suffix);
                if (!value_suffix.empty()) val_eng = find_header_mapping(bare, hv->second);
            }
            if (val_eng) {
                out = std::string(eng) + ": " + val_eng + value_suffix;
                return true;
            }
        }
        /* Special handling for "稀有度: 稀有" → "Rarity: Rare" */
        if (strcmp(eng, "Rarity") == 0) {
            std::string value_part = line.substr(colon + 1);
            size_t start = value_part.find_first_not_of(" \t\r\n");
            if (start != std::string::npos) value_part = value_part.substr(start);
            size_t end = value_part.find_last_not_of(" \t\r\n");
            if (end != std::string::npos) value_part = value_part.substr(0, end + 1);
            const char *rarity_eng = find_header_mapping(value_part, s_rarity_values_vec);
            if (rarity_eng) {
                out = std::string("Rarity: ") + rarity_eng;
                return true;
            }
        }
        /* Special handling for "物品種類: 胸甲" → "Item Class: Body Armours" */
        if (strcmp(eng, "Item Class") == 0) {
            std::string value_part = line.substr(colon + 1);
            size_t start = value_part.find_first_not_of(" \t");
            if (start != std::string::npos) value_part = value_part.substr(start);
            size_t end = value_part.find_last_not_of(" \t\r\n");
            if (end != std::string::npos) value_part = value_part.substr(0, end + 1);
            const char *class_eng = find_header_mapping(value_part, s_item_classes_vec);
            if (class_eng) {
                out = std::string("Item Class: ") + class_eng;
                return true;
            }
            /* Log unmatched class with hex dump for debugging */
            {
                char dbg[512];
                int pos = snprintf(dbg, sizeof(dbg),
                    "[pob-proxy] Item Class NOT FOUND (%d bytes) hex:",
                    (int)value_part.size());
                for (size_t b = 0; b < value_part.size() && b < 30 && pos < 450; b++) {
                    pos += snprintf(dbg + pos, sizeof(dbg) - pos,
                        " %02X", (unsigned char)value_part[b]);
                }
                snprintf(dbg + pos, sizeof(dbg) - pos, "\n");
                OutputDebugStringA(dbg);
            }
        }
        /* Generic header: replace key, keep value as-is -- apart from the
        ** words item_metadata.header_words names for this header (需求: 等級 78
        ** -> Requires: Level 78; 127 到 213 -> 127-213 under every header). */
        out = std::string(eng) + rest;
        for (const char *scope : { eng, "*" }) {
            auto hw = s_header_words.find(scope);
            if (hw == s_header_words.end()) continue;
            for (auto &w : hw->second) replace_all(out, w.chinese, w.english);
        }
        if (s_header_names.count(eng)) {
            std::string named;
            size_t i = 0;
            while (i < out.size()) {
                if ((unsigned char)out[i] < 0x80) { named += out[i++]; continue; }
                size_t j = i;
                while (j < out.size() && ((unsigned char)out[j] >= 0x80 ||
                       (out[j] == ' ' && j + 1 < out.size() && (unsigned char)out[j + 1] >= 0x80))) j++;
                const std::string run = out.substr(i, j - i);
                auto rit = s_reverse.find(run);
                named += (rit != s_reverse.end()) ? rit->second : run;
                i = j;
            }
            out = named;
        }
        return true;
    }

    /* Prefix match: handle "品質 (能量護盾): +24%" → "Quality: +24%" */
    std::string matched_hdr;
    const char *prefix_eng = find_header_prefix(key_part, matched_hdr, s_item_headers_vec);
    if (prefix_eng) {
        out = std::string(prefix_eng) + rest;
        return true;
    }

    return false;
}

/* ========== Helper: strip and restore suffixes like (implicit), (crafted) ========== */

static std::string strip_suffix(const std::string &line, std::string &suffix) {
    suffix.clear();

    for (auto &sfx : s_mod_suffixes_vec) {
        std::string padded = " " + sfx;
        if (line.size() > padded.size() &&
            line.compare(line.size() - padded.size(), padded.size(), padded) == 0) {
            suffix = padded;
            return line.substr(0, line.size() - padded.size());
        }
    }

    /* "… — 無法使用的值": advanced copy's marker for a roll that cannot scale. Unlike the
     * (implicit)-style suffixes above it is TRANSLATED on re-attachment, because POB parses
     * the English marker (Item.lua:1059 strips " - Unscalable Value"). ASCII hyphen on purpose
     * (sanitiseText folds dashes to '-'); the em-dash is required on the Chinese side, to tell
     * GGG's marker from prose that merely ends with the same characters. */
    {
        static const char kUnscalableZh[] =
            "\xe7\x84\xa1\xe6\xb3\x95\xe4\xbd\xbf\xe7\x94\xa8\xe7\x9a\x84\xe5\x80\xbc"; /* 無法使用的值 (PoE1) */
        static const char kEmDash[] = "\xe2\x80\x94";                                   /* — U+2014 */
        auto try_marker = [&](const std::string &marker, std::string &out) {
            const size_t ulen = marker.size();
            if (ulen == 0 || line.size() <= ulen ||
                line.compare(line.size() - ulen, ulen, marker) != 0) return false;
            size_t p = line.size() - ulen;
            while (p > 0 && line[p - 1] == ' ') p--;
            if (p < 3 || line.compare(p - 3, 3, kEmDash) != 0) return false;
            p -= 3;
            while (p > 0 && line[p - 1] == ' ') p--;
            if (p == 0) return false;
            out = line.substr(0, p);
            return true;
        };
        std::string stripped;
        if (try_marker(kUnscalableZh, stripped)) {
            suffix = " - Unscalable Value";
            return stripped;
        }
        for (const std::string &m : s_unscalable_markers) {
            if (try_marker(m, stripped)) {
                suffix = " - Unscalable Value";
                return stripped;
            }
        }
    }
    return line;
}

/* ========== Helper: rebuild a 3.29 advanced-copy annotation line ==========
**
** The game writes, inside braces, [wrapper] core "affix name"(tier)— tags:
**     { 已大師工藝 前綴 "進化的"(階級：3)— 防禦,護甲,能量護盾 }
** POB reads ONLY the words before the literal "Modifier" and looks each one up in Item.lua's
** lineFlags table (Item.lua:495), so the line collapses to its flag words:
**     { Master Crafted Prefix Modifier }
** which decides whether a mod counts as an implicit at all. Dropped: the tier and tags (only
** matter once an affix name matched); the quoted name too, except as handled below.
** Marker strings, the em-dash separator and spacing come from GGPK clientstrings
** (ModDescriptionLine*, AdvancedModDisplayTags). See gen_item_metadata.py. */
static bool rebuild_mod_annotation(const std::string &line, std::string &out) {
    if (line.size() < 3 || line[0] != '{' || line.back() != '}') return false;
    std::string body = line.substr(1, line.size() - 2);

    /* Cut the tag list. AdvancedModDisplayTags is " — {0}" in English but
    ** "— {0}" in Chinese, so anchor on the em-dash itself, not the spacing. */
    static const char kEmDash[] = "\xe2\x80\x94";  /* — U+2014 */
    size_t dash = body.find(kEmDash);
    if (dash != std::string::npos) body.erase(dash);

    /* The quoted affix name, kept before the cut: POB reads it back (Item.lua:463,
    ** fullModName:match("^.*Modifier \"(.*)\"")) to fill the prefix/suffix slots. Flags alone
    ** leave them empty, and on a CLUSTER JEWEL that erases the item: a "{ " line sets
    ** self.crafted (Item.lua:455), ItemsTab then calls CraftClusterJewel (ItemsTab.lua:1858),
    ** which rebuilds the jewel FROM the affix dropdowns and drops every rolled notable.
    ** One English name covering many modifiers is fine: POB narrows by the following stat line
    ** (Item.lua:871-889); all 284 cluster-jewel notables share the name "Notable". */
    std::string affix_zh;
    size_t quote = body.find('"');
    if (quote != std::string::npos) {
        size_t quote2 = body.find('"', quote + 1);
        if (quote2 != std::string::npos)
            affix_zh = body.substr(quote + 1, quote2 - quote - 1);
        /* Every marker sits before the quoted name, so cutting here keeps a
        ** marker-shaped affix name from matching. */
        body.erase(quote);
    }

    /* Collect flag words. s_mod_annotations_vec is longest-first and each match
    ** is consumed, so 固定詞綴 cannot steal from 灼烙總督固定詞綴. */
    std::vector<std::pair<size_t, std::string>> found;
    for (auto &m : s_mod_annotations_vec) {
        size_t p = body.find(m.chinese);
        if (p == std::string::npos) continue;
        found.push_back({p, m.english});
        body.erase(p, m.chinese.size());
    }

    if (found.empty()) {
        /* GGG ships a few annotations untranslated (ModDescriptionLineBrequel-
        ** Mutated = "Foulborn Unique Modifier"). Those already parse — pass them
        ** through with the tags removed. */
        size_t b = body.find_first_not_of(" \t");
        size_t e = body.find_last_not_of(" \t");
        if (b == std::string::npos) return false;
        std::string trimmed = body.substr(b, e - b + 1);
        for (unsigned char c : trimmed) if (c >= 0x80) return false;
        if (trimmed.find("Modifier") == std::string::npos) return false;
        out = "{ " + trimmed + " }";
        return true;
    }

    std::sort(found.begin(), found.end());  /* keep the game's word order */
    out = "{";
    for (auto &f : found) { out += ' '; out += f.second; }
    out += " Modifier";
    /* Only names with ONE English counterpart that POB has an affix for are in the map (the
    ** generator drops the rest); an unresolved name is simply left off. */
    if (!affix_zh.empty()) {
        auto ait = s_affix_names.find(affix_zh);
        if (ait != s_affix_names.end()) out += " \"" + ait->second + "\"";
    }
    out += " }";
    return true;
}

/* ========== Helper: reverse-translate one line ========== */

/* ========== Section grammar: split into sections and label every line ==========
**
** Anchors are the ones POB itself uses (Item.lua switches parser stage on the Item Level
** line); every test is data-driven (a header key is recognised by translating it, not by
** Chinese spelled into this file). Section ORDER is not an anchor. Anything the grammar
** cannot place stays Unknown, meaning "do exactly what the per-line code did". */

static bool is_separator_line(const std::string &l) {
    if (l.size() < 4) return false;
    for (char c : l) if (c != '-') return false;
    return true;
}

static bool is_annotation_line(const std::string &l) {
    return l.size() >= 2 && l[0] == '{' && l.back() == '}';
}

static bool is_enchant_line(const std::string &l) {
    static const char suf[] = " (enchant)";
    const size_t n = sizeof(suf) - 1;
    return l.size() > n && l.compare(l.size() - n, n, suf) == 0;
}

/* English name of the "key:" part, or nullptr when the key is not a known one. */
static const char* header_key_english(const std::string &line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return nullptr;
    return find_header_mapping(line.substr(0, colon), s_item_headers_vec);
}

/* One splitter, used by both the classifier's caller and the debug dump, so the
** two can never disagree about where the lines are. */
static std::vector<std::string> split_input_lines(const std::string &input) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p < input.size()) {
        size_t eol = input.find('\n', p);
        if (eol == std::string::npos) eol = input.size();
        std::string l = input.substr(p, eol - p);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
        p = eol + 1;
    }
    return out;
}

static std::vector<LineKind> classify_lines(const std::vector<std::string> &lines) {
    std::vector<LineKind> kinds(lines.size(), LineKind::Unknown);

    /* 1. group line indices into dash-separated sections */
    std::vector<std::vector<size_t>> sections(1);
    for (size_t i = 0; i < lines.size(); i++) {
        if (is_separator_line(lines[i])) {
            kinds[i] = LineKind::Separator;
            sections.push_back({});
            continue;
        }
        if (!lines[i].empty()) sections.back().push_back(i);
    }
    if (sections.empty() || sections[0].empty()) return kinds;

    /* 2. the Item Level line closes the property block. No Item Level means this
     *    is not the shape we know (a gem's "N% experience" popup, a partial
     *    paste), so label nothing and let every line take the old path. */
    size_t il_sec = (size_t)-1;
    for (size_t s = 0; s < sections.size() && il_sec == (size_t)-1; s++) {
        for (size_t idx : sections[s]) {
            const char *k = header_key_english(lines[idx]);
            if (k && strcmp(k, "Item Level") == 0) { il_sec = s; break; }
        }
    }
    if (il_sec == (size_t)-1) return kinds;

    for (size_t idx : sections[0]) kinds[idx] = LineKind::Header;
    for (size_t s = 1; s <= il_sec; s++)
        for (size_t idx : sections[s]) kinds[idx] = LineKind::Property;

    /* 3. rarity decides whether flavour text is even possible */
    bool is_unique = false;
    for (size_t idx : sections[0]) {
        const char *k = header_key_english(lines[idx]);
        if (!k || strcmp(k, "Rarity") != 0) continue;
        std::string v = lines[idx].substr(lines[idx].find(':') + 1);
        size_t b = v.find_first_not_of(" \t"), e = v.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        const char *rv = find_header_mapping(v.substr(b, e - b + 1), s_rarity_values_vec);
        if (rv && strcmp(rv, "Unique") == 0) is_unique = true;
    }

    /* Advanced copy annotates every modifier section. Without annotations (plain Ctrl+C) a mod
     * section looks like flavour text, so the positional flavour rule stays off and the content
     * test in translation_reverse_text keeps the job. */
    bool any_annotation = false;
    for (const std::string &l : lines) if (is_annotation_line(l)) any_annotation = true;

    /* 4. sections past Item Level */
    for (size_t s = il_sec + 1; s < sections.size(); s++) {
        const std::vector<size_t> &sec = sections[s];
        if (sec.empty()) continue;
        bool has_ann = false, all_ench = true, all_status = true, any_skip = false;
        for (size_t idx : sec) {
            const std::string &l = lines[idx];
            if (is_annotation_line(l)) has_ann = true;
            if (!is_enchant_line(l))   all_ench = false;
            if (!find_header_mapping(l, s_status_lines_vec) &&
                !find_header_mapping(l, s_influence_tags_vec)) all_status = false;
            for (const std::string &p : s_skip_patterns_vec)
                if (l.find(p) != std::string::npos) any_skip = true;
        }
        LineKind k = LineKind::Unknown;
        if (has_ann)         k = LineKind::Mods;      /* checked first: influence
                                                       * lines share the section */
        else if (all_ench)   k = LineKind::Enchant;
        else if (all_status) k = LineKind::Status;
        else if (any_skip)   k = LineKind::Description;
        for (size_t idx : sec) kinds[idx] = k;
    }

    /* 5. The prose that trails the last section carrying mechanics: either the item class
     *    instruction ("放置到一個天賦樹的珠寶插槽中…", boilerplate the dictionary has) or a
     *    unique's flavour text (per-item, in no dictionary). The discriminator is "does the
     *    dictionary know this whole line", not position or a keyword list (GGG writes 右鍵點擊
     *    on jewels but 點擊右鍵 on flasks). Both are dropped: neither feeds a calculation, and
     *    POB would render them as a modifier it could not parse.
     *    Anything that is neither stays Unknown, so a mod section that somehow reached here
     *    stays visible. */
    if (any_annotation) {
        size_t last_mech = (size_t)-1;
        for (size_t s = il_sec + 1; s < sections.size(); s++) {
            if (sections[s].empty()) continue;
            LineKind k = kinds[sections[s][0]];
            if (k == LineKind::Mods || k == LineKind::Enchant) last_mech = s;
        }
        if (last_mech != (size_t)-1) {
            for (size_t s = last_mech + 1; s < sections.size(); s++) {
                if (sections[s].empty()) continue;
                if (kinds[sections[s][0]] != LineKind::Unknown) continue;
                bool all_known = true;
                for (size_t idx : sections[s])
                    if (!s_reverse.count(lines[idx])) { all_known = false; break; }
                LineKind k = all_known ? LineKind::Description
                           : is_unique ? LineKind::Flavour
                                       : LineKind::Unknown;
                if (k == LineKind::Unknown) continue;
                for (size_t idx : sections[s]) kinds[idx] = k;
            }
        }
    }

    return kinds;
}

static std::string reverse_one_line_impl(const std::string &line, LineKind kind);

/* Thin wrapper so every branch is recorded in one place. reverse_one_line_impl
** sets s_rev_rule (via the REV macro) right before it returns. */
static std::string reverse_one_line(const std::string &line, LineKind kind = LineKind::Unknown) {
    if (!s_trace_on) return reverse_one_line_impl(line, kind);

    s_rev_rule = "?";
    s_rev_key.clear();
    s_rev_file.clear();
    std::string out = reverse_one_line_impl(line, kind);

    auto esc = [](const std::string &s) {
        std::string r;
        for (char c : s) r += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
        return r;
    };
    s_trace_buf += s_rev_rule;
    s_trace_buf += '\t'; s_trace_buf += kind_name(kind);
    s_trace_buf += '\t'; s_trace_buf += esc(s_rev_key);
    s_trace_buf += '\t'; s_trace_buf += esc(s_rev_file);
    s_trace_buf += '\t'; s_trace_buf += esc(line);
    s_trace_buf += '\t'; s_trace_buf += esc(out);
    s_trace_buf += '\n';
    return out;
}

static std::string reverse_one_line_impl(const std::string &line_in, LineKind kind) {
    if (line_in.empty()) return REV("empty", line_in);
    /* GGG markup that survived into the copied text ("[1.00E|裂紋怪客]"): the shown words are
     * the part after the pipe. Only a line with non-ASCII inside the brackets is touched. */
    std::string line_markup;
    if (line_in.find('[') != std::string::npos && line_in.find(']') != std::string::npos) {
        const std::string shown = strip_brackets(line_in);
        bool cjk = false;
        for (unsigned char c : shown) if (c >= 0x80) { cjk = true; break; }
        if (cjk && shown != line_in) line_markup = shown;
    }
    const std::string &line = line_markup.empty() ? line_in : line_markup;

    /* 0. Separator lines. The 3.29+ copy format emits variable-width dash
     * rows (matching the section width); POB only recognises exactly
     * "--------", so normalize any all-dash line to the canonical 8-dash. */
    {
        bool all_dash = line.size() >= 4;
        for (size_t i = 0; all_dash && i < line.size(); i++) {
            if (line[i] != '-') all_dash = false;
        }
        if (all_dash) return REV("separator", "--------");
    }
    if (line.find("--------") != std::string::npos) return REV("separator-embedded", line);

    /* 0a. Sections the grammar placed as carrying no mechanics. Position is the
     * better witness than a per-line guess: it holds for flavour that contains a
     * number and for a description whose second sentence has no keyword in it. */
    if (kind == LineKind::Flavour) {
        log_translation_miss("FLAVOUR ", line);
        return REV("flavour", "");
    }
    if (kind == LineKind::Description) return REV("description", "");

    /* 0b. Advanced-copy affix annotation lines ({ 前綴 "微光的"(階層：10)— 防禦,能量護盾 }):
     * rebuilt into the flag-only English form POB parses so implicit / fractured / crafted /
     * exarch survive. Unrecognised ones are stripped (in Chinese they would reach POB as '?'). */
    if (line.size() >= 2 && line[0] == '{' && line.back() == '}') {
        std::string rebuilt;
        if (rebuild_mod_annotation(line, rebuilt)) return REV("annotation", rebuilt);
        return REV("annotation-unknown", "");  /* empty = skip this line */
    }

    /* 0c. Reminder text. Chinese wraps it in FULL-WIDTH parens, so POB's own
     * skip rule (Item.lua:445, "^%(%a+") never fires and the line gets parsed
     * as a modifier. It carries no mechanics — drop it, as upstream does. */
    {
        static const char kFullParen[] = "\xef\xbc\x88";  /* （ U+FF08 */
        if (line.size() >= 3 && line.compare(0, 3, kFullParen) == 0) return REV("reminder", "");
    }

    /* 1. Try item header translation (物品種類:, 稀有度:, etc.) */
    std::string header_result;
    if (try_header_translate(line, header_result) && !header_result.empty()) {
        return REV("header", header_result);
    }

    /* 1a. Game UI instruction text ("點擊右鍵以喝下藥劑…"). Kept as a fallback for
     * lines the grammar could not place; inside a modifier section the keyword
     * list is not allowed to fire, because there a match would delete a real mod. */
    if (kind != LineKind::Mods) {
        for (auto &pat : s_skip_patterns_vec) {
            if (line.find(pat) != std::string::npos)
                return REV("skip-pattern", "");
        }
    }

    /* 1b. Flask-specific dynamic lines (持續, 充能次數, etc.) */
    {
        /* 持續 = E6 8C 81 E7 BA 8C, 秒 = E7 A7 92 */
        static const char s_lasts[] = "\xe6\x8c\x81\xe7\xba\x8c";       /* 持續 */
        static const char s_sec[]   = "\xe7\xa7\x92";                     /* 秒 */
        /* 每次使用會從 = E6AF8F E6ACA1 E4BDBF E794A8 E69C83 E5BE9E */
        static const char s_consumes_prefix[] = "\xe6\xaf\x8f\xe6\xac\xa1\xe4\xbd\xbf\xe7\x94\xa8\xe6\x9c\x83\xe5\xbe\x9e"; /* 每次使用會從 */
        /* 充能次數 = E5 85 85 E8 83 BD E6 AC A1 E6 95 B8 */
        static const char s_charges[] = "\xe5\x85\x85\xe8\x83\xbd\xe6\xac\xa1\xe6\x95\xb8"; /* 充能次數 */
        /* 中消耗 = E4 B8 AD E6 B6 88 E8 80 97 */
        static const char s_consume[] = "\xe4\xb8\xad\xe6\xb6\x88\xe8\x80\x97"; /* 中消耗 */
        /* 次 = E6 AC A1 */
        static const char s_times[] = "\xe6\xac\xa1"; /* 次 */
        /* 目前有 = E7 9B AE E5 89 8D E6 9C 89 */
        static const char s_currently[] = "\xe7\x9b\xae\xe5\x89\x8d\xe6\x9c\x89"; /* 目前有 */

        /* "持續 X 秒" — skip: POB computes duration from base type data */
        if (line.find(s_lasts) == 0 && line.find(s_sec) != std::string::npos) {
            return REV("flask-duration", "");  /* empty = skip this line */
        }

        /* "每次使用會從 X 充能次數中消耗 Y 次" → "Consumes Y of X Charges on use" */
        /* Debug: log search results for consumes pattern */
        {
            bool has_prefix = line.find(s_consumes_prefix) != std::string::npos;
            bool has_charges = line.find(s_charges) != std::string::npos;
            /* Check if line contains 每次使用 (common substring) for targeted logging */
            static const char s_usage[] = "\xe6\xaf\x8f\xe6\xac\xa1\xe4\xbd\xbf\xe7\x94\xa8"; /* 每次使用 */
            if (line.find(s_usage) != std::string::npos) {
                char msg[512];
                int mpos = snprintf(msg, sizeof(msg),
                    "[pob-proxy] Flask consumes check: prefix=%d charges=%d line(%d)=",
                    has_prefix, has_charges, (int)line.size());
                for (size_t b = 0; b < line.size() && b < 80 && mpos < 450; b++) {
                    mpos += snprintf(msg + mpos, sizeof(msg) - mpos,
                        " %02X", (unsigned char)line[b]);
                }
                snprintf(msg + mpos, sizeof(msg) - mpos, "\n");
                OutputDebugStringA(msg);
            }
        }
        if (line.find(s_consumes_prefix) != std::string::npos &&
            line.find(s_charges) != std::string::npos) {
            /* Extract X (total charges) and Y (consumed) */
            size_t after_prefix = line.find(s_consumes_prefix) + strlen(s_consumes_prefix);
            /* Skip space */
            while (after_prefix < line.size() && line[after_prefix] == ' ') after_prefix++;
            /* Read X (digits) */
            size_t x_start = after_prefix;
            while (after_prefix < line.size() && line[after_prefix] >= '0' && line[after_prefix] <= '9') after_prefix++;
            std::string total_charges = line.substr(x_start, after_prefix - x_start);

            /* Find 中消耗 and extract Y after it */
            size_t consume_pos = line.find(s_consume);
            if (consume_pos != std::string::npos) {
                size_t after_consume = consume_pos + strlen(s_consume);
                while (after_consume < line.size() && line[after_consume] == ' ') after_consume++;
                /* Y might include "(augmented)" or similar */
                size_t y_end = line.rfind(s_times);
                if (y_end != std::string::npos && y_end > after_consume) {
                    std::string consumed = line.substr(after_consume, y_end - after_consume);
                    while (!consumed.empty() && consumed.back() == ' ') consumed.pop_back();
                    /* Skip: POB computes charges from base type data */
                    return REV("flask-charges", "");  /* empty = skip this line */
                }
            }
        }

        /* "目前有 X 充能次數" — skip: POB computes charges from base type data */
        if (line.find(s_currently) == 0 && line.find(s_charges) != std::string::npos) {
            return REV("flask-charges", "");  /* empty = skip this line */
        }
    }

    /* 2. Try influence/eldritch tags */
    {
        const char *inf_eng = find_header_mapping(line, s_influence_tags_vec);
        if (inf_eng) {
            rev_note(line, "item_metadata.influence_tags");
            return REV("influence", std::string(inf_eng));
        }
    }

    /* 2b. Standalone item status lines (Corrupted, Mirrored, etc.) */
    {
        const char *st_eng = find_header_mapping(line, s_status_lines_vec);
        if (st_eng) {
            rev_note(line, "item_metadata.status_lines");
            return REV("status", std::string(st_eng));
        }
    }

    /* 3. Strip suffix like (implicit), (crafted) before lookup */
    std::string suffix;
    std::string core = strip_suffix(line, suffix);

    /* 3a. Strip "(augmented)" from MIDDLE of line — game inserts it after modified values
     * e.g. "3.50 秒內回復 2509 (augmented) 生命" → "3.50 秒內回復 2509 生命"
     * The suffix is preserved separately for re-attachment. */
    {
        static const char aug[] = " (augmented)";
        static const size_t aug_len = 12;
        size_t aug_pos = core.find(aug);
        /* Only strip if it's in the middle (not at end — that's handled by strip_suffix) */
        if (aug_pos != std::string::npos && aug_pos + aug_len < core.size()) {
            core = core.substr(0, aug_pos) + core.substr(aug_pos + aug_len);
        }
    }

    /* 3b. "配置" (Allocates) prefix handling for passive skill enchants */
    /* Patterns: "配置 <name>", "若禁忌血肉上有符合的詞綴，配置 <name>", etc. */
    {
        /* UTF-8: 配置=E9 85 8D E7 BD AE */
        static const char pz[] = "\xe9\x85\x8d\xe7\xbd\xae";
        static const size_t pz_len = 6; /* strlen(配置) */
        size_t pz_pos = core.find(pz);
        if (pz_pos != std::string::npos) {
            /* Find the passive name after "配置" (skip optional space) */
            size_t name_start = pz_pos + pz_len;
            if (name_start < core.size() && core[name_start] == ' ') name_start++;
            if (name_start < core.size()) {
                std::string passive_name = core.substr(name_start);
                /* Look up passive name in reverse map */
                auto pit2 = s_reverse.find(passive_name);
                if (pit2 != s_reverse.end()) {
                    std::string eng_name = pit2->second;
                    std::string result;
                    if (pz_pos == 0) {
                        /* Simple: "配置 <name>" → "Allocates <name>" */
                        result = "Allocates " + eng_name;
                    } else {
                        /* Prefix before 配置: translate the prefix part too */
                        std::string prefix_cn = core.substr(0, pz_pos);
                        /* "若禁忌血肉上有符合的詞綴，" → "if you have the matching modifier on Forbidden Flesh, " */
                        /* Try to find the full pattern in s_reverse_pattern */
                        std::string full_pattern = prefix_cn + pz + " #";
                        auto fpit = s_reverse_pattern.find(full_pattern);
                        if (fpit != s_reverse_pattern.end()) {
                            /* 這裡填的是天賦名不是數字,但位置的問法完全相同:
                            ** 構造出來的中文樣式只有一格,英文那邊的計畫就指向它。 */
                            result = fill_plan(fpit->second, {eng_name});
                        } else {
                            /* Fuzzy try */
                            std::string fuzzy_fp = normalize_chinese_synonyms(full_pattern);
                            fpit = s_reverse_pattern.find(fuzzy_fp);
                            if (fpit != s_reverse_pattern.end()) {
                                result = fill_plan(fpit->second, {eng_name});
                            } else {
                                /* Fallback: just "Allocates <name>" */
                                result = "Allocates " + eng_name;
                            }
                        }
                    }
                    char msg[512];
                    snprintf(msg, sizeof(msg), "[pob-proxy] Allocates match: '%.*s' -> '%.*s'\n",
                             (int)(core.size() > 80 ? 80 : core.size()), core.c_str(),
                             (int)(result.size() > 80 ? 80 : result.size()), result.c_str());
                    OutputDebugStringA(msg);
                    rev_note(passive_name, "s_reverse");
                    return REV("allocates", result + suffix);
                }
            }
        }
    }

    /* 3c. Composed lines ("Added Small Passive Skills grant: {0}" is built by substitution, so it
     * is in no dictionary): strip the prefix, translate the remainder as an ordinary stat, put the
     * English prefix back. Runs BEFORE the direct lookups on purpose: the dictionary holds the same
     * Chinese under BOTH "grant:" and "also grant:", so the merged map answers wrongly half the time
     * for a rolled modifier. Not cosmetic: POB reads the cluster jewel's SKILL from the enchant
     * line via an exact whole-line lookup (ModParser.lua:6463); a wrong line rebuilds the jewel
     * around the wrong skill and discards every rolled notable. */
    {
        static int depth = 0;
        if (depth == 0) {
            const bool is_enchant = suffix.find("(enchant)") != std::string::npos;
            for (auto &p : s_composite_prefixes_vec) {
                if (core.size() <= p.chinese.size()) continue;
                if (core.compare(0, p.chinese.size(), p.chinese) != 0) continue;
                std::string inner = core.substr(p.chinese.size());
                depth++;
                /* the inner fragment has no section of its own */
                std::string inner_en = reverse_one_line_impl(inner, LineKind::Unknown);
                depth--;
                if (inner_en.empty() || inner_en == inner) break;  /* inner unknown */

                /* Every number the inner carried has to survive, or the rule would wrap a
                 * WRONG inner match into confident nonsense. */
                bool numbers_survived = true;
                for (const std::string &n : extract_numbers(inner)) {
                    if (inner_en.find(n) == std::string::npos) { numbers_survived = false; break; }
                }
                if (!numbers_survived) break;

                const std::string &en = is_enchant ? p.english_enchant : p.english;
                return REV("composite", en + inner_en + suffix);
            }
        }
    }

    /* 3d. "殘存 <基底名>" -- a Vestigial item's base-type line, composed from the "Vestigial {0}"
     * clientstring and so in no dictionary. Translate the base name and put the English prefix
     * back (POB strips "^Vestigial " itself, Item.lua:973). Guarded by "the remainder is a whole
     * dictionary key", so a modifier that merely starts with 殘存 is not rewritten. */
    {
        static const char kVestigialZh[] = "\xe6\xae\x98\xe5\xad\x98 "; /* 殘存 + space */
        const size_t vlen = sizeof(kVestigialZh) - 1;
        if (core.size() > vlen && core.compare(0, vlen, kVestigialZh) == 0) {
            const std::string rest = core.substr(vlen);
            auto vit = s_reverse.find(rest);
            if (vit != s_reverse.end()) {
                rev_note(rest, s_reverse_origin.count(rest) ? s_reverse_origin[rest] : "?");
                return REV("vestigial-base", "Vestigial " + vit->second + suffix);
            }
        }
    }

    /* 4. Exact match in reverse map */
    auto it = s_reverse.find(core);
    if (it != s_reverse.end()) {
        char msg[512];
        snprintf(msg, sizeof(msg), "[pob-proxy] Reverse exact match: '%.*s' -> '%.*s'%s\n",
                 (int)(core.size() > 60 ? 60 : core.size()), core.c_str(),
                 (int)(it->second.size() > 60 ? 60 : it->second.size()), it->second.c_str(),
                 suffix.empty() ? "" : suffix.c_str());
        OutputDebugStringA(msg);
        rev_note(core, s_reverse_origin.count(core) ? s_reverse_origin[core] : "?");
        return REV("exact", it->second + suffix);
    }

    /* 5. Pattern match: hash digits and look up in pre-hashed pattern map */
    std::string pattern = normalize_whitespace(digits_to_hash(core));
    auto pit = s_reverse_pattern.find(pattern);
    if (pit != s_reverse_pattern.end()) {
        /* Found English pattern, fill in numbers from original line */
        std::string result = fill_plan(pit->second, hash_slot_values(core, /*normalize_ph=*/false)) + suffix;
        char msg[512];
        snprintf(msg, sizeof(msg), "[pob-proxy] Reverse pattern match: '%.*s' -> '%.*s'\n",
                 (int)(core.size() > 80 ? 80 : core.size()), core.c_str(),
                 (int)(result.size() > 80 ? 80 : result.size()), result.c_str());
        OutputDebugStringA(msg);
        rev_note(pattern, s_reverse_pattern_origin.count(pattern) ? s_reverse_pattern_origin[pattern] : "?");
        return REV("pattern", result);
    }
    /* Debug: dump pattern hex for step 5 miss */
    {
        bool has_cjk_dbg = false;
        for (size_t b = 0; b < core.size(); b++) {
            if ((unsigned char)core[b] > 127) { has_cjk_dbg = true; break; }
        }
        if (has_cjk_dbg) {
            char msg[768];
            int mpos = snprintf(msg, sizeof(msg), "[pob-proxy] STEP5 MISS pattern(%d)=",
                                (int)pattern.size());
            for (size_t b = 0; b < pattern.size() && b < 60 && mpos < 600; b++) {
                mpos += snprintf(msg + mpos, sizeof(msg) - mpos,
                    " %02X", (unsigned char)pattern[b]);
            }
            mpos += snprintf(msg + mpos, sizeof(msg) - mpos, " core(%d)=",
                             (int)core.size());
            for (size_t b = 0; b < core.size() && b < 60 && mpos < 700; b++) {
                mpos += snprintf(msg + mpos, sizeof(msg) - mpos,
                    " %02X", (unsigned char)core[b]);
            }
            snprintf(msg + mpos, sizeof(msg) - mpos, "\n");
            OutputDebugStringA(msg);
        }
    }

    /* 5b. Fuzzy pattern match: normalize synonyms, 點, 的, spacing, retry */
    {
        std::string fuzzy = normalize_chinese_synonyms(pattern);
        if (fuzzy != pattern) {
            pit = s_reverse_pattern.find(fuzzy);
            if (pit != s_reverse_pattern.end()) {
                std::string result = fill_plan(pit->second, hash_slot_values(core, /*normalize_ph=*/false)) + suffix;
                char msg[512];
                snprintf(msg, sizeof(msg), "[pob-proxy] Reverse fuzzy match: '%.*s' -> '%.*s'\n",
                         (int)(core.size() > 80 ? 80 : core.size()), core.c_str(),
                         (int)(result.size() > 80 ? 80 : result.size()), result.c_str());
                OutputDebugStringA(msg);
                rev_note(fuzzy, s_reverse_pattern_origin.count(fuzzy) ? s_reverse_pattern_origin[fuzzy] : "?");
                return REV("fuzzy", result);
            }
        }
    }

    /* 5c. Sign prefix match: game text has "+#%" or "-#%" but translations use "{0}%" (no sign) */
    {
        std::string sign_prefix;
        std::string no_sign = pattern;
        /* Strip leading '+' or '-' before '#' */
        if (no_sign.size() >= 2 && (no_sign[0] == '+' || no_sign[0] == '-') && no_sign[1] == '#') {
            sign_prefix = no_sign.substr(0, 1);
            no_sign = no_sign.substr(1);
        }
        if (!sign_prefix.empty()) {
            /* Try direct pattern lookup without sign */
            pit = s_reverse_pattern.find(no_sign);
            if (pit != s_reverse_pattern.end()) {
                std::string result = sign_prefix + fill_plan(pit->second, hash_slot_values(core, /*normalize_ph=*/false)) + suffix;
                char msg[512];
                snprintf(msg, sizeof(msg), "[pob-proxy] Reverse sign-prefix match: '%.*s' -> '%.*s'\n",
                         (int)(core.size() > 80 ? 80 : core.size()), core.c_str(),
                         (int)(result.size() > 80 ? 80 : result.size()), result.c_str());
                OutputDebugStringA(msg);
                rev_note(no_sign, s_reverse_pattern_origin.count(no_sign) ? s_reverse_pattern_origin[no_sign] : "?");
                return REV("sign", result);
            }
            /* Try fuzzy on no_sign */
            std::string fuzzy_no_sign = normalize_chinese_synonyms(no_sign);
            if (fuzzy_no_sign != no_sign) {
                pit = s_reverse_pattern.find(fuzzy_no_sign);
                if (pit != s_reverse_pattern.end()) {
                    std::string result = sign_prefix + fill_plan(pit->second, hash_slot_values(core, /*normalize_ph=*/false)) + suffix;
                    char msg[512];
                    snprintf(msg, sizeof(msg), "[pob-proxy] Reverse sign-prefix fuzzy: '%.*s' -> '%.*s'\n",
                             (int)(core.size() > 80 ? 80 : core.size()), core.c_str(),
                             (int)(result.size() > 80 ? 80 : result.size()), result.c_str());
                    OutputDebugStringA(msg);
                    rev_note(fuzzy_no_sign, s_reverse_pattern_origin.count(fuzzy_no_sign) ? s_reverse_pattern_origin[fuzzy_no_sign] : "?");
                    return REV("sign-fuzzy", result);
                }
            }
        }
    }

    /* 5d. Whitespace-blind: the same pattern with every space removed (PoE2's
     * client writes no space between a number and the words). Sign handled as
     * in 5c. Ambiguous compact keys were never stored. */
    {
        std::string ck = compact_key(pattern);
        std::string sign_prefix;
        auto cit = s_reverse_pattern_compact.find(ck);
        if (cit == s_reverse_pattern_compact.end() && ck.size() >= 2 && (ck[0] == '+' || ck[0] == '-') && ck[1] == '#') {
            sign_prefix = ck.substr(0, 1);
            cit = s_reverse_pattern_compact.find(ck.substr(1));
        }
        if (cit != s_reverse_pattern_compact.end()) {
            std::string result = sign_prefix + fill_plan(cit->second, hash_slot_values(core, /*normalize_ph=*/false)) + suffix;
            const std::string noted = sign_prefix.empty() ? ck : ck.substr(1);
            rev_note(noted, s_reverse_pattern_compact_origin.count(noted) ? s_reverse_pattern_compact_origin[noted] : "?");
            return REV("compact", result);
        }
    }

    /* 6. Try base type lookup in translations (check if value matches a Chinese base type) */
    it = s_reverse.find(line);
    if (it != s_reverse.end()) {
        rev_note(line, s_reverse_origin.count(line) ? s_reverse_origin[line] : "?");
        return REV("base-type", it->second);
    }

    /* 7. No match, log with hex dump for debugging */
    if (core.size() > 0) {
        bool has_cjk = false;
        bool has_digit_or_pct = false;
        for (size_t i = 0; i < core.size(); i++) {
            if ((unsigned char)core[i] > 127) has_cjk = true;
            if ((core[i] >= '0' && core[i] <= '9') || core[i] == '%') has_digit_or_pct = true;
        }
        if (has_cjk) {
            if (has_digit_or_pct) {
                /* Likely a stat mod — full hex dump of both input and pattern */
                char msg[768];
                int mpos = snprintf(msg, sizeof(msg), "[pob-proxy] NO MATCH input(%d) hex:",
                                    (int)core.size());
                for (size_t b = 0; b < core.size() && b < 50 && mpos < 400; b++) {
                    mpos += snprintf(msg + mpos, sizeof(msg) - mpos,
                        " %02X", (unsigned char)core[b]);
                }
                mpos += snprintf(msg + mpos, sizeof(msg) - mpos, " | pattern(%d) hex:",
                                 (int)pattern.size());
                for (size_t b = 0; b < pattern.size() && b < 50 && mpos < 700; b++) {
                    mpos += snprintf(msg + mpos, sizeof(msg) - mpos,
                        " %02X", (unsigned char)pattern[b]);
                }
                snprintf(msg + mpos, sizeof(msg) - mpos, "\n");
                OutputDebugStringA(msg);
            } else {
                /* Pure CJK text (no digits/%) — likely flavor text or untranslated name */
                char msg[256];
                snprintf(msg, sizeof(msg), "[pob-proxy] Unmatched text (name/flavor): '%.*s'\n",
                         (int)(core.size() > 100 ? 100 : core.size()), core.c_str());
                OutputDebugStringA(msg);
            }
        }
    }

    /* 7b. A "key: value" line in the property block that nothing above could translate. Leaving
     * it is the worst option: POB turns the Chinese into '?' bytes yet the line still satisfies
     * its property regex, so Item.lua:704 flips the parser into the EXPLICIT stage early and every
     * implicit after it is misclassified. Dropping costs nothing (POB recomputes properties from
     * base type plus modifiers). Logged, because an untranslatable property is still a gap. */
    if (kind == LineKind::Property && line.find(':') != std::string::npos) {
        log_translation_miss("PROPERTY ", line);
        return REV("drop-property", "");
    }

    return REV("miss", line);
}

/* ========== Public: reverse-translation trace ========== */

void translation_trace_begin(void) {
    s_trace_on = true;
    s_trace_buf.clear();
}

const char* translation_trace_get(void) {
    return s_trace_buf.c_str();
}

void translation_trace_end(void) {
    s_trace_on = false;
    s_trace_buf.clear();
    s_trace_buf.shrink_to_fit();
}

const char* translation_strip_ggg_markup(const char *s) {
    static std::string holder;
    holder = s ? strip_brackets(s) : std::string();
    return holder.c_str();
}

const char* translation_classify_dump(const char *text) {
    translation_wait_ready();
    static std::string holder;
    holder.clear();
    if (!text) return holder.c_str();
    const std::vector<std::string> lines = split_input_lines(text);
    const std::vector<LineKind> kinds = classify_lines(lines);
    for (size_t i = 0; i < lines.size(); i++) {
        holder += kind_name(kinds[i]);
        holder += '\t';
        holder += lines[i];
        holder += '\n';
    }
    return holder.c_str();
}

/* ========== Public: reverse-translate multi-line text ========== */

char* translation_reverse_text(const char *chinese_text) {
    if (!chinese_text) return nullptr;
    translation_wait_ready();
    if (s_reverse.empty()) return nullptr;

    /* Check if text contains any non-ASCII (potential CJK) */
    bool has_nonascii = false;
    for (const char *p = chinese_text; *p; p++) {
        if ((unsigned char)*p > 127) { has_nonascii = true; break; }
    }
    if (!has_nonascii) return nullptr; /* Pure ASCII, no translation needed */

    /* Log first 60 bytes as hex to determine encoding (UTF-8 vs Big5) */
    {
        char hexmsg[512];
        int hpos = snprintf(hexmsg, sizeof(hexmsg), "[pob-proxy] Paste input (%d bytes) hex:",
                            (int)strlen(chinese_text));
        for (int b = 0; b < 60 && chinese_text[b] && hpos < 450; b++) {
            hpos += snprintf(hexmsg + hpos, sizeof(hexmsg) - hpos,
                " %02X", (unsigned char)chinese_text[b]);
        }
        snprintf(hexmsg + hpos, sizeof(hexmsg) - hpos, "\n");
        OutputDebugStringA(hexmsg);
    }

    std::string input(chinese_text);

    /* Split first, then label: what a line means depends on the section it is in,
    ** so nothing can be decided while still reading one line at a time. */
    const std::vector<std::string> raw_lines = split_input_lines(input);
    const std::vector<LineKind> kinds = classify_lines(raw_lines);

    /* Lines are collected rather than concatenated so the flavour-text pass below
    ** can look at whole sections. */
    struct OutLine { std::string text; bool untranslated; LineKind kind; };
    std::vector<OutLine> out_lines;

    bool any_translated = false;
    bool is_unique = false;
    int prev_rarity_type = 0; /* 0=none, 1=Rare, 2=Unique, 3=Magic/Normal */

    for (size_t li = 0; li < raw_lines.size(); li++) {
        const std::string &line = raw_lines[li];
        const LineKind kind = kinds[li];

        std::string translated = reverse_one_line(line, kind);
        /* Empty return = skip this line (e.g. flask metadata computed by POB) */
        if (translated.empty() && !line.empty()) {
            any_translated = true;
            continue;
        }
        if (translated != line) any_translated = true;

        /* Handle name line after Rarity */
        if (prev_rarity_type > 0 && translated == line) {
            bool has_cjk = false;
            for (size_t i = 0; i < translated.size(); i++) {
                if ((unsigned char)translated[i] > 127) { has_cjk = true; break; }
            }
            if (has_cjk) {
                if (prev_rarity_type == 3) {
                    /* Magic/Normal: the name contains the base type; POB needs it in English.
                     * Longest substring match in s_reverse_bases (not s_reverse: affix names
                     * would give false positives). */
                    std::string best_en;
                    size_t best_len = 0;
                    for (auto& pair : s_reverse_bases) {
                        if (pair.first.size() > best_len &&
                            line.find(pair.first) != std::string::npos) {
                            best_en = pair.second;
                            best_len = pair.first.size();
                        }
                    }
                    if (!best_en.empty()) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "[pob-proxy] Magic base type match: '%.*s' -> '%.*s'\n",
                                 (int)(line.size() > 60 ? 60 : line.size()), line.c_str(),
                                 (int)(best_en.size() > 60 ? 60 : best_en.size()), best_en.c_str());
                        OutputDebugStringA(msg);
                        translated = best_en;
                        any_translated = true;
                    } else {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "[pob-proxy] Magic name no base found: '%.*s'\n",
                                 (int)(line.size() > 80 ? 80 : line.size()), line.c_str());
                        OutputDebugStringA(msg);
                    }
                } else if (prev_rarity_type == 1) {
                    /* Rare: random name, replace with "New Item" */
                    char msg[256];
                    snprintf(msg, sizeof(msg), "[pob-proxy] Random rare name replaced: '%.*s' -> 'New Item'\n",
                             (int)(line.size() > 80 ? 80 : line.size()), line.c_str());
                    OutputDebugStringA(msg);
                    translated = "New Item";
                    any_translated = true;
                } else {
                    /* Unique: try fuzzy lookup for name (synonym normalization) */
                    std::string fuzzy_name = normalize_chinese_synonyms(line);
                    auto nit = s_reverse.find(fuzzy_name);
                    if (nit == s_reverse.end()) {
                        /* Try stripping known unique prefixes (e.g. 穢生 = Foulborn) */
                        /* UTF-8: 穢生=E7 A9 A2 E7 94 9F */
                        static const struct { const char *cn; const char *en; } unique_prefixes[] = {
                            { "\xe7\xa9\xa2\xe7\x94\x9f ", "Foulborn " },  /* 穢生 */
                            { nullptr, nullptr }
                        };
                        for (int up = 0; unique_prefixes[up].cn; up++) {
                            size_t plen = strlen(unique_prefixes[up].cn);
                            if (line.size() > plen && line.compare(0, plen, unique_prefixes[up].cn) == 0) {
                                std::string base_name = line.substr(plen);
                                auto bit = s_reverse.find(base_name);
                                if (bit != s_reverse.end()) {
                                    nit = bit; /* will use this match below */
                                    /* Prepend English prefix */
                                    std::string eng_full = std::string(unique_prefixes[up].en) + bit->second;
                                    s_reverse[line] = eng_full; /* cache for future lookups */
                                    nit = s_reverse.find(line);
                                    break;
                                }
                            }
                        }
                    }
                    if (nit != s_reverse.end()) {
                        translated = nit->second;
                        any_translated = true;
                        char msg[256];
                        snprintf(msg, sizeof(msg), "[pob-proxy] Unique name match: '%.*s' -> '%.*s'\n",
                                 (int)(line.size() > 60 ? 60 : line.size()), line.c_str(),
                                 (int)(translated.size() > 60 ? 60 : translated.size()), translated.c_str());
                        OutputDebugStringA(msg);
                    } else {
                        /* Still not found — log but PoB may still match base type */
                        char msg[256];
                        snprintf(msg, sizeof(msg), "[pob-proxy] Unique name NOT FOUND: '%.*s'\n",
                                 (int)(line.size() > 80 ? 80 : line.size()), line.c_str());
                        OutputDebugStringA(msg);
                    }
                }
            }
        }

        /* Dev log: a CJK line the reverse dictionaries could not translate
         * (name lines are handled by the rarity-specific logic above) */
        if (translated == line && prev_rarity_type == 0) {
            for (unsigned char c : line) {
                if (c >= 0x80) { log_translation_miss("REV ", line); break; }
            }
        }

        /* Detect rarity type for next-line name handling */
        if (translated.find("Rarity: Unique") == 0) {
            prev_rarity_type = 2;
            is_unique = true;
        } else if (translated.find("Rarity: Rare") == 0) {
            prev_rarity_type = 1;
        } else if (translated.find("Rarity: Magic") == 0 ||
                   translated.find("Rarity: Normal") == 0) {
            prev_rarity_type = 3;
        } else {
            prev_rarity_type = 0;
        }

        /* Strip "have " prefix from stat lines — game format includes "have"
           but PoB expects raw stat (e.g. "28% reduced duration" not "have 28%...") */
        if (translated.size() > 5 && translated.compare(0, 5, "have ") == 0) {
            char ch = translated[5];
            if ((ch >= '0' && ch <= '9') || ch == '+' || ch == '-') {
                translated = translated.substr(5);
            }
        }

        {
            bool left_cjk = false;
            if (translated == line) {
                for (unsigned char c : line) if (c >= 0x80) { left_cjk = true; break; }
            }
            out_lines.push_back({ translated, left_cjk, kind });
        }
    }

    /* Unique flavour text, FALLBACK path. POB has no branch for it, so a pasted flavour line is
    ** parsed as a modifier ('?' bytes in Chinese); dropping loses nothing (POB renders flavour
    ** from its own Data/FlavourText). The section grammar already dropped what it could place by
    ** position; this is for plain Ctrl+C text: only a section where NOTHING translated and no
    ** line carries a number qualifies, and sections the grammar labelled are skipped. */
    if (is_unique) {
        size_t i = 0;
        while (i < out_lines.size()) {
            if (out_lines[i].text == "--------") { i++; continue; }
            size_t start = i;
            bool all_untranslated = true, any_digit = false, all_unplaced = true;
            while (i < out_lines.size() && out_lines[i].text != "--------") {
                if (!out_lines[i].untranslated) all_untranslated = false;
                if (out_lines[i].kind != LineKind::Unknown) all_unplaced = false;
                for (char c : out_lines[i].text) if (c >= '0' && c <= '9') any_digit = true;
                i++;
            }
            if (all_unplaced && all_untranslated && !any_digit && i > start) {
                for (size_t k = start; k < i; k++) {
                    log_translation_miss("FLAVOUR ", out_lines[k].text);
                    out_lines[k].text.clear();
                    out_lines[k].untranslated = false;
                }
                any_translated = true;
            }
        }
        /* second pass strips the emptied lines */
        std::vector<OutLine> kept;
        kept.reserve(out_lines.size());
        for (auto &l : out_lines) if (!l.text.empty()) kept.push_back(l);
        out_lines.swap(kept);
    }

    /* Rejoin exactly as the old per-line concatenation did: a newline after every
    ** line that had one in the input, so a trailing newline survives. */
    const bool input_ends_nl = !input.empty() && input.back() == '\n';
    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < out_lines.size(); i++) {
        result += out_lines[i].text;
        if (i + 1 < out_lines.size() || input_ends_nl) result += '\n';
    }

    if (!any_translated) return nullptr;

    /* Allocate and return */
    char *out = (char*)malloc(result.size() + 1);
    if (out) {
        memcpy(out, result.c_str(), result.size() + 1);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "[pob-proxy] Reverse-translated paste text (%d bytes)\n", (int)result.size());
    OutputDebugStringA(msg);

    return out;
}

void translation_free(char *str) {
    free(str);
}

void translation_set_enabled(bool enabled) {
    translation_wait_ready(); /* clears caches the worker may still be filling */
    s_translation_enabled = enabled;
    s_lookup_cache.clear();
    for (auto &kv : s_source_cache) kv.second.clear();
    for (auto &kv : s_item_title_source_cache) kv.second.clear();
    OutputDebugStringA(enabled
        ? "[pob-proxy] Translation ON (F2)\n"
        : "[pob-proxy] Translation OFF (F2)\n");
}

bool translation_is_enabled(void) {
    return s_translation_enabled;
}

const char* translation_reverse_lookup(const char *chinese) {
    if (!chinese) return nullptr;
    translation_wait_ready();
    if (s_reverse.empty()) return nullptr;
    auto it = s_reverse.find(chinese);
    if (it != s_reverse.end()) {
        return it->second.c_str();
    }
    return nullptr;
}
