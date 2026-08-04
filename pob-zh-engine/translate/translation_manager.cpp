/*
** translation_manager.cpp - JSON translation loader
**
** Loads translations from Data/Translate/poe1/{locale}/ directories.
** JSON format (flat):       { "entries": { "key": "value", ... } }
** JSON format (structured): { "entries": { "key": { "翻譯": "value", "色碼標籤": "^7", ... }, ... } }
** Additional config: meta.json, synonyms.json, item_metadata.json
**
** Provides O(1) lookup via std::unordered_map.
**
** Locale selection:
**   1. Environment variable POB_LOCALE (e.g. "zh-rTW")
**   2. If not set, reads Data/Translate.json and picks first entry
**   3. If nothing found, translation is disabled (passthrough)
*/

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fstream>
#include <sstream>
#include "translation_manager.h"

/* nlohmann/json for structured JSON translation loading */
#include <json.hpp>
using njson = nlohmann::json;

/* ========== Internal State ========== */

static std::unordered_map<std::string, std::string> s_translations;      /* English → Chinese */
static std::unordered_map<std::string, std::string> s_reverse;           /* Chinese → English (exact) */
static std::unordered_map<std::string, std::string> s_reverse_pattern;   /* Chinese pattern → English pattern (hashed) */
/* Which file supplied the entry that currently OWNS each reverse key. Both maps
** are last-wins across the eight dictionaries, so this is the only way to say
** which file shadowed which — see the trace notes below. Populated always (a few
** MB) because the answer is needed the moment something looks wrong, and a build
** flag would mean the diagnostic is missing exactly when it is wanted. */
static std::unordered_map<std::string, std::string> s_reverse_origin;
static std::unordered_map<std::string, std::string> s_reverse_pattern_origin;
static std::unordered_map<std::string, std::string> s_forward_pattern;   /* English pattern → Chinese pattern (hashed) */
static std::unordered_map<std::string, std::string> s_reverse_bases;    /* Chinese base type → English (from items.json base entries) */
static std::vector<std::pair<std::string, std::string>> s_term_glossary; /* sorted longest-first for substring replacement */
static std::unordered_map<std::string, std::string> s_lookup_cache;     /* forward lookup cache (input → result, empty = no match) */
static std::string s_locale;
static bool s_initialized = false;
static bool s_translation_enabled = true;

/* ========== Source dictionaries ==========
**
** The lookup map is flat and merged in meta.json load order, so the LAST file
** to define a key wins everywhere. That breaks whenever the same bare English
** word means different things depending on where POB read it: POB stores
** support gems under their short name ("Volatility", not "Volatility Support"),
** and passives.json — loaded later — redefines that key as the passive node of
** the same name, so the skill tab showed 易爆 instead of 易變輔助 (issue #3).
** 65 keys collide this way in poe1.
**
** Load order cannot fix it: whichever file wins, the other context is wrong.
** So POB tells us WHERE it read the string from (PobToolsSetSource, wrapped
** around the controls that draw gem names) and that file's own dictionary is
** consulted first. Nothing is marked by default, so unmarked call sites — the
** passive tree included — behave exactly as before.
*/
struct SourceDict {
    const char *name;   /* what Lua passes to PobToolsSetSource */
    const char *file;   /* the dictionary file backing it */
};
static const SourceDict kSourceDicts[] = {
    { "gems", "gems.json" },
};

static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> s_sources;
static const std::unordered_map<std::string, std::string> *s_active_source = nullptr;
static std::string s_active_source_name;

/* Results computed while a source is active may embed a source-specific
** translation (the pattern / comma-list / suffix paths all recurse), so they
** must never land in the shared cache — a later unmarked lookup of the same
** string would get the gem-context answer. Each source therefore caches into
** its own map. Entries are created once at load time, and unordered_map keeps
** references to existing elements valid, so these pointers stay good. */
static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> s_source_cache;
static std::unordered_map<std::string, std::string> *s_lookup_cache_active = &s_lookup_cache;
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
** See translation_manager.h. The only field that needs threading through the
** lookup helpers is the origin: which of the eight dictionary files supplied
** the entry that won. Without it "who shadowed whom" is unanswerable, and that
** question is the root of every mis-attribution the paste work has produced. */
static bool        s_trace_on = false;
static std::string s_trace_buf;
static const char *s_rev_rule = "";      /* branch that produced the last result */
static std::string s_rev_key;            /* dictionary key that matched, if any */
static std::string s_rev_file;           /* file that key's winning copy came from */

