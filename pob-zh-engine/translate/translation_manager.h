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
** Returned pointer is valid until translation_shutdown(). */
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
** data file the string being drawn came from (currently only "gems"). Pass
** nullptr or "" to clear. Returns the previous selection (nullptr if none) so
** callers can restore it; an unknown name clears instead of failing.
**
** Why: the merged map is last-wins, so a bare word like "Volatility" resolves
** to whichever file defined it last (passives.json) even when POB is drawing a
** gem name from Gems.lua. Marking the source lets both contexts be right. */
const char* translation_set_source(const char *name);

/* Currently selected source dictionary, or nullptr. */
const char* translation_get_source(void);

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
