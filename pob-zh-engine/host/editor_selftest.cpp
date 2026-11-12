// Headless check that an edit made through the translation editor actually reaches the engine.
//
// The engine merges the dictionary files into ONE flat map (later file wins, order from
// meta.json), so an edit applied to the losing copy of a key changes nothing on screen.
// The test drives the real data layer (editor_data.cpp), then asks the real loader
// (translation_init + translation_lookup) what the engine returns -- shadowed case included.
//
// Runs against whatever Data\ sits next to the exe; point it at a scratch deployment so the
// shipping dictionaries are never touched. Every changed value is restored and verified.

#include "editor_selftest.h"

#include <windows.h>

#include <cstdio>
#include <map>
#include <unordered_set>
#include <string>
#include <vector>

#include "editor_data.h"
#include "trans_editor_logic.h"
#include "launcher_config.h"
#include "../translate/translation_manager.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* what)
{
	(ok ? g_pass : g_fail)++;
	printf("  %s  %s\n", ok ? "[PASS]" : "[FAIL]", what);
}

std::string narrow(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring exe_dir()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring s(path);
	size_t slash = s.find_last_of(L'\\');
	return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
}

bool read_file_bytes(const std::wstring& p, std::string& out)
{
	HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	out.clear();
	char tmp[65536];
	DWORD got = 0;
	while (ReadFile(h, tmp, sizeof(tmp), &got, nullptr) && got) out.append(tmp, got);
	CloseHandle(h);
	return true;
}

// The engine's own merge rule, replayed here so the test can say which file's
// copy of a key is the one that actually takes effect.
std::map<std::string, std::string> merge_winner(const EditorModel& model,
                                                const std::vector<std::string>& order)
{
	std::map<std::string, std::string> winner; // key -> file name
	for (const std::string& fname : order) {
		for (const EditorEntry& e : model.entries) {
			if (model.files[(size_t)e.fileIdx].name == fname)
				winner[e.key] = fname;
		}
	}
	return winner;
}

// One string array out of meta.json ("load_order", "source_only").
std::vector<std::string> meta_list(const EditorModel& model, const char* field)
{
	std::vector<std::string> order;
	for (const EditorFile& f : model.files) {
		if (f.name != "meta.json") continue;
		if (f.doc.contains(field) && f.doc[field].is_array()) {
			for (const auto& x : f.doc[field])
				if (x.is_string()) order.push_back(x.get<std::string>());
		}
	}
	if (order.empty()) {
		// meta.json is filtered out of the model (no "entries"): read it directly
		std::wstring p = model.dataDir + L"meta.json";
		HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			std::string buf;
			char tmp[4096];
			DWORD got = 0;
			while (ReadFile(h, tmp, sizeof(tmp), &got, nullptr) && got) buf.append(tmp, got);
			CloseHandle(h);
			try {
				auto doc = nlohmann::ordered_json::parse(buf);
				if (doc.contains(field) && doc[field].is_array())
					for (const auto& x : doc[field])
						if (x.is_string()) order.push_back(x.get<std::string>());
			} catch (...) {}
		}
	}
	return order;
}

std::vector<std::string> load_order(const EditorModel& model)
{
	return meta_list(model, "load_order");
}

// A file the global lookup never reads: listed in meta.json "source_only", or an
// id-keyed *_by_id.json. Its keys answer only per-source / per-id lookups, so
// translation_lookup() cannot see an edit made there.
bool not_in_global_table(const std::vector<std::string>& sourceOnly, const std::string& fname)
{
	static const std::string byId = "_by_id.json";
	if (fname.size() >= byId.size() &&
	    fname.compare(fname.size() - byId.size(), byId.size(), byId) == 0)
		return true;
	for (const std::string& s : sourceOnly)
		if (s == fname) return true;
	return false;
}

// What the engine returns for `key` after a fresh load of the dictionaries.
std::string engine_says(const char* key)
{
	translation_reload();
	const char* r = translation_lookup(key);
	return r ? std::string(r) : std::string();
}

} // namespace