/* Set right before returning, so the wrapper can record which branch ran.
** Deliberately a comma expression rather than a statement macro: it wraps the
** returned value in place and keeps each edit to one line. */
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
/* Lines the game COMPOSES as "<prefix><an ordinary stat>" rather than storing
** whole. GGPK holds only the template, so the composed string is absent from
** every dictionary — translate it the same way it was built.
**
** POB uses two English prefixes for the same Chinese one, and which is correct
** depends on whether the line is the jewel's ENCHANT or one of its rolled
** modifiers (verified against POB's own export of the same item):
**     enchant  ->  "Added Small Passive Skills grant: "
**     modifier ->  "Added Small Passive Skills also grant: "
** The discriminator is the trailing " (enchant)", not the Chinese wording: GGG
** writes 小型 on the enchant and 小 on the modifier, but its own stat data uses
** both for both, so the wording cannot be trusted to tell them apart. */
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
** A pasted item is a sequence of dash-separated SECTIONS, and what a line means
** depends on which section it sits in — the same prose is flavour text on a
** unique and a modifier on a rare. Every rule below used to guess that from the
** line alone ("starts with a full-width paren", "nothing in here translated and
** there are no digits", a hardcoded list of property names), which is why each
** new copy format needed one more special case.
**
** Deliberately NOT split into implicit/explicit/enchant-as-separate-kinds: they
** drive identical behaviour here (POB derives the stage from the "{ ... Modifier }"
** annotations we rebuild), and a distinction no rule reads is one that can drift
** wrong without any test noticing. */
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

/* ========== Helper: read env via Win32 (NOT getenv) ==========
**
** The host exe uses a static CRT and passes POB_* variables with
** SetEnvironmentVariableW. The UCRT used by this DLL snapshots the
** environment when ucrtbase initialises — which can happen at process start
** (system DLLs), BEFORE the host sets anything — so getenv() may never see
** them. GetEnvironmentVariableA reads the live Win32 environment. */
const char* translation_win_env(const char *name, char *buf, unsigned size) {
    DWORD n = GetEnvironmentVariableA(name, buf, size);
    return (n > 0 && n < size) ? buf : nullptr;
}

static const char* win_env(const char *name, char *buf, unsigned size) {
    return translation_win_env(name, buf, size);
}

/* ========== Helper: Check if char is an ASCII letter ========== */

static inline bool is_ascii_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* ========== Helper: case-insensitive ASCII string comparison ========== */

static std::string to_ascii_lower(const std::string &s) {
    std::string r = s;
    for (auto &c : r) {
        if (c >= 'A' && c <= 'Z') c += 32;
    }
    return r;
}

/* ========== Helper: case-insensitive find (ASCII only) ========== */

static size_t find_icase(const std::string &haystack, const std::string &needle_lower, size_t start = 0) {
    if (needle_lower.size() > haystack.size()) return std::string::npos;
    for (size_t i = start; i <= haystack.size() - needle_lower.size(); i++) {
        bool match = true;
        for (size_t j = 0; j < needle_lower.size(); j++) {
            char hc = haystack[i + j];
            if (hc >= 'A' && hc <= 'Z') hc += 32;
            if (hc != needle_lower[j]) { match = false; break; }
        }
        if (match) return i;
    }
    return std::string::npos;
}

/* ========== Helper: Check if a key is suitable for term glossary ========== */

static bool is_glossary_candidate(const std::string &key) {
    if (key.size() < 4 || key.size() > 60) return false;
    /* Must contain at least one letter */
    bool has_letter = false;
    for (char c : key) {
        if (is_ascii_alpha(c)) has_letter = true;
        /* Reject entries with placeholders */
        if (c == '{') return false;
    }
    /* Must not start with ^ (color code) */
    if (!key.empty() && key[0] == '^') return false;
    return has_letter;
}

/* ========== Helper: Get exe directory ========== */

static std::string get_exe_directory() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *lastSlash = strrchr(path, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
    }
    return std::string(path);
}

/* ========== Helper: normalize whitespace (collapse, trim, convert fullwidth) ========== */

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

/* ========== Helper: replace digits with # for pattern matching ========== */

/* ========== Helper: check if position starts a (X-Y) range pattern ========== */

/* Returns length of the range if matched (including parens), or 0 if not a range.
 * Matches patterns like (4-6), (10-20), (1.5-3.0), and the NEGATIVE form the
 * game uses for penalties: Melding of the Flesh rolls "-72(-80--70)% to all
 * Elemental Resistances", where four minus signs run together. Requiring a digit
 * straight after '(' rejected those, so the range was never collapsed, the
 * hashed pattern came out malformed, and every negative roll on the item failed
 * to translate. */
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

/* A '+'/'-' sitting right before a number is part of that number and gets folded
** into its '#'. A '-' BETWEEN two numbers is a range separator ("5-10") and must
** be left alone, so only absorb one when what precedes it is not itself a number.
** Templates write "{0}" and let the value carry its own sign, which is why the
** signed form has to hash the same as the unsigned one. */
static bool absorb_sign(std::string &result) {
    if (result.empty()) return false;
    char back = result.back();
    if (back == '+') { result.pop_back(); return true; }
    if (back == '-' && (result.size() < 2 || result[result.size() - 2] != '#')) {
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

static std::string digits_to_hash(const std::string &s) {
    std::string result;
    result.reserve(s.size());
    bool in_number = false;
    for (size_t i = 0; i < s.size(); i++) {
        /* Check for (X-Y) range pattern — collapse entire range to single # */
        size_t range_len = match_range(s, i);
        if (range_len > 0) {
            if (in_number) {
                /* Roll range attached to a value, e.g. "7(6-11)" from the
                 * game's advanced item copy — absorb into the preceding #
                 * so "+7(6-11) X" hashes the same as "+7 X". */
                i += range_len - 1;
                in_number = false;
                continue;
            }
            /* Absorb preceding sign, same as for plain numbers */
            absorb_sign(result);
            result += '#';
            i += range_len - 1; /* -1 because loop increments */
            in_number = false;
            continue;
        }
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (!in_number) {
                /* Absorb a preceding sign into the # placeholder.
                 * POB stat descriptions use {0:+d} format which outputs "+20",
                 * but translation templates use {0} which normalizes to just "#".
                 * By consuming the sign, "+20 to Intelligence" → "# to Intelligence"
                 * matches pattern "# to Intelligence"; likewise "-2" so that
                 * "此物品插槽中輔助寶石等級 -2" matches the same key "+2" does. */
                absorb_sign(result);
                result += '#';
                in_number = true;
            }
            /* skip digit */
        } else if (c == '.' && in_number && i + 1 < s.size() && s[i+1] >= '0' && s[i+1] <= '9') {
            /* decimal point inside number, skip */
        } else if (c == ',' && in_number && i + 1 < s.size() && s[i+1] >= '0' && s[i+1] <= '9') {
            /* thousands separator inside number (e.g., "7,881"), skip */
        } else {
            in_number = false;
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

/* ========== Helper: replace # placeholders with numbers ========== */

static std::string fill_numbers(const std::string &pattern, const std::vector<std::string> &nums) {
    std::string result;
    size_t ni = 0;
    for (char c : pattern) {
        if (c == '#' && ni < nums.size()) {
            result += nums[ni++];
        } else {
            result += c;
        }
    }
    return result;
}

/* ========== Helper: normalize {0},{1},... placeholders to # ========== */

static std::string normalize_placeholders(const std::string &s) {
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
                    i = j; /* skip past } */
                    continue;
                }
                /* GGG's format specs — {0:+d}, {0:.2f} — are the same placeholder
                ** as {0}, but were left literal here, so every template carrying
                ** one hashed to garbage ("{#:+d}") and could never match. The
                ** Chinese side of "{0}% Chance to Block Attack Damage during
                ** Effect" is one of these. Bounded so a stray '{' cannot scan on. */
                if (s[j] == ':') {
                    size_t k = s.find('}', j);
                    if (k != std::string::npos && k - j <= 8) {
                        result += '#';
                        i = k; /* skip past } */
                        continue;
                    }
                }
            }
        }
        result += s[i];
    }
    return result;
}

