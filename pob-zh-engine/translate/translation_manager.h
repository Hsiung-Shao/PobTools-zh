/*
** translation_manager.h - JSON translation loader + O(1) lookup
**
** Loads translation JSON files from Data/Translate/poe1/{locale}/
** Flat format:       { "entries": { "key": "value", ... } }
** Structured format: { "entries": { "key": { "翻譯": "value", ... }, ... } }
** Provides O(1) string lookup via unordered_map.
*/
#ifndef TRANSLATION_MANAGER_H
#define TRANSLATION_MANAGER_H

/* Initialize translation system: load JSON files based on config */
void translation_init(void);

/* 背景載入:在 InitAPI 啟一條執行緒建字典,POB 的 Lua 不必等它。任何會讀字典的
** 公開 API 開頭都會 translation_wait_ready(),所以從呼叫端看語意與同步版相同,
** 只是第一次呼叫可能阻塞到載入完成。host 的自檢仍用同步的 translation_init()。
** 不 wait 的:is_enabled / win_env / trace_* / free / strip_ggg_markup / debug_*
**(它們不讀字典;win_env 在 RenderInit 期被 r_font 呼叫,加了就把載入串回
** 關鍵路徑)。 */
void translation_init_async(void);
void translation_wait_ready(void);

/* Shutdown: free all translation data */
void translation_shutdown(void);

/* Reload: shutdown + re-init (used by F3 hotkey) */
void translation_reload(void);

/* Look up translation for a given English string.
** Returns translated string if found, or nullptr if no translation.
** Returned pointer is valid until translation_shutdown(), except the answer of
** a "/"-joined description (its cache is capped and emptied when full): that
** one only until the next lookup. Callers copy the result before looking up
** again, which covers both. */
const char* translation_lookup(const char *english);

/* Look up a string the caller has already established is a generated ITEM TITLE
** (GGG's "<prefix> <suffix>" word pair, or a unique's name). Tries the normal
** pipeline first, then falls back to translating word by word.
**
** Why a separate entry point: the per-word fallback ignores the glossary's
** four-character floor, which exists to keep short function words out of prose.
** translation_lookup() can only relax it when the string carries its own proof
** of being an item name -- the "<title>, <base type>" form the item lists use.
** The tooltip draws the title on a line of its own (ItemsTab.lua:4388), so that
** proof is gone and only the CALL SITE knows. This function is that knowledge.
**
** Returns nullptr when the title does not fully translate; callers must then
** render the English, never a half-translated mix. */
const char* translation_lookup_item_title(const char *english);

/* Select the dictionary file consulted BEFORE the merged map, naming the POB
** data file the string being drawn came from ("gems", "passives", "affixes",
** "stats", "uniques" or "items" -- see kSourceDicts). Its exact entries AND its
** own value patterns are consulted before the merged ones. Pass
** nullptr or "" to clear. Returns the previous selection (nullptr if none) so
** callers can restore it; an unknown name clears instead of failing.
** Without meta.json "source_lookup" only "gems" (exact entries) can be
** selected -- the behaviour from before the other sources existed.
**
** Why: the merged map is last-wins, so a bare word like "Volatility" resolves
** to whichever file defined it last (passives.json) even when POB is drawing a
** gem name from Gems.lua. Marking the source lets both contexts be right. */
const char* translation_set_source(const char *name);

/* Currently selected source dictionary, or nullptr. */
const char* translation_get_source(void);

/* Look up a translation by stable id ("passives" -> GraphId, "affixes" -> mod
** Id) in passives_by_id.json / affixes_by_id.json. nullptr when the id or the
** source is unknown, when translation is off (F2), or when the locale's
** meta.json lacks "source_lookup". The files are listed in meta.json
** "source_dicts" (NOT load_order, so an older engine never sees them); their
** keys are ids, so they never reach the merged map or the English source
** dictionaries and translation_lookup cannot find them. */
const char* translation_lookup_by_id(const char *source, const char *id);

/* translation_lookup_by_id that also checks the English the caller is showing.
** A Timeless jewel / tattoo replaces a node's name but POB keeps its node.id,
** so an id hit alone can hand the replaced node the ORIGINAL node's name.
** en == nullptr: same as translation_lookup_by_id. en non-null: the id's
** translation only when the file's "templates" has the id and its GGPK
** English equals en (trimmed, inner whitespace collapsed, case-sensitive);
** otherwise nullptr. A file WITHOUT "templates" (older data pack) keeps the
** plain id behaviour. No source is exempt from the check. There is no
** "legion" id source at all: POB, poedb and GGPK agree on the legion Names,
** only GGPK's Id strings swap the two Faith notables, so an id lookup would
** be the wrong one. Tested by --stat-selftest P0-* and S8-*. */
const char* translation_lookup_by_id_en(const char *source, const char *id, const char *en);