// T1-T13 for one game. Writes to the REAL dictionaries (edit -> verify through the engine -> restore).
//
// `slotRoot` is ALWAYS the built-in <exeDir>Data\<game>\, never a configured external
// folder -- see RunEditorSelftest.
static int run_for_game(const std::wstring& exeDir, const std::wstring& slotRoot, const char* game)
{
	printf("\n=== %s ===\n", game);
	EditorModel model = LoadModel(slotRoot, "zh-rTW");
	if (!model.localeExists || model.files.empty()) {
		printf("  [FAIL]  locale dir not found or no dictionary files\n");
		return 2;
	}
	printf("  loaded %zu files, %zu entries\n", model.files.size(), model.entries.size());
	check(model.entries.size() > 1000, "T1 model loads the dictionaries");

	// The loader takes its locale from POB_GAME / POB_LOCALE (set by host_main). A headless run
	// inherits neither, and the loader silently enters passthrough mode -- every comparison
	// below would "pass" for the wrong reason. Set them as the launcher does and prove the files were read.
	SetEnvironmentVariableW(L"POB_GAME",
	                        std::wstring(game, game + strlen(game)).c_str()); // ASCII ids
	SetEnvironmentVariableW(L"POB_LOCALE", L"zh-rTW");
	translation_reload();
	printf("  engine: locale=%s entries=%d\n",
	       translation_get_locale() ? translation_get_locale() : "(null)",
	       translation_get_count());
	check(translation_get_count() > 1000, "T1b engine loaded the same dictionaries");

	std::vector<std::string> order = load_order(model);
	check(!order.empty(), "T2 meta.json load_order readable");
	std::map<std::string, std::string> winner = merge_winner(model, order);

	// pick two subjects
	// (a) a key that lives in exactly one file  -> editing it must take effect
	// (b) a key that lives in several files     -> editing the LOSING copy must
	//     not take effect, editing the winner must
	std::map<std::string, int> occurrences;
	for (const EditorEntry& e : model.entries) occurrences[e.key]++;
	// (a) must come from a file the global lookup reads: a key that only lives
	// in affixes.json (source_only) or a *_by_id.json is invisible to
	// translation_lookup(), so T5/T7/T8 would fail for the wrong reason.
	std::vector<std::string> sourceOnly = meta_list(model, "source_only");
	for (const std::string& f : meta_list(model, "source_dicts")) sourceOnly.push_back(f);

	const EditorEntry* single = nullptr;
	const EditorEntry* shadowLoser = nullptr;
	const EditorEntry* shadowWinner = nullptr;
	for (const EditorEntry& e : model.entries) {
		if (e.structured) continue;
		const std::string& owner = model.files[(size_t)e.fileIdx].name;
		if (!single && occurrences[e.key] == 1 && e.key.size() > 6 && e.value.size() > 2 &&
		    model.files[(size_t)e.fileIdx].order >= 0 &&
		    !not_in_global_table(sourceOnly, owner))
			single = &e;
		if (occurrences[e.key] > 1 && e.key.size() > 4) {
			auto w = winner.find(e.key);
			if (w == winner.end()) continue;
			if (!shadowLoser && owner != w->second) shadowLoser = &e;
			if (shadowLoser && shadowLoser->key == e.key && owner == w->second)
				shadowWinner = &e;
		}
		if (single && shadowLoser && shadowWinner) break;
	}
	check(single != nullptr, "T3 found a single-file key to edit");
	check(shadowLoser && shadowWinner, "T4 found a key present in several files");

	const std::string sentinel = u8"【編輯器自我測試】";

	// (a) single-file key: edit must reach the engine
	if (single) {
		const std::string key = single->key;
		const std::string before = single->value;
		const int fidx = single->fileIdx;
		std::string baseline = engine_says(key.c_str());
		if (baseline != before) {
			printf("     key      : %s\n", key.c_str());
			printf("     on disk  : %s (in %s)\n", before.c_str(),
			       model.files[(size_t)fidx].name.c_str());
			printf("     engine   : %s\n",
			       baseline.empty() ? "(no match)" : baseline.c_str());
		}
		check(baseline == before,
		      "T5 engine returns the on-disk value before editing");

		SetEntry(model, fidx, key, sentinel + before);
		const int editsBefore = DirtyEntryCount(model);
		std::string err;
		int saved = SaveAll(model, &err);
		check(saved > 0, "T6 SaveAll writes the edited file");
		// the save button counts edits, not files (one edit here, one file)
		check(editsBefore == 1 && DirtyEntryCount(model) == 0 && DirtyCount(model) == 0,
		      "T6b pending-edit count is per entry and clears on save");

		std::string after = engine_says(key.c_str());
		check(after == sentinel + before,
		      "T7 **edit through the editor reaches the engine**");

		// restore and prove the restore worked
		EditorModel reload = LoadModel(slotRoot, "zh-rTW");
		int ridx = FindFileIdx(reload, model.files[(size_t)fidx].name);
		SetEntry(reload, ridx, key, before);
		SaveAll(reload, &err);
		check(engine_says(key.c_str()) == before, "T8 restore verified");
	}

	// (b) shadowed key: editing the loser must NOT take effect
	if (shadowLoser && shadowWinner) {
		const std::string key = shadowLoser->key;
		const std::string loserFile = model.files[(size_t)shadowLoser->fileIdx].name;
		const std::string winnerFile = model.files[(size_t)shadowWinner->fileIdx].name;
		printf("  shadowed key %s: in %s (loses) and %s (wins)\n",
		       key.c_str(), loserFile.c_str(), winnerFile.c_str());

		EditorModel m2 = LoadModel(slotRoot, "zh-rTW");
		const std::string winnerValue = engine_says(key.c_str());
		std::string err;

		// edit the losing copy only
		int li = FindFileIdx(m2, loserFile);
		std::string loserBefore;
		for (const EditorEntry& e : m2.entries)
			if (e.key == key && e.fileIdx == li) loserBefore = e.value;
		SetEntry(m2, li, key, sentinel + loserBefore);
		SaveAll(m2, &err);
		std::string afterLoser = engine_says(key.c_str());
		if (afterLoser != winnerValue) {
			printf("     winner file value : %s\n", winnerValue.c_str());
			printf("     loser  file value : %s (%s)\n", loserBefore.c_str(),
			       loserFile.c_str());
			printf("     engine after edit : %s\n", afterLoser.c_str());
		}
		check(afterLoser == winnerValue,
		      "T9 editing the LOSING copy changes nothing (the shadowing trap)");

		// now edit the winning copy
		EditorModel m3 = LoadModel(slotRoot, "zh-rTW");
		int wi = FindFileIdx(m3, winnerFile);
		SetEntry(m3, wi, key, sentinel + winnerValue);
		SaveAll(m3, &err);
		check(engine_says(key.c_str()) == sentinel + winnerValue,
		      "T10 editing the WINNING copy does take effect");

		// restore both
		EditorModel m4 = LoadModel(slotRoot, "zh-rTW");
		SetEntry(m4, FindFileIdx(m4, loserFile), key, loserBefore);
		SetEntry(m4, FindFileIdx(m4, winnerFile), key, winnerValue);
		SaveAll(m4, &err);
		check(engine_says(key.c_str()) == winnerValue, "T11 restore verified");
	}

	// round-trip fidelity
	{
		EditorModel m = LoadModel(slotRoot, "zh-rTW");
		bool keysKept = true, metaKept = true;
		for (const EditorFile& f : m.files) {
			if (!f.doc.contains("entries")) { keysKept = false; break; }
			// source_files / is_base_items must survive a save untouched
			if (f.name == "items.json" && !f.doc.contains("source_files")) metaKept = false;
		}
		check(keysKept, "T12 every file still has its entries object");
		check(metaKept, "T13 non-entries fields survived the round trip");
	}

	// load order, exposed on the model: T24-T26 check the editor's own load_order reading
	// against the full merge replay and against the engine.
	{
		check(!model.loadOrder.empty(), "T24 model exposes meta.json load_order");

		std::vector<int> byOrder = FileIdxInLoadOrder(model);
		bool sameSize = byOrder.size() == model.files.size();
		bool ascending = true;
		int prev = -1;
		for (int fi : byOrder) {
			const int o = model.files[(size_t)fi].order;
			if (o < 0) continue;
			if (o <= prev) ascending = false;
			prev = o;
		}
		check(sameSize && ascending, "T25 FileIdxInLoadOrder covers every file, in order");

		std::map<std::string, int> occ;
		for (const EditorEntry& e : model.entries) occ[e.key]++;
		std::map<std::string, std::string> w = merge_winner(model, load_order(model));
		int checked = 0, wrong = 0;
		std::string firstWrong;
		for (const auto& kv : occ) {
			if (kv.second < 2) continue;
			auto it = w.find(kv.first);
			if (it == w.end()) continue;
			const int fi = WinnerFileIdx(model, kv.first);
			if (fi < 0 || model.files[(size_t)fi].name != it->second) {
				if (wrong == 0) firstWrong = kv.first;
				wrong++;
			}
			checked++;
		}
		if (wrong) printf("     first mismatch: %s\n", firstWrong.c_str());
		printf("     cross-checked %d shadowed keys\n", checked);
		check(checked > 100 && wrong == 0,
		      "T26 WinnerFileIdx agrees with a full merge replay on every shadowed key");
	}

	// line endings survive a save (works on copies)
	{
		bool anyCrlf = false, anyChecked = false, allKept = true;
		for (const EditorFile& f : model.files) {
			if (!f.crlf) continue;
			anyCrlf = true;
			const std::wstring tmp = f.path + L".lftest";
			if (!CopyFileW(f.path.c_str(), tmp.c_str(), FALSE)) continue;
			EditorFile probe;
			probe.name = f.name;
			probe.path = tmp;
			probe.doc = f.doc;
			probe.crlf = f.crlf;
			std::string err;
			if (SaveFile(probe, &err)) {
				std::string before, after;
				// count CRs before/after through the same reader the model uses
				HANDLE h = CreateFileW(tmp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
				                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (h != INVALID_HANDLE_VALUE) {
					char buf[8192];
					DWORD got = 0;
					size_t cr = 0, lf = 0;
					while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got)
						for (DWORD i = 0; i < got; i++) {
							if (buf[i] == '\r') cr++;
							if (buf[i] == '\n') lf++;
						}
					CloseHandle(h);
					if (cr == 0 || cr != lf) allKept = false;
					anyChecked = true;
				}
			}
			DeleteFileW(tmp.c_str());
			DeleteFileW((tmp + L".bak").c_str());
			break; // one representative file is enough
		}
		check(anyCrlf && anyChecked && allKept,
		      "T27 SaveFile keeps a CRLF file CRLF");
	}

	// adding a brand-new key reaches the engine. SetEntry has no delete path, so the file
	// is restored from a byte snapshot.
	{
		const std::string newKey = "__pobtools_editor_selftest_key__";
		const std::string newVal = u8"自我測試新增";
		const int target = FindFileIdx(model, "ui.json");
		check(target >= 0, "T28a ui.json present as an add target");
		if (target >= 0) {
			const std::wstring path = model.files[(size_t)target].path;
			const std::wstring snap = path + L".snap";
			CopyFileW(path.c_str(), snap.c_str(), FALSE);

			EditorModel m = LoadModel(slotRoot, "zh-rTW");
			const int ti = FindFileIdx(m, "ui.json");
			SetEntry(m, ti, newKey, newVal);
			std::string err;
			SaveAll(m, &err);
			check(engine_says(newKey.c_str()) == newVal,
			      "T28 a key added through the editor reaches the engine");

			MoveFileExW(snap.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
			DeleteFileW((path + L".bak").c_str());
			check(engine_says(newKey.c_str()).empty(), "T29 restore removed the test key");
		}
	}

	// locale enumeration
	{
		std::vector<std::string> l1 = ListLocales(BuiltinDictDir(exeDir, DictSlot::Poe1));
		std::vector<std::string> l2 = ListLocales(BuiltinDictDir(exeDir, DictSlot::Poe2));
		bool has1 = std::find(l1.begin(), l1.end(), std::string("zh-rTW")) != l1.end();
		bool has2 = std::find(l2.begin(), l2.end(), std::string("zh-rTW")) != l2.end();
		std::vector<std::string> lx = ListLocales(exeDir + L"Data\\poe9\\");
		check(has1 && has2, "T30 ListLocales finds zh-rTW for both games");
		check(!lx.empty(), "T31 ListLocales falls back instead of returning nothing");

		const std::wstring launcherRoot = BuiltinDictDir(exeDir, DictSlot::Launcher);
		std::vector<std::string> ll = ListLocales(launcherRoot);
		EditorModel lm = LoadModel(launcherRoot, "zh-rTW");
		check(std::find(ll.begin(), ll.end(), std::string("zh-rTW")) != ll.end(),
		      "T32 ListLocales sees the launcher-strings dictionary");
		check(lm.localeExists && lm.files.size() == 1 && lm.entries.size() >= 40 &&
		          lm.loadOrder.size() == 1 && lm.loadOrder[0] == "launcher.json",
		      "T33 the launcher dictionary loads like any other");

		// T33c -- a language nobody hardcoded: list it, load it, remove it.
		{
			const std::wstring probeRoot = BuiltinDictDir(exeDir, DictSlot::Poe1);
			const std::wstring probe = probeRoot + L"xx-TEST\\";
			CreateDirectoryW(probe.c_str(), nullptr);
			auto put = [](const std::wstring& p, const std::string& body) {
				HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				                       FILE_ATTRIBUTE_NORMAL, nullptr);
				if (h == INVALID_HANDLE_VALUE) return;
				DWORD w = 0;
				WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr);
				CloseHandle(h);
			};
			put(probe + L"meta.json",
			    "{\"display_name\":\"Probe\",\"load_order\":[\"probe.json\"]}");
			put(probe + L"probe.json", "{\"entries\":{\"Chaos Orb\":\"PROBE\"}}");

			std::vector<std::string> ls = ListLocales(probeRoot);
			EditorModel pm = LoadModel(probeRoot, "xx-TEST");
			const bool listed = std::find(ls.begin(), ls.end(), std::string("xx-TEST")) != ls.end();
			check(listed && pm.localeExists && pm.entries.size() == 1 &&
			          pm.loadOrder.size() == 1 && pm.loadOrder[0] == "probe.json",
			      "T33c a newly dropped-in language folder is listed and loads");

			DeleteFileW((probe + L"probe.json").c_str());
			DeleteFileW((probe + L"meta.json").c_str());
			RemoveDirectoryW(probe.c_str());
			check(GetFileAttributesW(probe.c_str()) == INVALID_FILE_ATTRIBUTES,
			      "T33d the probe language folder was removed again");
		}

		// T33b -- which rows get the expanded editor (multi-line yes, short label no).
		{
			EditorEntry multi; multi.value = "one\ntwo";
			EditorEntry longv; longv.value = std::string(80, 'x');
			EditorEntry longk; longk.key = std::string(80, 'x'); longk.value = "short";
			// not `small`: windows.h (rpcndr.h) has `#define small char`
			EditorEntry brief; brief.key = "Close"; brief.value = u8"關閉";
			int multiline = 0;
			for (const EditorEntry& e : lm.entries)
				if (e.value.find('\n') != std::string::npos) multiline++;
			check(NeedsExpandedEditor(multi) && NeedsExpandedEditor(longv) &&
			          NeedsExpandedEditor(longk) && !NeedsExpandedEditor(brief) && multiline >= 1,
			      "T33b long / multi-line entries get the expanded editor, short ones do not");
		}
	}

	// T33c -- every table cell is exactly one line.
	//
	// The entries table is an ImGuiListClipper over 110k rows sizing the scroll range from ONE
	// row's height, so a single taller row strands the last entries off the bottom.
	{
		check(OneLineForCell("a\nb") == "a\\nb" &&
		          OneLineForCell("a\r\nb") == "a\\nb" &&
		          OneLineForCell("plain") == "plain",
		      "T33c1 a line break becomes the two characters \\n, CRLF included");

		int folded = 0;
		std::string worst;
		for (const EditorEntry& e : model.entries) {
			const std::string k = OneLineForCell(e.key);
			const std::string v = OneLineForCell(e.value);
			if (e.key.find('\n') != std::string::npos) folded++;
			if (k.find_first_of("\r\n") != std::string::npos ||
			    v.find_first_of("\r\n") != std::string::npos) {
				if (worst.empty()) worst = e.key.substr(0, 40);
			}
		}
		// `folded > 0` keeps this honest: otherwise a dictionary with no multi-line key passes vacuously.
		const std::string msg = "T33c2 no dictionary entry can draw a multi-line cell (" +
		                        std::to_string(folded) + " folded)";
		check(worst.empty() && folded > 0, msg.c_str());
	}

	// SaveFile leaves a .bak per edited file; the values are already restored, so remove the litter.
	for (const EditorFile& f : model.files)
		DeleteFileW((f.path + L".bak").c_str());

	return 0;
}