/* ========== Helper: replace all occurrences of a substring ========== */

static void replace_all(std::string &str, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.size(), to);
        pos += to.size();
    }
}

/* ========== Helper: normalize Chinese synonyms for fuzzy matching ========== */

static std::string normalize_chinese_synonyms(const std::string &s) {
    std::string result = s;

    /* Apply dynamic synonym rules from synonyms.json */

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

/* ========== Helper: strip [X|Y] bracket markup → Y (display part only) ========== */

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

/* ========== Helper: determine locale ========== */

static std::string determine_locale() {
    /* 1. Check environment variable */
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

static int load_json_translations(const std::string &filepath, bool is_base_items,
                                  std::unordered_map<std::string, std::string> *sink = nullptr) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return 0;

    /* bare file name, e.g. "stats.json" — recorded as the origin of every key
    ** this call wins, so the trace can name who shadowed whom */
    std::string origin = filepath;
    {
        size_t slash = origin.find_last_of("\\/");
        if (slash != std::string::npos) origin = origin.substr(slash + 1);
    }

    njson doc;
    try {
        doc = njson::parse(file);
    } catch (const njson::parse_error &e) {
        char msg[512];
        snprintf(msg, sizeof(msg), "[pob-proxy] JSON parse error in %s: %s\n",
                 filepath.c_str(), e.what());
        OutputDebugStringA(msg);
        return 0;
    }

    if (!doc.contains("entries") || !doc["entries"].is_object()) return 0;

    /* Check is_base_items flag from JSON (overrides parameter if present) */
    if (doc.contains("is_base_items") && doc["is_base_items"].is_boolean()) {
        is_base_items = doc["is_base_items"].get<bool>();
    }

    /* Entries GGG ships as ONE string containing its own "\n" (497 of them).
    ** The game renders those as separate lines and a pasted item presents them
    ** as separate lines, so the joined form — which is the dictionary key —
    ** matches neither half. The Red Nightmare's
    **     "Passives granting Fire Resistance or all Elemental Resistances in
    **      Radius\nalso grant Chance to Block Attack Damage at {0}% of its value"
    ** arrived as two lines and the first one had no translation at all.
    **
    ** Registered per line IN ADDITION to the joined form, and only when the two
    ** sides line up: same number of lines, and the same placeholder/number count
    ** on each. GGG's "\n" in a stat description is a hard break at the same point
    ** in both languages, but that is a claim about the data, so it is checked
    ** rather than assumed — a mismatch means the break moved and pairing line i
    ** with line i would be exactly the positional guessing this project bans.
    ** Existing entries are never overwritten: a purpose-built single-line entry
    ** always beats one derived by splitting. */
    /* Two spellings of the same break live in the shipped files: stats.json
    ** carries GGG's newline as the literal two characters '\' 'n', ui.json as a
    ** real 0x0A. Both mean "the game renders this as two lines", so both split. */
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
            /* UTF-8: 翻譯=E7 BF BB E8 AD AF, 色碼標籤=E8 89 B2 E7 A2 BC E6 A8 99 E7 B1 A4 */
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

        /* Apply color prefix: prepend to both key and value for lookup matching.
         * POB sends "^x7F7F7FArmour:" and expects "^x7F7F7F護甲:" back. */
        std::string full_key = color_prefix.empty() ? key : (color_prefix + key);
        std::string full_value = color_prefix.empty() ? value : (color_prefix + value);

        /* Duplicate key detection */
        auto existing = s_translations.find(full_key);
        if (existing != s_translations.end() && existing->second != full_value) {
            char msg[512];
            snprintf(msg, sizeof(msg), "[pob-proxy] JSON duplicate key: \"%.80s\" old=\"%.60s\" new=\"%.60s\" in %s\n",
                     full_key.c_str(), existing->second.c_str(), full_value.c_str(), filepath.c_str());
            OutputDebugStringA(msg);
        }

        std::string clean_key = strip_brackets(full_key);
        std::string clean_value = strip_brackets(full_value);

        s_translations[full_key] = full_value;
        if (sink) (*sink)[full_key] = full_value;
        s_reverse[full_value] = full_key;
        s_reverse_origin[full_value] = origin;
        if (clean_value != full_value) {
            s_reverse[clean_value] = clean_key;
            s_reverse_origin[clean_value] = origin;
        }

        if (is_base_items) {
            s_reverse_bases[clean_value] = clean_key;
            if (clean_value != full_value) {
                s_reverse_bases[full_value] = full_key;
            }
            std::string variant = clean_value;
            replace_all(variant, "\xe5\xb7\x96", "\xe5\xb2\xa9"); /* 巖→岩 */
            if (variant != clean_value) {
                s_reverse_bases[variant] = clean_key;
            }
        }

        /* Build pattern maps (use clean keys without color prefix for pattern matching) */
        std::string norm_value = normalize_whitespace(digits_to_hash(normalize_placeholders(clean_value)));
        std::string norm_key = normalize_whitespace(digits_to_hash(normalize_placeholders(clean_key)));
        if (norm_value != clean_value || norm_key != clean_key) {
            s_reverse_pattern[norm_value] = norm_key;
            s_reverse_pattern_origin[norm_value] = origin;
            s_forward_pattern[to_ascii_lower(norm_key)] = norm_value;
        }
        std::string fuzzy_value = normalize_chinese_synonyms(norm_value);
        if (fuzzy_value != norm_value) {
            s_reverse_pattern[fuzzy_value] = norm_key;
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
            }
            if (aligned) {
                for (size_t i = 0; i < klines.size(); i++) {
                    const std::string &k = klines[i], &v = vlines[i];
                    if (!s_translations.count(k)) {
                        s_translations[k] = v;
                        if (sink) (*sink)[k] = v;
                    }
                    if (!s_reverse.count(v)) {
                        s_reverse[v] = k;
                        s_reverse_origin[v] = origin;
                    }
                    std::string nv = normalize_whitespace(digits_to_hash(normalize_placeholders(v)));
                    std::string nk = normalize_whitespace(digits_to_hash(normalize_placeholders(k)));
                    if (nv != v || nk != k) {
                        if (!s_reverse_pattern.count(nv)) {
                            s_reverse_pattern[nv] = nk;
                            s_reverse_pattern_origin[nv] = origin;
                        }
                        std::string lk = to_ascii_lower(nk);
                        if (!s_forward_pattern.count(lk)) s_forward_pattern[lk] = nv;
                    }
                }
            }
        }
        count++;
    }

    return count;
}