/* Translate one stat line through stats_by_id.json (keys
** "stat_ids|cond|src_file|dup_ix", English templates in "templates").
** ctx = "item" (stat_descriptions.txt), "passive"
** (passive_skill_stat_descriptions.txt first, then stat_descriptions.txt) or
** "gem" (the skill/gem description files POB's gem tooltips use first, then
** stat_descriptions.txt).
** The line's numbers (ranges "(10-15)", signs included) are filled into the
** Chinese template by {N} index. nullptr when nothing matches, when the
** filtered candidates disagree (ambiguous), when translation is off (F2), or
** when the locale's meta.json lacks "source_lookup". Contract tested by
** --stat-selftest (host/stat_selftest.cpp). */
const char* translation_lookup_stat(const char *line, const char *ctx);

/* translation_lookup_stat narrowed by the GGPK stat ids behind the line
** (space separated; POB's describeStats lineMap gives one per line). After the
** ctx filter only candidates whose stat_ids intersect `stat_ids` stay; the
** rest is translation_lookup_stat's rules, and on a tie a candidate whose
** written-out numbers equal the line's values exactly wins. nullptr or ""
** stat_ids: same as translation_lookup_stat. Tested by --stat-selftest P1-3a/e. */
const char* translation_lookup_stat_ids(const char *line, const char *ctx, const char *stat_ids);

/* translation_lookup_stat with the line's colour kept: a leading "^xRRGGBB"
** or "^N" escape is stripped for the lookup and, on a hit, written in front of
** the answer AND after every '\n' in it (a GGPK template's "\n" turns one
** English line into several Chinese lines; each must keep the colour). No
** leading colour -> same as translation_lookup_stat. nullptr when no hit.
** Pointer valid until the next reload. Tested by --stat-selftest F4b/F4c. */
const char* translation_lookup_stat_colored(const char *line, const char *ctx);

/* translation_lookup_stat_colored narrowed by stat ids, as
** translation_lookup_stat_ids narrows translation_lookup_stat. nullptr or ""
** stat_ids: same as translation_lookup_stat_colored. */
const char* translation_lookup_stat_ids_colored(const char *line, const char *ctx, const char *stat_ids);

/* Consecutive lines that are ONE GGPK description (ggpk_groups.json): POB
** draws a multi-line description one tooltip line / label per English line,
** so each line alone is only a fragment. lines[0..n) are the drawn lines with
** the colour lead and POB's "not supported" note removed; ctx as
** translation_lookup_stat. out[i] = the Chinese to draw on line i for every
** line inside a recognised group ("" = leave the line blank: GGPK's Chinese
** has fewer lines), nullptr for lines in no group. Stat descriptions are
** worded by stats_by_id.json (the joined lines through the stat lookup), texts
** (flavour text) by the catalog itself, and only where GGPK's Chinese has a
** different line count. Returns the number of groups; 0 when translation is
** off (F2), without "source_lookup", or without the catalog. Pointers valid
** until the next call. Tested by --stat-selftest H*. */
int translation_group_lines(const char *const *lines, int n, const char *ctx, const char **out);

/* ctx ("item" / "passive" / "gem[:file]", as translation_lookup_stat) the
** "/"-joined line lookup inside translation_lookup uses (POB's crafting rows
** and mastery popup join a description's lines with "/" or " / "). nullptr /
** "" = "item", the default. The caller sets it around what it draws (the
** mastery popup: "passive") and puts back what this returns: the previous
** ctx, nullptr for the default (static storage, overwritten by the next call). */
const char* translation_set_slash_context(const char *ctx);

/* A display string that must be drawn in English exactly as given (使用者裁定
** 2026-10-27: a unique's flavour text stays English). From now on
** translation_lookup answers nullptr for that exact string -- before its exact
** map, caches, patterns and glossary -- so neither the tooltip that registered it
** nor the draw-time lookup translates it. Exact bytes, colour codes included;
** kept across translation_reload (the tooltip holding the line is not rebuilt);
** bounded (registrations past the cap are ignored). nullptr / "" is ignored. */
void translation_keep_english(const char *s);

/* The strings that stay English only for now -- the tooltip being drawn: its
** flavour lines' wrapped pieces carry no colour code, so they are not
** registered for good (translation_keep_english), where the same words drawn
** anywhere else would stay English too. Replaces the whole scope; n == 0 (or
** s == nullptr) clears it. The caller sets it around one tooltip's Draw and
** puts back what was there before (it keeps the lists). */
void translation_set_keep_english_scope(const char *const *s, int n);