int RunEditorSelftest(const std::string& games)
{
	g_pass = g_fail = 0;
	const std::wstring dir = exe_dir();

	// PINNED to the shipped dictionaries (empty configured path): following the user's DataDir
	// setting would rewrite a translator's working copy, which has no backup.
	const std::wstring poe1Root = ResolveDictDir(dir, DictSlot::Poe1, L"").root;
	const std::wstring poe2Root = ResolveDictDir(dir, DictSlot::Poe2, L"").root;
	printf("editor selftest - dictionary folders: %s | %s\n",
	       narrow(poe1Root).c_str(), narrow(poe2Root).c_str());
	printf("  NOTE: this edits the dictionaries next to the exe (edit -> verify -> restore)\n");
	// Prove the pinning: even with an external folder configured, the built-in ones are used.
	check(poe1Root == BuiltinDictDir(dir, DictSlot::Poe1) &&
	          poe2Root == BuiltinDictDir(dir, DictSlot::Poe2),
	      "T34 selftest is pinned to the built-in dictionaries");

	const bool doPoe1 = (games != "poe2");
	const bool doPoe2 = (games != "poe1");
	if (doPoe1 && run_for_game(dir, poe1Root, "poe1") == 2) return 2;
	if (doPoe2 && run_for_game(dir, poe2Root, "poe2") == 2) return 2;

	// Everything below is poe1-only (Volatility gem/passive collision). Printed, not skipped silently.
	printf("\n=== source dictionaries (poe1 only) ===\n");
	SetEnvironmentVariableW(L"POB_GAME", L"poe1");
	SetEnvironmentVariableW(L"POB_LOCALE", L"zh-rTW");
	translation_reload();
	EditorModel model = LoadModel(poe1Root, "zh-rTW");
	std::vector<std::string> order = load_order(model);
	(void)order;

	// source dictionaries: POB stores support gems under their SHORT name, so "Volatility" is both a
	// gem (易變輔助) and a passive node (易爆). passives.json loads later and wins
	// the merge, which is right for the tree and wrong for the skill tab. The
	// engine resolves it by letting POB name the data file a string came from.
	{
		translation_reload();
		auto say = [](const char* key) {
			const char* r = translation_lookup(key);
			return r ? std::string(r) : std::string();
		};
		const std::string bareVolatility = say("Volatility");
		const std::string bareList = say("Volatility, Point Blank");

		check(bareVolatility == u8"易爆",
		      "T14 unmarked lookup is unchanged (passive wording still wins)");

		const char* prev = translation_set_source("gems");
		check(prev == nullptr, "T15 setting a source returns the previous one (none)");
		check(say("Volatility") == u8"易變輔助",
		      "T16 **gem source returns the gem name (issue #3)**");
		// A comma list recurses through the whole pipeline and caches its result;
		// that result must not escape into the shared cache.
		const std::string sourcedList = say("Volatility, Point Blank");
		check(sourcedList == u8"易變輔助, 零點射擊輔助",
		      "T17 每段都走來源字典");

		translation_set_source(nullptr);
		check(say("Volatility") == bareVolatility,
		      "T18 clearing the source restores the previous answer");
		check(say("Volatility, Point Blank") == bareList,
		      "T19 **the source result never leaks into the shared cache**");

		// An unknown name must degrade to "no source", not to a crash or a stale
		// selection: an older data pack may simply not have the file.
		translation_set_source("no-such-source");
		check(translation_get_source() == nullptr,
		      "T20 unknown source name degrades to none");
		check(say("Volatility") == bareVolatility, "T21 ...and lookups are unaffected");

		// Nesting: the returned name is what restores the outer scope.
		translation_set_source("gems");
		const char* inner = translation_set_source("gems");
		check(inner && std::string(inner) == "gems",
		      "T22 nested set returns the outer source for restoring");
		translation_set_source(inner);
		check(say("Volatility") == u8"易變輔助", "T23 outer source still in effect");
		translation_set_source(nullptr);
	}

	// more source dictionaries (S1-S9): data is planted (and restored) so assertions don't depend on
	// shipped content. Every planted competitor sits in a file loading LATER than the source file,
	// so a source that silently degrades to "none" cannot pass.
	printf("\n=== source dictionaries: passives/affixes/stats/... (poe1 only) ===\n");
	{
		auto say = [](const char* key) {
			const char* r = translation_lookup(key);
			return r ? std::string(r) : std::string();
		};
		auto sayTitle = [](const char* key) {
			const char* r = translation_lookup_item_title(key);
			return r ? std::string(r) : std::string();
		};
		auto readBytes = [](const std::wstring& p, std::string& out) -> bool {
			HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
			                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) return false;
			out.clear();
			char tmp[65536];
			DWORD got = 0;
			while (ReadFile(h, tmp, sizeof(tmp), &got, nullptr) && got) out.append(tmp, got);
			CloseHandle(h);
			return true;
		};
		auto writeBytes = [](const std::wstring& p, const std::string& body) -> bool {
			HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			                       FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) return false;
			DWORD w = 0;
			const BOOL ok = WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr);
			CloseHandle(h);
			return ok && w == (DWORD)body.size();
		};

		const std::wstring dictDir = model.dataDir;
		const char* kRobust = "Robust";
		const char* kStatTmpl = "{0}% increased TestStatXyz";
		const char* kStatExact = "15% increased TestStatXyz";

		translation_set_source(nullptr);
		translation_reload();
		const std::string robustBefore = say(kRobust);

		// S17: the shipped meta.json keeps source-only / id files OUT of
		// load_order. An older engine (v1.8.1) reads load_order only and merges
		// everything in it: the affix names would reach the global table and the
		// id files' numeric keys would be taken as English.
		{
			std::string body;
			bool ok = readBytes(dictDir + L"meta.json", body);
			std::string why;
			try {
				auto meta = nlohmann::ordered_json::parse(body);
				auto inArr = [&](const char* field, const char* f) {
					if (!meta.contains(field) || !meta[field].is_array()) return false;
					for (const auto& x : meta[field])
						if (x.is_string() && x.get<std::string>() == f) return true;
					return false;
				};
				for (const char* f : {"affixes.json", "passives_by_id.json",
				                      "affixes_by_id.json", "ascendancy.json"})
					if (inArr("load_order", f)) { ok = false; why += std::string(" load_order:") + f; }
				for (const char* f : {"affixes.json", "passives_by_id.json", "affixes_by_id.json"})
					if (!inArr("source_dicts", f)) { ok = false; why += std::string(" !source_dicts:") + f; }
				if (!(meta.contains("source_lookup") && meta["source_lookup"] == true)) {
					ok = false; why += " !source_lookup";
				}
			} catch (...) { ok = false; why += " parse"; }
			check(ok, "S17 meta.json 的 load_order 不含只當來源與 id 的檔(舊引擎看不到)");
			if (!ok) printf("    %s\n", why.c_str());
		}

		// S20: the editor reads load_order for its file ranks, so those files
		// show up as "not in load_order" (rank -1): never offered as a target for
		// new keys and never treated as winning the merge.
		{
			bool ok = true;
			for (const EditorFile& f : model.files) {
				if ((f.name == "affixes.json" || f.name == "passives_by_id.json" ||
				     f.name == "affixes_by_id.json" || f.name == "ascendancy.json") && f.order >= 0) {
					ok = false;
					printf("    %s order=%d\n", f.name.c_str(), f.order);
				}
			}
			check(ok, "S20 編輯器把只當來源與 id 的檔列為未載入(不當新增目標)");
		}

		// S1: every key of every dictionary, looked up unmarked under two
		// meta.json variants -- the shipped one, and one stripped of
		// source_dicts / source_lookup / source_only (what an engine that knows
		// none of these fields makes of the same data). Same data, so the
		// unmarked answers must be identical key for key.
		{
			std::string metaBody;
			bool ok = readBytes(dictDir + L"meta.json", metaBody);
			std::vector<std::string> keys;
			{
				std::unordered_set<std::string> seen;
				for (const EditorEntry& e : model.entries)
					if (seen.insert(e.key).second) keys.push_back(e.key);
			}
			std::string stripped;
			try {
				auto meta = nlohmann::ordered_json::parse(metaBody);
				meta.erase("source_dicts");
				meta.erase("source_lookup");
				meta.erase("source_only");
				stripped = meta.dump(2);
			} catch (...) { ok = false; }
			auto sayAll = [&](std::vector<std::string>& out) {
				translation_set_source(nullptr);
				translation_reload();
				out.clear();
				out.reserve(keys.size());
				for (const std::string& k : keys) out.push_back(say(k.c_str()));
			};
			std::vector<std::string> plain, full;
			const DWORD t0 = GetTickCount();
			if (ok && writeBytes(dictDir + L"meta.json", stripped)) sayAll(plain); else ok = false;
			if (!writeBytes(dictDir + L"meta.json", metaBody)) ok = false;
			if (ok) sayAll(full);
			// A key that still holds a raw "{0}" is an unfilled template: POB never
			// shows one, so no player reaches it, and with source_lookup on the
			// "/"-joined variants resolve through stats_by_id (numbers copied from
			// GGPK, keep_literals). Those may legitimately differ.
			size_t diffs = 0;
			for (size_t i = 0; ok && i < keys.size(); i++) {
				if (plain[i] == full[i]) continue;
				if (keys[i].find("{0}") != std::string::npos) continue;
				if (++diffs <= 10)
					printf("    %s: stripped=%s full=%s\n", keys[i].c_str(), plain[i].c_str(), full[i].c_str());
			}
			printf("  (S1: %zu keys, %zu differ, %lu ms)\n", keys.size(), diffs,
			       (unsigned long)(GetTickCount() - t0));
			check(ok && keys.size() > 10000 && diffs == 0,
			      "S1 全字典未標記查找：完整 meta 與去掉來源欄位的 meta 結果相同");
		}

		// Back up every file this block touches, byte for byte.
		struct Backup { std::wstring path; std::string bytes; bool existed; };
		std::vector<Backup> backups;
		auto backup = [&](const wchar_t* name) {
			Backup b{dictDir + name, {}, false};
			b.existed = readBytes(b.path, b.bytes);
			backups.push_back(b);
		};
		backup(L"meta.json");
		backup(L"passives.json");
		backup(L"affixes.json");
		backup(L"stats.json");
		backup(L"ui.json");
		backup(L"uniques.json");
		backup(L"monsters.json");
		backup(L"passives_by_id.json");
		backup(L"affixes_by_id.json");

		// Add entries to a dictionary file (creating it when absent).
		bool planted = true;
		auto plant = [&](const wchar_t* name,
		                 std::initializer_list<std::pair<const char*, const char*>> kv) {
			std::string body;
			nlohmann::ordered_json doc;
			if (readBytes(dictDir + name, body)) {
				try { doc = nlohmann::ordered_json::parse(body); }
				catch (...) { planted = false; return; }
			} else {
				doc = nlohmann::ordered_json::object();
			}
			if (!doc.contains("entries") || !doc["entries"].is_object())
				doc["entries"] = nlohmann::ordered_json::object();
			for (const auto& p : kv) doc["entries"][p.first] = p.second;
			if (!writeBytes(dictDir + name, doc.dump(2))) planted = false;
		};
		plant(L"passives.json", {{kRobust, u8"堅固"}});
		// affixes.json is source-only (meta.json "source_only"): TestOnlyAffixXyz
		// exists nowhere else, so any unmarked hit means it leaked into the merge.
		plant(L"affixes.json", {{kRobust, u8"健壯的"}, {"TestOnlyAffixXyz", u8"測試詞綴名"}});
		plant(L"stats.json", {{kStatTmpl, u8"增加 {0}% 測試詞綴甲"},
		                      // S18: the engine walks entries in KEY order (nlohmann::json
		                      // is a std::map), not file order. Case 1: the dedicated
		                      // key sorts first ("...OneXyz" < "...OneXyz\n..."), so it
		                      // is written before the joined entry derives it.
		                      {"TestSplitOneXyz", u8"專用一"},
		                      {"TestSplitOneXyz\nTestSplitTwoXyz", u8"衍生一\n衍生二"},
		                      // Case 2: the dedicated key is the joined entry's SECOND
		                      // line and sorts after it ("...Aaa..." < "...Zzz..."), so
		                      // the derived line is written first, the dedicated one after.
		                      {"TestSplitAaaXyz\nTestSplitZzzXyz", u8"衍生甲\n衍生乙"},
		                      {"TestSplitZzzXyz", u8"專用乙"}});
		// uniques.json loads after stats.json: its pattern would win the merge.
		plant(L"uniques.json", {{kStatTmpl, u8"錯誤乙 {0}%"}});
		// ui.json carries the EXACT line -- exact beats pattern in the merge.
		plant(L"ui.json", {{kStatExact, u8"錯誤丙"}});
		// monsters.json loads last: the unmarked "Robust" becomes this.
		plant(L"monsters.json", {{kRobust, u8"錯誤丁"}});
		// S12-S14: id-keyed dictionaries. Two passives share one English name
		// but different GraphIds; each id has its own wording. Written whole
		// (not via plant) to match the spec's file shape.
		auto plantById = [&](const wchar_t* name, const char* srcFile,
		                     std::initializer_list<std::pair<const char*, const char*>> kv) {
			nlohmann::ordered_json doc = nlohmann::ordered_json::object();
			doc["source_files"] = nlohmann::ordered_json::array({srcFile});
			doc["is_base_items"] = false;
			doc["entries"] = nlohmann::ordered_json::object();
			for (const auto& p : kv) doc["entries"][p.first] = p.second;
			if (!writeBytes(dictDir + name, doc.dump(2))) planted = false;
		};
		plantById(L"passives_by_id.json", "passiveTree.csv",
		          {{"90001", u8"戰神"}, {"90002", u8"勇士"}});
		plantById(L"affixes_by_id.json", "Mods.dat",
		          {{"TestModIdXyz1", u8"測試詞綴甲"}, {"TestModIdXyz2", u8"測試詞綴乙"}});
		{
			std::string body;
			if (readBytes(dictDir + L"meta.json", body)) {
				try {
					auto meta = nlohmann::ordered_json::parse(body);
					// Source-only and id files live in "source_dicts" and NOT in
					// load_order (an older engine reads load_order only).
					auto& lo = meta["load_order"];
					for (const char* f : {"affixes.json", "passives_by_id.json", "affixes_by_id.json"}) {
						for (auto it = lo.begin(); it != lo.end();) {
							if (it->is_string() && it->get<std::string>() == f) it = lo.erase(it);
							else ++it;
						}
					}
					meta.erase("source_only");
					auto& sd = meta["source_dicts"];
					if (!sd.is_array()) sd = nlohmann::ordered_json::array();
					for (const char* f : {"affixes.json", "passives_by_id.json", "affixes_by_id.json"}) {
						bool have = false;
						for (const auto& x : sd) if (x.is_string() && x.get<std::string>() == f) have = true;
						if (!have) sd.push_back(f);
					}
					meta["source_lookup"] = true;
					if (!writeBytes(dictDir + L"meta.json", meta.dump(2))) planted = false;
				} catch (...) { planted = false; }
			} else {
				planted = false;
			}
		}
		check(planted, "S0 測試用暫時條目已寫入字典");
		translation_set_source(nullptr);
		translation_reload();

		const std::string bareRobust = say(kRobust);
		printf("  (unmarked Robust after planting = %s)\n", bareRobust.c_str());

		// S2 / S3
		translation_set_source("passives");
		check(say(kRobust) == u8"堅固", "S2 passives 來源下 Robust 譯為堅固");
		translation_set_source(nullptr);
		translation_set_source("affixes");
		check(say(kRobust) == u8"健壯的", "S3 affixes 來源下 Robust 譯為健壯的");
		translation_set_source(nullptr);

		// S4: no exact key for 23%, so only patterns compete.
		translation_set_source("stats");
		check(say("23% increased TestStatXyz") == u8"增加 23% 測試詞綴甲",
		      "S4 stats 來源的 pattern 贏過他檔 pattern");
		translation_set_source(nullptr);

		// S5
		translation_set_source("stats");
		const std::string s5Sourced = say(kStatExact);
		translation_set_source(nullptr);
		const std::string s5Bare = say(kStatExact);
		check(s5Sourced == u8"增加 15% 測試詞綴甲", "S5 stats 來源的 pattern 贏過他檔 exact");
		check(s5Bare == u8"錯誤丙", "S5b 未標記時仍是他檔 exact");

		// S6a: bare first, sourced, bare again (cache must not keep the sourced one).
		{
			const char* combo = "15% increased TestStatXyz, Robust";
			const std::string bareCombo = say(combo);
			translation_set_source("stats");
			const std::string sourcedCombo = say(combo);
			translation_set_source(nullptr);
			check(sourcedCombo.find(u8"增加 15% 測試詞綴甲") != std::string::npos,
			      "S6 stats 來源下組合字串的數值段走來源字典");
			check(say(combo) == bareCombo, "S6a 切走來源後組合字串回到未標記值");
		}
		// S6b: sourced FIRST on a never-seen string, then compare with a fresh load.
		{
			const char* combo = "23% increased TestStatXyz, Robust";
			translation_set_source("stats");
			(void)say(combo);
			translation_set_source(nullptr);
			const std::string afterSourced = say(combo);
			translation_reload();
			const std::string fresh = say(combo);
			check(afterSourced == fresh, "S6b 先查來源再查未標記不會吃到來源快取");
		}

		// S7: translation_lookup_item_title is public, so the title path is tested too.
		{
			const std::string bareTitle = sayTitle(kRobust);
			translation_set_source("affixes");
			const std::string sourcedTitle = sayTitle(kRobust);
			translation_set_source(nullptr);
			check(sourcedTitle == u8"健壯的", "S7 affixes 來源下物品標題查找也走來源字典");
			check(sayTitle(kRobust) == bareTitle, "S7b 切走來源後物品標題回到未標記值");
		}

		// S10 / S11: a source-only file feeds its source dictionary and nothing else.
		{
			const char* kOnly = "TestOnlyAffixXyz";
			translation_set_source(nullptr);
			const char* r = translation_lookup(kOnly);
			check(r == nullptr || std::string(r) == kOnly, "S10 只當來源的檔不進全域表");
			if (r && std::string(r) != kOnly) printf("    unmarked %s = %s\n", kOnly, r);
			translation_set_source("affixes");
			check(say(kOnly) == u8"測試詞綴名", "S11 只當來源的檔在 affixes 來源下仍可查");
			translation_set_source(nullptr);
		}

		// S12-S14: lookups by stable id (GraphId / mod Id), not by English text.
		{
			auto byId = [](const char* src, const char* id) {
				const char* r = translation_lookup_by_id(src, id);
				return r ? std::string(r) : std::string();
			};
			translation_set_source(nullptr);
			const std::string a = byId("passives", "90001");
			const std::string b = byId("passives", "90002");
			check(a == u8"戰神" && b == u8"勇士", "S12 同名天賦依 GraphId 取不同譯名");
			if (a != u8"戰神" || b != u8"勇士")
				printf("    90001=%s 90002=%s\n", a.c_str(), b.c_str());

			// S13: a miss is nullptr -- guarded by hits in the same files, so an
			// always-nullptr implementation cannot pass.
			const bool missNull = translation_lookup_by_id("passives", "99999") == nullptr;
			const bool affixHit = byId("affixes", "TestModIdXyz1") == u8"測試詞綴甲";
			const bool crossNull = translation_lookup_by_id("affixes", "90001") == nullptr;
			const bool unknownNull = translation_lookup_by_id("no-such", "90001") == nullptr;
			check(missNull && !a.empty() && affixHit && crossNull && unknownNull,
			      "S13 查不到的 id 回傳空值(同檔有命中為前提)");
			if (!(missNull && !a.empty() && affixHit && crossNull && unknownNull))
				printf("    miss=%d passivesHit=%d affixHit=%d cross=%d unknown=%d\n",
				       missNull, !a.empty(), affixHit, crossNull, unknownNull);

			// S14: the id file is loaded (S12 hit) yet its keys reach neither the
			// merged table nor the English "passives" source dictionary.
			const char* bare = translation_lookup("90001");
			const bool bareKept = bare == nullptr || std::string(bare) == "90001";
			translation_set_source("passives");
			const char* sourced = translation_lookup("90001");
			const bool sourcedKept = sourced == nullptr || std::string(sourced) == "90001";
			translation_set_source(nullptr);
			check(bareKept && sourcedKept && !a.empty(), "S14 by_id 檔不進全域表");
			if (!(bareKept && sourcedKept && !a.empty()))
				printf("    bare=%s sourced=%s loaded=%d\n", bare ? bare : "(null)",
				       sourced ? sourced : "(null)", !a.empty());

			// S15: F2 (translation off) applies to id lookups too.
			translation_set_enabled(false);
			const char* off = translation_lookup_by_id("passives", "90001");
			translation_set_enabled(true);
			const std::string on = byId("passives", "90001");
			check(off == nullptr && on == u8"戰神", "S15 翻譯關閉時依 id 查詢一律回傳空值");
			if (!(off == nullptr && on == u8"戰神"))
				printf("    off=%s on=%s\n", off ? off : "(null)", on.c_str());
		}

		// S18: inside the source copy a derived (split) line never replaces a
		// purpose-built entry, whichever is written first.
		{
			translation_set_source("stats");
			const std::string one = say("TestSplitOneXyz");
			const std::string two = say("TestSplitTwoXyz");
			const std::string aaa = say("TestSplitAaaXyz");
			const std::string zzz = say("TestSplitZzzXyz");
			translation_set_source(nullptr);
			const bool first = one == u8"專用一" && two == u8"衍生二";
			const bool later = zzz == u8"專用乙" && aaa == u8"衍生甲";
			check(first, "S18 專用條目先寫：拆行衍生條目不覆蓋它");
			check(later, "S18b 衍生條目先寫：後寫的專用條目蓋過它");
			if (!(first && later))
				printf("    one=%s two=%s aaa=%s zzz=%s\n", one.c_str(), two.c_str(),
				       aaa.c_str(), zzz.c_str());
		}

		// S8
		translation_set_source("no-such");
		check(translation_get_source() == nullptr && say(kRobust) == bareRobust,
		      "S8 未知來源退回無來源");
		translation_set_source(nullptr);

		// S9
		{
			const char* outer = translation_set_source("passives");
			const char* inner = translation_set_source("stats");
			const bool innerOk = inner && std::string(inner) == "passives";
			translation_set_source(inner);
			const bool backToPassives = say(kRobust) == u8"堅固";
			translation_set_source(outer);
			check(innerOk && backToPassives && translation_get_source() == nullptr,
			      "S9 巢狀設定逐層還原：先回 passives 再回無來源");
			translation_set_source(nullptr);
		}

		// S16: a locale whose meta.json lacks "source_lookup" keeps the old
		// behaviour: every source added by this work answers exactly like the
		// unmarked lookup, and id lookups find nothing. "gems" (shipped before
		// the flag existed) still works as it always did.
		{
			std::string flagged;
			bool ok = readBytes(dictDir + L"meta.json", flagged);
			try {
				auto meta = nlohmann::ordered_json::parse(flagged);
				meta.erase("source_lookup");
				ok = ok && writeBytes(dictDir + L"meta.json", meta.dump(2));
			} catch (...) { ok = false; }
			translation_set_source(nullptr);
			translation_reload();
			std::vector<std::string> keys = {kRobust, kStatExact, "23% increased TestStatXyz",
			                                 "TestOnlyAffixXyz", "TestSplitOneXyz", "Volatility"};
			for (const char* f : {"passives.json", "affixes.json", "stats.json", "uniques.json", "items.json"}) {
				int n = 0;
				for (const EditorEntry& e : model.entries)
					if (model.files[(size_t)e.fileIdx].name == f && n++ < 500) keys.push_back(e.key);
			}
			std::vector<std::string> bare, bareTitle;
			for (const std::string& k : keys) {
				bare.push_back(say(k.c_str()));
				bareTitle.push_back(sayTitle(k.c_str()));
			}
			size_t diffs = 0;
			bool noneActive = true;
			for (const char* src : {"passives", "affixes", "stats", "uniques", "items", "ascendancy"}) {
				translation_set_source(src);
				if (translation_get_source() != nullptr) noneActive = false;
				for (size_t i = 0; i < keys.size(); i++) {
					const std::string got = say(keys[i].c_str());
					const std::string gotTitle = sayTitle(keys[i].c_str());
					if ((got != bare[i] || gotTitle != bareTitle[i]) && ++diffs <= 10)
						printf("    [%s] %s: sourced=%s bare=%s\n", src, keys[i].c_str(),
						       got.c_str(), bare[i].c_str());
				}
				translation_set_source(nullptr);
			}
			const bool idNull = translation_lookup_by_id("passives", "90001") == nullptr &&
			                    translation_lookup_by_id("affixes", "TestModIdXyz1") == nullptr;
			translation_set_source("gems");
			const std::string gemVol = say("Volatility");
			translation_set_source(nullptr);
			printf("  (S16: %zu keys x 6 sources, %zu differ)\n", keys.size(), diffs);
			check(ok && diffs == 0 && noneActive,
			      "S16 無 source_lookup 旗標：設來源後的查找等於未標記");
			check(ok && idNull, "S16b 無 source_lookup 旗標：依 id 查詢回傳空值");
			check(ok && gemVol == u8"易變輔助", "S16c 無 source_lookup 旗標：gems 來源照舊有效");
			// S22: and nothing is built that could never be read.
			const std::string built = translation_debug_sources();
			check(ok && built == "exact=gems;pattern=;id=",
			      "S22 無 source_lookup 旗標：只替 gems 建來源 exact 表，其餘不建");
			if (built != "exact=gems;pattern=;id=") printf("    %s\n", built.c_str());
			writeBytes(dictDir + L"meta.json", flagged);
			translation_reload();
			// S22a: guard -- with the flag every source is built, so S22 is not
			// passing on an observer that always reports "gems".
			const std::string full = translation_debug_sources();
			check(full == "exact=affixes,anoint,gem_names,gem_names_nonactive,gem_names_support,gems,items,legion,passives,stats,tattoo,title_words,uniques;"
			              "pattern=affixes,anoint,gem_names,gem_names_nonactive,gem_names_support,gems,items,legion,passives,stats,tattoo,title_words,uniques;"
			              "id=affixes,ascendancy,gem_names,gems_desc,mod_stats,passive_stats,passives",
			      "S22a 有旗標時來源表齊全(觀察函式不是假的)");
			if (full.find("passives") == std::string::npos) printf("    %s\n", full.c_str());
			else printf("    (flagged: %s)\n", full.c_str());
		}

		// S21: a file listed in load_order AND source_dicts is still never merged.
		{
			std::string flagged;
			bool ok = readBytes(dictDir + L"meta.json", flagged);
			try {
				auto meta = nlohmann::ordered_json::parse(flagged);
				meta["load_order"].insert(meta["load_order"].begin(), "affixes.json");
				ok = ok && writeBytes(dictDir + L"meta.json", meta.dump(2));
			} catch (...) { ok = false; }
			translation_set_source(nullptr);
			translation_reload();
			const char* kOnly = "TestOnlyAffixXyz";
			const char* r = translation_lookup(kOnly);
			const bool bareKept = r == nullptr || std::string(r) == kOnly;
			translation_set_source("affixes");
			const std::string sourced = say(kOnly);
			translation_set_source(nullptr);
			check(ok && bareKept && sourced == u8"測試詞綴名",
			      "S21 同時列在 load_order 與 source_dicts 的檔不併入全域表");
			if (!(bareKept && sourced == u8"測試詞綴名"))
				printf("    unmarked=%s sourced=%s\n", r ? r : "(null)", sourced.c_str());
			writeBytes(dictDir + L"meta.json", flagged);
			translation_reload();
		}

		// Restore every file exactly, delete what we created.
		bool restored = true;
		for (const Backup& b : backups) {
			if (b.existed) {
				if (!writeBytes(b.path, b.bytes)) restored = false;
			} else {
				DeleteFileW(b.path.c_str());
			}
			std::string now;
			const bool exists = readBytes(b.path, now);
			if (exists != b.existed || (exists && now != b.bytes)) restored = false;
		}
		translation_reload();
		check(restored && say(kRobust) == robustBefore, "SR 測試結束後字典完整還原");
	}

	// S16p: the shipped poe2 dictionaries carry "source_lookup". Real data, nothing planted: a source may
	// pick a different official wording than the unmarked lookup -- that is the
	// point of sources -- but it must never lose a translation the unmarked
	// lookup had.
	printf("\n=== source dictionaries: poe2 with source_lookup ===\n");
	{
		SetEnvironmentVariableW(L"POB_GAME", L"poe2");
		translation_set_source(nullptr);
		translation_reload();
		EditorModel m2 = LoadModel(poe2Root, "zh-rTW");
		auto say = [](const std::string& key) {
			const char* r = translation_lookup(key.c_str());
			return r ? std::string(r) : std::string();
		};
		std::vector<std::string> keys;
		for (const char* f : {"passives.json", "stats.json", "items.json", "uniques.json"}) {
			int n = 0;
			for (const EditorEntry& e : m2.entries)
				if (m2.files[(size_t)e.fileIdx].name == f && n++ < 500) keys.push_back(e.key);
		}
		std::vector<std::string> bare;
		for (const std::string& k : keys) bare.push_back(say(k));
		size_t lost = 0, differ = 0;
		for (const char* src : {"passives", "affixes", "stats", "uniques", "items"}) {
			translation_set_source(src);
			for (size_t i = 0; i < keys.size(); i++) {
				const std::string got = say(keys[i]);
				if (got == bare[i]) continue;
				differ++;
				const bool hadOne = !bare[i].empty() && bare[i] != keys[i];
				const bool hasOne = !got.empty() && got != keys[i];
				if (hadOne && !hasOne && ++lost <= 10)
					printf("    [%s] %s: sourced=%s bare=%s\n", src, keys[i].c_str(),
					       got.c_str(), bare[i].c_str());
			}
			translation_set_source(nullptr);
		}
		printf("  (S16p: %zu keys x 5 sources, %zu differ, %zu lost)\n", keys.size(), differ, lost);
		check(keys.size() > 1000 && lost == 0,
		      "S16p poe2 有旗標：設來源後原本翻得到的仍翻得到");
		const std::string built = translation_debug_sources();
		const char* kPoe2Built = "exact=gems,items,passives,stats,title_words,uniques;"
		                         "pattern=gems,items,passives,stats,title_words,uniques;id=affixes,mod_stats";
		check(built == kPoe2Built,
		      "S22p poe2 有旗標：依 meta 建齊來源表與 id 表");
		if (built != kPoe2Built) printf("    %s\n", built.c_str());
		SetEnvironmentVariableW(L"POB_GAME", L"poe1");
		translation_reload();
	}

	// external dictionary root (POB_ZH_DATADIR): a key existing ONLY there must come back, and a
	// key existing only in the built-ins must be gone -- separates "read the external root" from
	// "merged something extra on top".
	printf("\n=== external dictionary root ===\n");
	{
		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		// The variable points at the folder holding the <locale> sub-folders --
		// i.e. a copy of Data\poe1, not of Data.
		const std::wstring extRoot = std::wstring(tmp) + L"pobtools_ext_dict\\";
		const std::wstring leaf = extRoot + L"zh-rTW\\";
		CreateDirectoryW(extRoot.c_str(), nullptr);
		CreateDirectoryW(leaf.c_str(), nullptr);

		auto put = [](const std::wstring& p, const std::string& body) {
			HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			                       FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) return;
			DWORD w = 0;
			WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr);
			CloseHandle(h);
		};
		const char* kOnlyExternal = "PobToolsExternalRootProbe";
		put(leaf + L"meta.json", "{\"load_order\":[\"probe.json\"]}");
		put(leaf + L"probe.json",
		    std::string("{\"entries\":{\"") + kOnlyExternal + "\":\"\xe5\xa4\x96\xe9\x83\xa8\"}}");

		// A key that only the shipped dictionaries have, to prove the switch.
		const char* kOnlyBuiltin = "Chaos Orb";
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", extRoot.c_str());
		translation_reload();
		const char* extHit = translation_lookup(kOnlyExternal);
		const char* builtinGone = translation_lookup(kOnlyBuiltin);
		check(extHit != nullptr, "T35 the engine reads the dictionaries at POB_ZH_DATADIR");
		check(builtinGone == nullptr, "T36 ...instead of the built-in ones, not on top of them");

		// Pointing it at a folder with no dictionary for this game must fall back
		// rather than leave POB with nothing translated at all.
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", (extRoot + L"nowhere").c_str());
		translation_reload();
		check(translation_lookup(kOnlyBuiltin) != nullptr,
		      "T37 an unusable POB_ZH_DATADIR falls back to the built-in dictionaries");

		SetEnvironmentVariableW(L"POB_ZH_DATADIR", nullptr);
		translation_reload();
		check(translation_lookup(kOnlyExternal) == nullptr &&
		          translation_lookup(kOnlyBuiltin) != nullptr,
		      "T38 clearing the variable returns to the built-in dictionaries");

		DeleteFileW((leaf + L"probe.json").c_str());
		DeleteFileW((leaf + L"meta.json").c_str());
		RemoveDirectoryW(leaf.c_str());
		RemoveDirectoryW(extRoot.c_str());
	}

	// T39-T41: POB's raw item-text tags ("{enchant}{rune}", "{variant:N}", "{range:N}") that no
	// dictionary key has. Without the tag-aware step the glossary translates the TAGS and matches
	// "6% increased" inside "36% increased". The box is saved straight back into the item, so a
	// translated tag corrupts the item.
	printf("\n=== raw item-text tags (poe2) ===\n");
	SetEnvironmentVariableW(L"POB_GAME", L"poe2");
	SetEnvironmentVariableW(L"POB_LOCALE", L"zh-rTW");
	translation_reload();
	{
		const std::string plain = engine_says("36% increased Armour, Evasion and Energy Shield");
		const std::string tagged = engine_says("{enchant}{rune}36% increased Armour, Evasion and Energy Shield");
		check(!plain.empty() && tagged == "{enchant}{rune}" + plain,
		      "T39 {enchant}{rune} stays verbatim and the body translates as if it stood alone");
		const std::string bondedPlain = engine_says("Bonded: +40 to maximum Life");
		const std::string bonded = engine_says("{enchant}{rune}Bonded: +40 to maximum Life");
		check(!bondedPlain.empty() && bonded == "{enchant}{rune}" + bondedPlain,
		      "T40 ...the same for a line the tags used to hide from the dictionary entirely");
		const std::string body = engine_says("+(5-10)% to Critical Damage Bonus");
		const std::string variant = engine_says("{variant:2}{range:0.5}+(5-10)% to Critical Damage Bonus");
		check(!body.empty() && variant == "{variant:2}{range:0.5}" + body,
		      "T41 the unique database's own tags ({variant}/{range}) are left alone too");
	}
	SetEnvironmentVariableW(L"POB_GAME", L"poe1");
	translation_reload();

	// T42-T45: the crafting drop-down's '#' placeholders ("+# to all Attributes (9)"). A number absorbs
	// its sign into the key ("+5 ..." -> "# ...") but a literal '#' did not. The '#' form must come
	// out exactly as the numeric form with '#' for the number.
	printf("\n=== '#' placeholders (crafting drop-down) ===\n");
	{
		auto same_as_number = [&](const char* hashForm, const char* numForm, const char* num, const char* name) {
			const std::string viaNum = engine_says(numForm);
			const std::string viaHash = engine_says(hashForm);
			std::string want = viaNum;
			const size_t p = want.find(num);
			if (p != std::string::npos) want.replace(p, strlen(num), "#");
			check(!viaNum.empty() && viaNum != numForm && viaHash == want, name);
			if (viaHash != want) printf("           got '%s' want '%s'\n", viaHash.c_str(), want.c_str());
		};
		same_as_number("+# to all Attributes", "+5 to all Attributes", "5", "T42 '+#' takes its sign like a number (all Attributes)");
		same_as_number("+#% to Fire Resistance", "+37% to Fire Resistance", "37", "T43 ...and with a percent (Fire Resistance)");
		same_as_number("-#% to Cold Resistance", "-20% to Cold Resistance", "20", "T44 ...and a minus sign");
		// an unsigned '#' that already had its own dictionary entry keeps it
		// (that entry spells the spacing differently from the numeric one, so
		// only "translated, '#' kept" is asserted)
		const std::string unsignedHash = engine_says("#% increased Fire Damage");
		check(!unsignedHash.empty() && unsignedHash != "#% increased Fire Damage" && unsignedHash.find('#') != std::string::npos &&
		          unsignedHash.find("increased") == std::string::npos,
		      "T45 an unsigned '#' that already translated keeps doing so");
	}

	// P8:id 字典檔(*_by_id.json)照列但唯讀;說明不寫「不會被讀取」(source_dicts 的檔不在 load_order 卻會被引擎讀)。
	// 在 %TEMP% 的複本上做:SaveFile 被拒前若誤寫，也只寫到複本。
	printf("\n=== P8 id 字典唯讀 ===\n");
	{
		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		const std::wstring root8 = std::wstring(tmp) + L"pobtools_p8_editor\\";
		const std::wstring leaf8 = root8 + L"zh-rTW\\";
		CreateDirectoryW(root8.c_str(), nullptr);
		CreateDirectoryW(leaf8.c_str(), nullptr);
		const std::wstring src8 = poe1Root + L"zh-rTW\\";
		const wchar_t* idFiles[] = {L"passives_by_id.json", L"affixes_by_id.json",
		                            L"gems_desc_by_id.json", L"stats_by_id.json", L"legion_by_id.json"};
		bool setup = CopyFileW((src8 + L"meta.json").c_str(), (leaf8 + L"meta.json").c_str(), FALSE) &&
		             CopyFileW((src8 + L"ui.json").c_str(), (leaf8 + L"ui.json").c_str(), FALSE);
		std::vector<std::string> present;   // 出貨資料裡實際存在的 id 檔
		for (const wchar_t* f : idFiles) {
			if (GetFileAttributesW((src8 + f).c_str()) == INVALID_FILE_ATTRIBUTES) continue;  // legion_by_id 可能不存在
			if (!CopyFileW((src8 + f).c_str(), (leaf8 + f).c_str(), FALSE)) setup = false;
			present.push_back(narrow(f));
		}
		check(setup && present.size() >= 4, "P8-0 id 字典檔複本已建立(至少 4 個)");

		EditorModel m8 = LoadModel(root8, "zh-rTW");
		// P8-1:照列、標唯讀;一般字典(ui.json)不唯讀
		{
			bool listed = true, ro = true;
			for (const std::string& f : present) {
				const int fi = FindFileIdx(m8, f);
				if (fi < 0) { listed = false; printf("    not listed: %s\n", f.c_str()); continue; }
				if (!m8.files[(size_t)fi].readOnly) { ro = false; printf("    not readOnly: %s\n", f.c_str()); }
			}
			const int ui = FindFileIdx(m8, "ui.json");
			check(listed, "P8-1a id 字典檔仍列在編輯器檔案清單");
			check(ro, "P8-1b id 字典檔標為唯讀");
			check(ui >= 0 && !m8.files[(size_t)ui].readOnly, "P8-1c 一般字典(ui.json)不是唯讀");
		}
		// P8-2:編輯被拒(SetEntry 回 SIZE_MAX、值與 dirty 不變)
		{
			const int fi = FindFileIdx(m8, "passives_by_id.json");
			bool ok = fi >= 0;
			long long at = -1;
			std::string before;
			if (ok) {
				for (size_t i = 0; i < m8.entries.size(); i++)
					if (m8.entries[i].fileIdx == fi) { at = (long long)i; break; }
				ok = at >= 0;
			}
			if (ok) {
				before = m8.entries[(size_t)at].value;
				const std::string key = m8.entries[(size_t)at].key;
				const size_t r = SetEntry(m8, fi, key, before + "X");
				const size_t r2 = SetEntry(m8, fi, "P8NewKeyXyz", u8"新條目");
				ok = r == (size_t)-1 && r2 == (size_t)-1 && m8.entries[(size_t)at].value == before &&
				     FindEntry(m8, fi, "P8NewKeyXyz") < 0 && !m8.files[(size_t)fi].dirty;
				if (!ok) printf("    r=%lld r2=%lld value=%s dirty=%d\n", (long long)r, (long long)r2,
				                m8.entries[(size_t)at].value.c_str(), (int)m8.files[(size_t)fi].dirty);
			}
			check(ok, "P8-2 唯讀檔的編輯與新增被拒(值不變、不變 dirty)");
		}
		// P8-3:存檔被拒(SaveFile 回 false,檔案位元組不變)
		{
			const int fi = FindFileIdx(m8, "affixes_by_id.json");
			bool ok = fi >= 0;
			if (ok) {
				std::string beforeBytes, afterBytes;
				read_file_bytes(m8.files[(size_t)fi].path, beforeBytes);
				m8.files[(size_t)fi].doc["entries"]["P8ForcedKeyXyz"] = "x";   // 繞過 SetEntry 直接改 doc
				m8.files[(size_t)fi].dirty = true;
				std::string err;
				const bool saved = SaveFile(m8.files[(size_t)fi], &err);
				read_file_bytes(m8.files[(size_t)fi].path, afterBytes);
				ok = !saved && !err.empty() && afterBytes == beforeBytes;
				if (!ok) printf("    saved=%d err=%s same=%d\n", (int)saved, err.c_str(), (int)(afterBytes == beforeBytes));
			}
			check(ok, "P8-3 唯讀檔的存檔被拒且檔案不變");
		}
		// P8-4:說明文字不再寫「不會被讀取」;唯讀檔的說明提到唯讀
		{
			bool noNever = true, saysRo = true;
			for (size_t fi = 0; fi < m8.files.size(); fi++) {
				const std::string note = FileStatusNote(m8, (int)fi);
				if (note.find(u8"不會被讀取") != std::string::npos) {
					noNever = false;
					printf("    %s: %s\n", m8.files[fi].name.c_str(), note.c_str());
				}
				if (m8.files[fi].name.find("_by_id.json") != std::string::npos &&
				    note.find(u8"唯讀") == std::string::npos) saysRo = false;
			}
			check(noNever, "P8-4a 檔案說明不再出現「不會被讀取」");
			check(saysRo, "P8-4b id 字典檔的說明標示唯讀");
		}
		// P8-5(review I1):模型層繞過 —— trans_editor_logic 的 SetWithUndo / ApplyState
		// 對已存在條目直接改值，沒看 readOnly。對 *_by_id.json 既有條目走這兩條路，
		// 值、edited、dirty 都不得改變，也不得記一筆 undo。
		{
			const int fi = FindFileIdx(m8, "gems_desc_by_id.json");
			long long at = -1;
			for (size_t i = 0; fi >= 0 && i < m8.entries.size(); i++)
				if (m8.entries[i].fileIdx == fi) { at = (long long)i; break; }
			bool ok = fi >= 0 && at >= 0;
			if (ok) {
				const std::string key = m8.entries[(size_t)at].key;
				const std::string before = m8.entries[(size_t)at].value;
				TransEd::UndoStack undo;
				TransEd::SetWithUndo(m8, undo, fi, key, before + "X");
				const bool viaUndo = m8.entries[(size_t)at].value == before && !m8.entries[(size_t)at].edited &&
				                     !m8.files[(size_t)fi].dirty && undo.Size() == 0;
				TransEd::EntryState st;
				st.exists = true;
				st.value = before + "Y";
				TransEd::ApplyState(m8, fi, key, st);
				const bool viaApply = m8.entries[(size_t)at].value == before && !m8.files[(size_t)fi].dirty;
				ok = viaUndo && viaApply;
				if (!ok) printf("    viaUndo=%d viaApply=%d value=%s dirty=%d undo=%zu\n", (int)viaUndo, (int)viaApply,
				                m8.entries[(size_t)at].value.c_str(), (int)m8.files[(size_t)fi].dirty, undo.Size());
			}
			check(ok, "P8-5 SetWithUndo / ApplyState 不得改唯讀檔的既有條目");
		}
		for (const wchar_t* f : idFiles) {
			DeleteFileW((leaf8 + f).c_str());
			DeleteFileW((leaf8 + f + std::wstring(L".bak")).c_str());
		}
		DeleteFileW((leaf8 + L"meta.json").c_str());
		DeleteFileW((leaf8 + L"ui.json").c_str());
		RemoveDirectoryW(leaf8.c_str());
		RemoveDirectoryW(root8.c_str());
	}

	printf("\neditor selftest: %d passed, %d failed\n", g_pass, g_fail);
	if (g_fail == 0) {
		printf("結論：透過編輯器的修正確實會生效；但同一個英文鍵存在多個檔案時，\n"
		       "      只有 load_order 最後那個檔的那一份有效（T9/T10 已實證）。\n");
	}
	return g_fail == 0 ? 0 : 3;
}