static bool load_json_meta(const std::string &filepath,
                           std::vector<std::string> &load_order) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;

    njson doc;
    try {
        doc = njson::parse(file);
    } catch (...) {
        return false;
    }

    if (doc.contains("load_order") && doc["load_order"].is_array()) {
        for (auto &item : doc["load_order"]) {
            if (item.is_string())
                load_order.push_back(item.get<std::string>());
        }
    }

    if (doc.contains("incomplete_translation_whitelist") &&
        doc["incomplete_translation_whitelist"].is_array()) {
        for (auto &item : doc["incomplete_translation_whitelist"]) {
            if (item.is_string())
                s_whitelist_terms.push_back(item.get<std::string>());
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

    /* The annotation scan consumes matches, so a marker that is a prefix of a
    ** longer one must never be tried first. The generator already emits them in
    ** order; re-sorting here means a hand-edited file cannot break it silently. */
    std::stable_sort(s_mod_annotations_vec.begin(), s_mod_annotations_vec.end(),
                     [](const HeaderMapping_Dyn &a, const HeaderMapping_Dyn &b) {
                         return a.chinese.size() > b.chinese.size();
                     });

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

/* Try to load JSON translation files. Returns true if JSON mode activated. */
static bool try_load_json(const std::string &base_dir) {
    /* Build locale-aware JSON directory path.
     * Layout: Data/{game}/{locale}/ — poe1 and poe2 dictionaries live in
     * separate folders directly under Data.
     * POB_LOCALE "" or "poe1" defaults to "zh-rTW".
     */
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

    /* POB_ZH_DATADIR: the dictionary folder for THIS game, kept outside the
     * install so a translator can edit a working copy and see it rendered by the
     * real POB -- and so updates, which only ever write to the install directory,
     * cannot overwrite it.
     *
     * It points at the folder that directly contains the <locale> sub-folders
     * (exactly what you get by copying Data\poe1 somewhere), NOT at a Data root:
     * the launcher already knows which game it is starting, so it picks the right
     * folder and this code never has to know about games at all.
     *
     * The launcher validates the folder and only sets the variable when it is
     * usable; the fallback below is for a hand-set variable pointing somewhere
     * wrong, where shipping no translations would be far worse than ignoring it.
     *
     * Note the whole dictionary path is ANSI (GetEnvironmentVariableA here,
     * std::ifstream below), exactly as it already is for the install directory,
     * so the folder has to be spellable in the system codepage. */
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
    if (!load_json_meta(meta_path, load_order)) {
        OutputDebugStringA("[pob-proxy] Failed to parse meta.json\n");
        return false;
    }

    /* Load synonyms.json */
    load_json_synonyms(json_dir + "synonyms.json");

    /* Load item_metadata.json */
    load_json_item_metadata(json_dir + "item_metadata.json");

    /* Load translation files in order */
    int totalEntries = 0;
    int fileCount = 0;
    for (auto &filename : load_order) {
        std::string filepath = json_dir + filename;
        /* Files backing a source dictionary get a second copy of their own
         * entries, untouched by whatever later files redefine. */
        std::unordered_map<std::string, std::string> *sink = nullptr;
        for (const SourceDict &sd : kSourceDicts) {
            if (filename == sd.file) {
                sink = &s_sources[sd.name];
                s_source_cache[sd.name];   /* create now: never rehash later */
                break;
            }
        }
        int count = load_json_translations(filepath, false, sink);
        if (count > 0) {
            snprintf(msg, sizeof(msg), "[pob-proxy] Loaded %d entries from %s\n",
                     count, filename.c_str());
            OutputDebugStringA(msg);
            totalEntries += count;
            fileCount++;
        }
    }

    if (totalEntries == 0) {
        OutputDebugStringA("[pob-proxy] No JSON entries loaded\n");
        return false;
    }

    snprintf(msg, sizeof(msg), "[pob-proxy] JSON mode: loaded %d entries from %d files\n",
             totalEntries, fileCount);
    OutputDebugStringA(msg);

    return true;
}

/* ========== Public API ========== */

void translation_init(void) {
    if (s_initialized) return;
    s_initialized = true;

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
    /* Build term glossary for substring replacement (sorted longest-first).
     * Store the LOWERCASE version of each term's key for case-insensitive matching,
     * but keep the original value (Chinese translation) as-is. */
    for (auto &kv : s_translations) {
        if (is_glossary_candidate(kv.first)) {
            /* Strip trailing colon for glossary matching (e.g., "Total Mana:" → "Total Mana") */
            std::string term = kv.first;
            std::string trans = kv.second;
            if (!term.empty() && term.back() == ':') {
                term.pop_back();
                if (!trans.empty() && (trans.back() == ':' || trans.back() == '\xef')) {
                    /* Strip Chinese colon (：= EF BC 9A) or ASCII colon */
                    if (trans.size() >= 3 && trans[trans.size()-3] == '\xef'
                        && trans[trans.size()-2] == '\xbc' && trans[trans.size()-1] == '\x9a') {
                        trans.erase(trans.size()-3);
                    } else if (trans.back() == ':') {
                        trans.pop_back();
                    }
                }
            }
            if (term.size() >= 4) {
                /* Store lowercase key for case-insensitive matching */
                std::string term_lower = to_ascii_lower(term);
                s_term_glossary.emplace_back(std::move(term_lower), std::move(trans));
            }
        }
    }
    /* Sort by length descending so longest terms match first */
    std::sort(s_term_glossary.begin(), s_term_glossary.end(),
        [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });

    snprintf(msg, sizeof(msg), "[pob-proxy] Loaded %d translations (JSON mode, %d pattern, %d glossary, %d base types)\n",
             (int)s_translations.size(),
             (int)s_reverse_pattern.size(), (int)s_term_glossary.size(), (int)s_reverse_bases.size());
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
                         test_names[i], (int)(it->second.size() > 80 ? 80 : it->second.size()),
                         it->second.c_str());
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
    s_translations.clear();
    s_reverse.clear();
    s_reverse_pattern.clear();
    s_reverse_bases.clear();
    s_forward_pattern.clear();
    s_term_glossary.clear();
    s_lookup_cache.clear();
    /* Drop the source dictionaries too, and point the active handles back at
     * the defaults — a reload must not leave them dangling into freed maps. */
    s_active_source = nullptr;
    s_active_source_name.clear();
    s_lookup_cache_active = &s_lookup_cache;
    s_sources.clear();
    s_source_cache.clear();
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
    s_reverse_origin.clear();
    s_reverse_pattern_origin.clear();
    s_synonym_rules.clear();
    s_strip_punctuation.clear();
    s_strip_space_around.clear();
    s_strip_measure_words.clear();
    s_whitelist_terms.clear();
    s_initialized = false;
    OutputDebugStringA("[pob-proxy] Translation manager shutdown\n");
}

void translation_reload(void) {
    translation_shutdown();
    translation_init();
    OutputDebugStringA("[pob-proxy] Translation reloaded (F3)\n");
}

/* ========== Dev helper: log untranslated strings ==========
**
** Writes every unique untranslated string to <exe dir>\translate_misses.log
** so the dictionaries can be tuned. Truncated on each run (the file always
** holds the CURRENT session). Disable with environment POB_TR_LOG=0.
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
    /* Flush periodically rather than per line: a text-heavy view (the About
     * popup's changelog/help) can log hundreds of misses in one frame, and a
     * per-line fflush turns that into hundreds of blocking disk writes on the
     * render thread. The editor reads this log after POB exits, by which point
     * the CRT has flushed the remainder. */
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

const char* translation_lookup(const char *english) {
    if (!english || s_translations.empty() || !s_translation_enabled) return nullptr;

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

    /* 0. Source dictionary: POB told us which of its own data files this
     *    string came from, so that file's entries win over the merged map.
     *    A miss here just falls through to the normal path. */
    if (s_active_source) {
        auto sit = s_active_source->find(english);
        if (sit != s_active_source->end()) {
            return sit->second.c_str();
        }
    }

    /* 1. Exact match (fast path, no cache needed — already O(1)) */
    auto it = s_translations.find(english);
    if (it != s_translations.end()) {
        return it->second.c_str();
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
    std::string pattern = to_ascii_lower(digits_to_hash(normalize_placeholders(input)));
    /* The second clause is not redundant. POB's own crafting UI writes a mod's
    ** rollable value as a literal '#' ("#% increased Armour during Effect" in
    ** the affix rows), which is ALREADY the hashed form — so hashing changes
    ** nothing, the first clause is false, and the pattern map was skipped
    ** entirely. Those rows stayed English while the same mod translated fine in
    ** the tooltip, where it carries a real number. */
    if ((pattern != to_ascii_lower(input) || input.find('#') != std::string::npos)
        && !s_forward_pattern.empty()) {
        auto pit = s_forward_pattern.find(pattern);
        if (pit != s_forward_pattern.end()) {
            auto nums = extract_numbers(input);
            std::string result = fill_numbers(pit->second, nums);
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
            auto [iter, _] = s_lookup_cache_active->emplace(input, std::move(result));
            return iter->second.c_str();
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

    /* 3.4 Colour-code retry: POB glues colour escapes straight onto words
     *     ("^x7070FFKinetic Blast ^720/20", "Full ^xE05030Life?"), which
     *     defeats every matcher above and breaks glossary word boundaries.
     *     Strip the codes and run the whole pipeline on the clean text. */
    if (input.find('^') != std::string::npos) {
        std::string stripped = strip_color_codes(input);
        if (stripped != input && !stripped.empty()) {
            if (const char *r = translation_lookup(stripped.c_str())) {
                std::string copy(r); /* copy before emplace can rehash the cache */
                auto [iter, _] = s_lookup_cache_active->emplace(input, std::move(copy));
                return iter->second.c_str();
            }
            /* clean text also missed: fall through with the original input */
        }
    }

    /* 3.5 Trailing parenthetical note: POB appends notes such as
     *     " (Not Supported in PoB yet)" to otherwise-translatable lines,
     *     which defeats the exact and pattern matches above. Split the note
     *     off, translate the body recursively, translate the note when the
     *     dictionary has it, and stitch the two back together. */
    if (input.size() > 4 && input.back() == ')') {
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
                    const char *suffix_zh = translation_lookup(suffix.c_str());
                    combined += " (";
                    combined += suffix_zh ? suffix_zh : suffix.c_str();
                    combined += ")";
                    auto [iter, _] = s_lookup_cache_active->emplace(input, std::move(combined));
                    return iter->second.c_str();
                }
            }
        }
    }

    /* 3.7 Comma-joined lists: POB builds strings like
     *     "Arcane Cloak, Automation, Sigil of Power" or
     *     "4 Power Charges, 4 Endurance Charges, Onslaught" at runtime.
     *     Translate each segment through the full pipeline; only join the
     *     result when EVERY segment translates (consistency rule: a line is
     *     either fully translated or left fully in English). */
    if (input.find(", ") != std::string::npos) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            size_t sep = input.find(", ", start);
            if (sep == std::string::npos) { parts.push_back(input.substr(start)); break; }
            parts.push_back(input.substr(start, sep - start));
            start = sep + 2;
        }
        bool all_ok = parts.size() >= 2;
        std::string joined;
        for (auto &p : parts) {
            if (p.empty()) { all_ok = false; break; }
            const char *r = translation_lookup(p.c_str());
            if (!r) { all_ok = false; break; }
            if (!joined.empty()) joined += ", ";
            joined += r; /* safe: r points into caches, appended before next lookup */
        }
        if (all_ok) {
            auto [iter, _] = s_lookup_cache_active->emplace(input, std::move(joined));
            return iter->second.c_str();
        }
    }

    /* 4. Term glossary: substring replacement for composite stat strings,
     *    e.g., "+5 Total Mana (+10.0%)" → "+5 最大魔力 (+10.0%)".
     *    Only applied when the glossary (plus the meta.json whitelist of
     *    terms allowed to stay in English, e.g. "DPS") covers EVERY letter
     *    of the input. Partially covered sentences — flavour text, unknown
     *    mods — are left fully in English: a mixed-language line is worse
     *    than an untranslated one. */
    if (input.size() >= 4 && !s_term_glossary.empty() && input.find("Community Fork") == std::string::npos) {
        struct Span { size_t pos, len; const std::string *to; };
        std::vector<Span> spans;     /* glossary hits, to be replaced */
        std::vector<Span> allowed;   /* whitelist hits, kept as-is */

        auto overlaps_any = [](const std::vector<Span> &v, size_t pos, size_t len) {
            for (auto &s : v) {
                if (pos < s.pos + s.len && s.pos < pos + len) return true;
            }
            return false;
        };
        auto boundary_ok = [&input](size_t pos, size_t len) {
            if (pos > 0 && is_ascii_alpha(input[pos - 1])) return false;
            size_t end = pos + len;
            if (end < input.size() && is_ascii_alpha(input[end])) return false;
            return true;
        };

        /* Index input positions by lowercased byte. Every occurrence of a term
         * must begin where the term's (already-lowercased) first char appears,
         * so each term only probes those positions instead of find_icase
         * rescanning the whole string for all ~thousands of glossary terms.
         * Same matches as before, but far cheaper on long prose — the About
         * popup renders the multi-thousand-line changelog/help text, and the
         * old full scan froze the render thread for seconds on first open. */
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
                auto [iter, _] = s_lookup_cache_active->emplace(input, std::move(result));
                return iter->second.c_str();
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

/* Select the source dictionary consulted before the merged map, or pass null /
** "" to go back to the plain merged lookup. Returns the PREVIOUS selection so
** the caller can restore it, which is what makes nesting safe.
**
** An unknown name deliberately behaves as "no source" rather than failing:
** a dictionary file may be missing from an older data pack, and the app must
** still render — just without the disambiguation. */
const char* translation_set_source(const char *name) {
    static std::string prev_holder;
    /* Copy the argument BEFORE touching prev_holder. Restoring a nested scope
    ** means passing back the pointer this function returned last time, which
    ** aliases prev_holder — assigning to it first would rewrite the argument
    ** out from under us. */
    const std::string want = (name && *name) ? std::string(name) : std::string();
    prev_holder = s_active_source_name;

    s_active_source = nullptr;
    s_active_source_name.clear();
    s_lookup_cache_active = &s_lookup_cache;

    if (!want.empty()) {
        auto it = s_sources.find(want);
        if (it != s_sources.end()) {
            s_active_source = &it->second;
            s_active_source_name = want;
            auto cit = s_source_cache.find(s_active_source_name);
            if (cit != s_source_cache.end()) s_lookup_cache_active = &cit->second;
        }
    }
    return prev_holder.empty() ? nullptr : prev_holder.c_str();
}

const char* translation_get_source(void) {
    return s_active_source_name.empty() ? nullptr : s_active_source_name.c_str();
}

const char* translation_get_locale(void) {
    return s_locale.c_str();
}

int translation_get_count(void) {
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

static bool try_header_translate(const std::string &line, std::string &out) {
    /* Check for "key: value" pattern */
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;

    std::string key_part = line.substr(0, colon);
    std::string rest = line.substr(colon); /* includes ": value" */

    /* Properties POB has no field for used to be named here, one at a time. That
    ** list is gone: an unknown property key is now dropped by the section grammar
    ** (reverse_one_line_impl step 7b), which needs no list to maintain. */

    /* Look up key in header mappings */
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
            const char *val_eng = find_header_mapping(value_part, hv->second);
            if (val_eng) {
                out = std::string(eng) + ": " + val_eng;
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
        /* Generic header: replace key, keep value as-is */
        out = std::string(eng) + rest;
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
    return line;
}

/* ========== Helper: rebuild a 3.29 advanced-copy annotation line ==========
**
** The game writes, inside braces, [wrapper] core "affix name"(tier)— tags:
**     { 已大師工藝 前綴 "進化的"(階級：3)— 防禦,護甲,能量護盾 }
** POB reads ONLY the words before the literal "Modifier" and looks each one up
** in Item.lua's lineFlags table (Classes/Item.lua:495), so the whole line
** collapses to its flag words:
**     { Master Crafted Prefix Modifier }
** That restores implicit / fractured / crafted / exarch / eater / prefix /
** suffix, which decide whether a mod is counted as an implicit at all.
**
** Deliberately dropped:
**   - the quoted affix name. There is no Chinese→English affix-name dictionary;
**     leaving Chinese in makes POB match no affix and mark the mod {custom},
**     which is worse than saying nothing.
**   - the tier and the tags. Both only matter once an affix name has matched.
**
** Composition (marker strings, the em-dash separator and the differing spacing
** between the English and Chinese forms) all come from GGPK clientstrings —
** ModDescriptionLine* and AdvancedModDisplayTags. See gen_item_metadata.py. */
static bool rebuild_mod_annotation(const std::string &line, std::string &out) {
    if (line.size() < 3 || line[0] != '{' || line.back() != '}') return false;
    std::string body = line.substr(1, line.size() - 2);

    /* Cut the tag list. AdvancedModDisplayTags is " — {0}" in English but
    ** "— {0}" in Chinese, so anchor on the em-dash itself, not the spacing. */
    static const char kEmDash[] = "\xe2\x80\x94";  /* — U+2014 */
    size_t dash = body.find(kEmDash);
    if (dash != std::string::npos) body.erase(dash);

    /* The quoted affix name, kept before the cut. POB reads it back out —
    **     Item.lua:463  local modName = fullModName:match("^.*Modifier \"(.*)\"")
    ** — and uses it to fill the item's prefix/suffix slots. Emitting the flags
    ** alone leaves those slots empty, and on a CLUSTER JEWEL that erases the
    ** item: a "{ " line sets self.crafted (Item.lua:455), a crafted cluster
    ** jewel makes ItemsTab call CraftClusterJewel (ItemsTab.lua:1858), and that
    ** rebuilds the whole jewel FROM the affix dropdowns — so empty dropdowns
    ** throw away every notable the jewel actually rolled.
    **
    ** One English name covering many modifiers is fine: POB narrows down within
    ** a name by matching the stat line that follows (Item.lua:871-889), and all
    ** 284 cluster-jewel notables share the single name "Notable". */
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
    /* Only names with ONE English counterpart that POB actually has an affix for
    ** are in the map (the generator drops the rest and writes the full excluded
    ** list to docs/notes). A name we cannot resolve is simply left off, which is
    ** exactly the behaviour that shipped before this. */
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
** Anchors are the ones POB itself uses (Item.lua switches parser stage on the
** Item Level line), and every test is data-driven — a header key is recognised
** by translating it, not by matching Chinese spelled into this file.
**
** Section ORDER is not an anchor: across the nine real items this was built
** from, sections range 6..8 and sockets/requirements/influence appear or not.
** Anything the grammar cannot place stays Unknown, and Unknown means "do
** exactly what the old per-line code did" — that is the whole safety story. */

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

    /* Advanced copy annotates every modifier section. Without those annotations
     * (plain Ctrl+C) there is no structural difference between a mod section and
     * flavour text, so the positional flavour rule below stays switched off and
     * the old content test in translation_reverse_text keeps the job. */
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

    /* 5. The prose that trails the last section carrying mechanics. Two things
     *    live there and they need telling apart:
     *
     *    - the item class instruction ("放置到一個天賦樹的珠寶插槽中…"), which is
     *      boilerplate GGG ships for every jewel and the dictionary therefore has;
     *    - a unique's flavour text, which is per-item and is not in any dictionary.
     *
     *    So the discriminator is "does the dictionary know this whole line", not
     *    position and not a keyword list. That matters because the keyword list
     *    only ever held 點擊右鍵 / 點擊左鍵, and GGG writes 右鍵點擊 on jewels —
     *    which is why the same sentence was dropped on a flask and passed through
     *    on a jewel. Both are dropped now: neither feeds a calculation, and left
     *    in place POB renders them as a modifier it could not parse.
     *
     *    Anything that is neither stays Unknown, so a mod section that somehow
     *    reached here still takes the old path and stays visible. */
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

static std::string reverse_one_line_impl(const std::string &line, LineKind kind) {
    if (line.empty()) return REV("empty", line);

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

    /* 0a. Sections the grammar placed as carrying no mechanics. Both used to be
     * decided per line: flavour by "nothing in this section translated and no
     * digits anywhere", description by a two-entry keyword list. Position is the
     * better witness — it holds for flavour that contains a number and for a
     * description whose second sentence has no keyword in it. */
    if (kind == LineKind::Flavour) {
        log_translation_miss("FLAVOUR ", line);
        return REV("flavour", "");
    }
    if (kind == LineKind::Description) return REV("description", "");

    /* 0b. Advanced-copy affix annotation lines, e.g.
     *   { 前綴 "微光的"(階層：10)— 防禦,能量護盾 }
     * Rebuild them into the flag-only English form POB parses, so implicit /
     * fractured / crafted / exarch survive. Anything unrecognised is stripped
     * instead: left in Chinese it would reach POB as '?' bytes, and a stripped
     * line still parses because the mod carries its own roll range. */
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
                            result = fill_numbers(fpit->second, {eng_name});
                        } else {
                            /* Fuzzy try */
                            std::string fuzzy_fp = normalize_chinese_synonyms(full_pattern);
                            fpit = s_reverse_pattern.find(fuzzy_fp);
                            if (fpit != s_reverse_pattern.end()) {
                                result = fill_numbers(fpit->second, {eng_name});
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

    /* 3c. Composed lines. GGG builds these by substitution — clientstrings
     * StatDescripotionTreeExpansionJewelGrantedSmallStat is "Added Small Passive
     * Skills grant: {0}" / "附加的小型天賦給予：{0}" — so the composed string exists
     * in no dictionary. Translate it the same way it was built: strip the prefix,
     * translate the remainder as the ordinary stat it is, put the English prefix
     * back.
     *
     * This runs BEFORE the direct lookups, and that ordering is the fix, not an
     * accident. The dictionary does hold some of these strings, but it holds the
     * same Chinese under BOTH "grant:" and "also grant:", so the merged map hands
     * back whichever file was loaded last — and for a rolled modifier that answer
     * is wrong half the time. Composing from the prefix decides it on evidence
     * the line actually carries.
     *
     * None of this is cosmetic: POB reads the cluster jewel's SKILL from the
     * enchant line (Data/ClusterJewels.lua) via an exact whole-line lookup
     * (Modules/ModParser.lua:6463). Get the line wrong and POB rebuilds the whole
     * jewel around the wrong skill, discarding every notable it actually rolled. */
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

                /* Every number the inner carried has to survive. Without this the
                 * rule will happily wrap a WRONG inner match — the census caught
                 * "附加的小天賦給予：#% 最大能量護盾" coming back as "...also grant:
                 * you and nearby Allies", turning a line that used to fail
                 * visibly into confident nonsense. A translation that dropped the
                 * value was not a translation of this line. */
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
        std::vector<std::string> nums = extract_numbers(core);
        std::string result = fill_numbers(pit->second, nums) + suffix;
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
                std::vector<std::string> nums = extract_numbers(core);
                std::string result = fill_numbers(pit->second, nums) + suffix;
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
                std::vector<std::string> nums = extract_numbers(core);
                std::string result = sign_prefix + fill_numbers(pit->second, nums) + suffix;
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
                    std::vector<std::string> nums = extract_numbers(core);
                    std::string result = sign_prefix + fill_numbers(pit->second, nums) + suffix;
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

    /* 7b. A "key: value" line in the property block that nothing above could
     * translate. Leaving it is the worst option available: POB sanitises the
     * Chinese to '?' bytes, and an unknown property still satisfies its property
     * regex, so Item.lua:704 flips the parser into the EXPLICIT stage early and
     * every implicit after it is misclassified. Dropping costs nothing — POB
     * recomputes every property from base type plus modifiers.
     *
     * This replaces the hardcoded drop list (which held exactly one entry, 無形性,
     * the Allflame crafting percentage). The list only ever named the symptom;
     * the rule is "unknown property", and it covers whatever GGG adds next.
     * Logged, because a property we cannot translate is still a gap. */
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
    if (!chinese_text || s_reverse.empty()) return nullptr;

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
                    /* Magic/Normal: name contains base type (e.g. 鷹眼的石英藥劑)
                     * POB needs the English base type in the name to identify the item.
                     * Search s_reverse_bases (base item entries) for the longest substring match.
                     * This avoids false positives from affix names in s_reverse. */
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

    /* Unique flavour text, FALLBACK path. POB has no branch for it, so a pasted
    ** flavour line is parsed as a modifier -- and in Chinese it arrives as '?'
    ** bytes, because Item.lua runs everything through sanitiseText. Nothing is
    ** lost by dropping it: POB renders flavour text from its own Data/FlavourText.
    **
    ** The section grammar already dropped flavour it could place by position.
    ** What is left is plain Ctrl+C text, where no annotation marks the modifier
    ** sections and position proves nothing — so the old content test still has to
    ** do the work: only a section where NOTHING translated and no line carries a
    ** number qualifies. Sections the grammar DID label are skipped, so the
    ** heuristic can no longer overrule it. */
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
    s_translation_enabled = enabled;
    s_lookup_cache.clear();
    for (auto &kv : s_source_cache) kv.second.clear();
    OutputDebugStringA(enabled
        ? "[pob-proxy] Translation ON (F2)\n"
        : "[pob-proxy] Translation OFF (F2)\n");
}

bool translation_is_enabled(void) {
    return s_translation_enabled;
}

const char* translation_reverse_lookup(const char *chinese) {
    if (!chinese || s_reverse.empty()) return nullptr;
    auto it = s_reverse.find(chinese);
    if (it != s_reverse.end()) {
        return it->second.c_str();
    }
    return nullptr;
}