/* ===== Reverse-translation trace =====
**
** Why this exists: reverse_one_line() is a chain of ~15 fallback rules over a
** flat dictionary merged from 8 files, and from the outside you cannot see
** which rule fired or which file's copy of a key won. Every claim about paste
** coverage then has to be inferred from black-box round trips, which is how
** three separate coverage figures came out wrong in one session.
**
** Between begin() and end(), each reverse-translated line appends one TSV row:
**   line <TAB> rule <TAB> key <TAB> file <TAB> nums <TAB> in <TAB> out
** `rule` names the branch that produced the result ("pattern", "status",
** "annotation", "miss", ...); `file` is the dictionary the winning entry came
** from. Tracing is off by default and costs nothing when off. */
void        translation_trace_begin(void);
const char* translation_trace_get(void);
void        translation_trace_end(void);

/* Render GGG's "[Key|Display]" / "[Key]" markup the way the game does before it
** draws a string. Exposed so a diagnostic can build a probe that looks like real
** game text using the ENGINE's own rendering — re-implementing it alongside is
** how an instrument ends up measuring itself instead of the engine. Returns a
** pointer to static storage, valid until the next call. */
const char* translation_strip_ggg_markup(const char *s);

/* Debug: the section grammar's verdict, one "<kind>\t<line>" row per input line
** (kinds: sep/header/property/enchant/mods/status/flavour/desc/-). Exposed so
** the selftest can assert what the grammar decided instead of only what came out
** the far end -- a wrong label that happens to produce the right English is a
** bug waiting for the next item shape. Buffer is valid until the next call. */
const char* translation_classify_dump(const char *text);

/* ===== 填值機制的測試掛勾(--placeholder-selftest 專用) =====
**
** 多佔位符的詞條靠 {N} 的編號對位,不靠出現順序:中文常把 {1} 講在 {0} 前面。
** 編號在雜湊成 '#' 的那一刻就消失了,所以對照表是載入時算好的,執行時只照表填。
**
** translation_debug_fill: 用 source 樣板解讀 input 的數值,填進 target 樣板。
** 走的是載入期/執行期共用的同一份計畫產生器,所以測試裡的邊角案例驗到的就是引擎
** 真正在跑的那一份。input 必須雜湊成與 source 相同的字串(執行期命中的條件),
** 否則回 nullptr —— 打錯的測試案例應該炸掉而不是安靜通過。
** 回傳靜態緩衝區,下次呼叫即失效。 */
const char* translation_debug_fill(const char *source_tmpl, const char *target_tmpl, const char *input);

/* 同一行文字,填值用的數值格與 extract_numbers 必須逐一相符(兩者是同一套掃描規則
** 的兩個出口)。回傳不一致的欄位數,0 = 相符。 */
int translation_debug_slot_mismatch(const char *text);

/* 突變開關:退回舊行為 —— 依 '#' 出現順序填,而且輸入只抽數字(原文自帶的 '#' 不
** 算一格)。兩半都要退,否則基準線比真正的舊引擎好,A/B 量到的差異會偏少。
** 必須在 translation_init() 之前設定才會影響字典裡的對照表(計畫是載入時算的);
** translation_debug_fill 則是每次呼叫都看現值。回傳先前的設定。 */
int translation_debug_set_legacy_fill(int on);

/* 只給 --editor-selftest 用:目前建了哪些來源表,格式
** "exact=gems,passives;pattern=passives;id=passives"(各段名稱依字母排序)。
** 用來驗證沒有 source_lookup 的語系只替 gems 建 exact 副本,其餘不建。
** 回傳靜態緩衝區,下次呼叫即失效。 */
const char* translation_debug_sources(void);

/* Get current locale string (e.g. "zh-rTW") */
const char* translation_get_locale(void);

/* Get number of loaded translation entries */
int translation_get_count(void);

/* 載入耗時摘要(一行文字,給探針與 console 用)。 */
const char* translation_get_init_stats(void);

/* Toggle translation on/off at runtime (F2 hotkey) */
void translation_set_enabled(bool enabled);
bool translation_is_enabled(void);

/* Reverse-translate a multi-line text (Chinese → English).
** Processes line by line, tries exact match and pattern match.
** Returns newly allocated string (caller must free with translation_free).
** Returns nullptr if no translations were made. */
char* translation_reverse_text(const char *chinese_text);

/* Free a string returned by translation_reverse_text */
void translation_free(char *str);

/* Reverse-lookup a single term: given Chinese text, return English.
** Uses a reverse map (Chinese → English) built at init time.
** Returns pointer to static storage, or nullptr if not found. */
const char* translation_reverse_lookup(const char *chinese);

/* Read an environment variable via the Win32 API (live environment).
** getenv() is unreliable here: the host exe uses a static CRT and sets
** POB_* variables with SetEnvironmentVariableW, which the UCRT's startup
** snapshot may predate. Returns buf on success, nullptr otherwise. */
const char* translation_win_env(const char *name, char *buf, unsigned size);

#endif /* TRANSLATION_MANAGER_H */
