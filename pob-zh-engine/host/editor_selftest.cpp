// Headless check that an edit made through the translation editor actually
// reaches the engine.
//
// The question this answers is not "does SaveFile write bytes" -- it is "after
// I change a string in the editor, does POB show it?". Those are different
// questions because the engine merges the eight dictionary files into ONE flat
// map and lets a later file overwrite an earlier one (translation_manager.cpp,
// order from meta.json). 12,254 English keys live in more than one file, so an
// edit applied to the copy that loses the merge changes nothing on screen and
// the editor gives no hint of it.
//
// So the test drives the real data layer (editor_data.cpp) and then asks the
// real loader (translation_init + translation_lookup) what the engine would
// return -- including the shadowed case, which must be demonstrated rather
// than assumed.
//
// Runs against whatever Data\ sits next to the exe. Point it at a scratch
// deployment (copy pob-zh.exe + Data into a temp dir) so the shipping
// dictionaries are never touched; the test still restores every value it
// changes, and verifies the restore.

#include "editor_selftest.h"

#include <windows.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "editor_data.h"
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

std::vector<std::string> load_order(const EditorModel& model)
{
	std::vector<std::string> order;
	for (const EditorFile& f : model.files) {
		if (f.name != "meta.json") continue;
		if (f.doc.contains("load_order") && f.doc["load_order"].is_array()) {
			for (const auto& x : f.doc["load_order"])
				if (x.is_string()) order.push_back(x.get<std::string>());
		}
	}
	if (order.empty()) {
		// meta.json is filtered out of the model (it has no "entries"), so read
		// it directly rather than guessing an order
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
				if (doc.contains("load_order"))
					for (const auto& x : doc["load_order"])
						if (x.is_string()) order.push_back(x.get<std::string>());
			} catch (...) {}
		}
	}
	return order;
}

// What the engine returns for `key` after a fresh load of the dictionaries.
std::string engine_says(const char* key)
{
	translation_reload();
	const char* r = translation_lookup(key);
	return r ? std::string(r) : std::string();
}

} // namespace

int RunEditorSelftest()
{
	g_pass = g_fail = 0;
	const std::wstring dir = exe_dir();
	printf("editor selftest — data dir next to: %s\n", narrow(dir).c_str());

	EditorModel model = LoadModel(dir, "poe1", "zh-rTW");
	if (!model.localeExists || model.files.empty()) {
		printf("  [FAIL]  locale dir not found or no dictionary files\n");
		return 2;
	}
	printf("  loaded %zu files, %zu entries\n", model.files.size(), model.entries.size());
	check(model.entries.size() > 1000, "T1 model loads the dictionaries");

	// The loader takes its locale from POB_GAME / POB_LOCALE, which host_main
	// sets before launching POB. A headless run inherits neither, and without
	// them determine_locale() returns empty and the loader silently enters
	// passthrough mode -- every lookup then returns nothing and every
	// comparison below "passes" for the wrong reason. Set them the same way
	// the launcher does, and prove the loader actually read the files.
	SetEnvironmentVariableW(L"POB_GAME", L"poe1");
	SetEnvironmentVariableW(L"POB_LOCALE", L"zh-rTW");
	translation_reload();
	printf("  engine: locale=%s entries=%d\n",
	       translation_get_locale() ? translation_get_locale() : "(null)",
	       translation_get_count());
	check(translation_get_count() > 1000, "T1b engine loaded the same dictionaries");

	std::vector<std::string> order = load_order(model);
	check(!order.empty(), "T2 meta.json load_order readable");
	std::map<std::string, std::string> winner = merge_winner(model, order);

	// --- pick two subjects -------------------------------------------------
	// (a) a key that lives in exactly one file  -> editing it must take effect
	// (b) a key that lives in several files     -> editing the LOSING copy must
	//     not take effect, editing the winner must
	std::map<std::string, int> occurrences;
	for (const EditorEntry& e : model.entries) occurrences[e.key]++;

	const EditorEntry* single = nullptr;
	const EditorEntry* shadowLoser = nullptr;
	const EditorEntry* shadowWinner = nullptr;
	for (const EditorEntry& e : model.entries) {
		if (e.structured) continue;
		const std::string& owner = model.files[(size_t)e.fileIdx].name;
		if (!single && occurrences[e.key] == 1 && e.key.size() > 6 && e.value.size() > 2)
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

	// --- (a) single-file key: edit must reach the engine --------------------
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
		std::string err;
		int saved = SaveAll(model, &err);
		check(saved > 0, "T6 SaveAll writes the edited file");

		std::string after = engine_says(key.c_str());
		check(after == sentinel + before,
		      "T7 **edit through the editor reaches the engine**");

		// restore and prove the restore worked
		EditorModel reload = LoadModel(dir, "poe1", "zh-rTW");
		int ridx = FindFileIdx(reload, model.files[(size_t)fidx].name);
		SetEntry(reload, ridx, key, before);
		SaveAll(reload, &err);
		check(engine_says(key.c_str()) == before, "T8 restore verified");
	}

	// --- (b) shadowed key: editing the loser must NOT take effect -----------
	if (shadowLoser && shadowWinner) {
		const std::string key = shadowLoser->key;
		const std::string loserFile = model.files[(size_t)shadowLoser->fileIdx].name;
		const std::string winnerFile = model.files[(size_t)shadowWinner->fileIdx].name;
		printf("  shadowed key %s: in %s (loses) and %s (wins)\n",
		       key.c_str(), loserFile.c_str(), winnerFile.c_str());

		EditorModel m2 = LoadModel(dir, "poe1", "zh-rTW");
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
		EditorModel m3 = LoadModel(dir, "poe1", "zh-rTW");
		int wi = FindFileIdx(m3, winnerFile);
		SetEntry(m3, wi, key, sentinel + winnerValue);
		SaveAll(m3, &err);
		check(engine_says(key.c_str()) == sentinel + winnerValue,
		      "T10 editing the WINNING copy does take effect");

		// restore both
		EditorModel m4 = LoadModel(dir, "poe1", "zh-rTW");
		SetEntry(m4, FindFileIdx(m4, loserFile), key, loserBefore);
		SetEntry(m4, FindFileIdx(m4, winnerFile), key, winnerValue);
		SaveAll(m4, &err);
		check(engine_says(key.c_str()) == winnerValue, "T11 restore verified");
	}

	// --- round-trip fidelity ----------------------------------------------
	{
		EditorModel m = LoadModel(dir, "poe1", "zh-rTW");
		bool keysKept = true, metaKept = true;
		for (const EditorFile& f : m.files) {
			if (!f.doc.contains("entries")) { keysKept = false; break; }
			// source_files / is_base_items must survive a save untouched
			if (f.name == "items.json" && !f.doc.contains("source_files")) metaKept = false;
		}
		check(keysKept, "T12 every file still has its entries object");
		check(metaKept, "T13 non-entries fields survived the round trip");
	}

	printf("\neditor selftest: %d passed, %d failed\n", g_pass, g_fail);
	if (g_fail == 0) {
		printf("結論：透過編輯器的修正確實會生效；但同一個英文鍵存在多個檔案時，\n"
		       "      只有 load_order 最後那個檔的那一份有效（T9/T10 已實證）。\n");
	}
	return g_fail == 0 ? 0 : 3;
}
