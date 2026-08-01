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

/* Shutdown: free all translation data */
void translation_shutdown(void);

/* Reload: shutdown + re-init (used by F3 hotkey) */
void translation_reload(void);

/* Look up translation for a given English string.
** Returns translated string if found, or nullptr if no translation.
** Returned pointer is valid until translation_shutdown(). */
const char* translation_lookup(const char *english);

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

/* Get current locale string (e.g. "zh-rTW") */
const char* translation_get_locale(void);

/* Get number of loaded translation entries */
int translation_get_count(void);

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
