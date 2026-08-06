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
#include "launcher_config.h"   // ResolveDataDir
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

// T1-T13 for one game. Everything here writes to the REAL dictionaries (edit ->
// verify through the engine -> restore), which is why it stayed hardcoded to poe1:
// running it against poe2 rewrites poe2's files too. Safe now that SaveFile
// preserves each file's original line ending.
//
// `slotRoot` is ALWAYS the built-in <exeDir>Data\<game>\, never a configured
// external folder -- see RunEditorSelftest. A self test that followed the user's
// setting would edit and re-save a translator's working copy.
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

	// The loader takes its locale from POB_GAME / POB_LOCALE, which host_main
	// sets before launching POB. A headless run inherits neither, and without
	// them determine_locale() returns empty and the loader silently enters
	// passthrough mode -- every lookup then returns nothing and every
	// comparison below "passes" for the wrong reason. Set them the same way
	// the launcher does, and prove the loader actually read the files.
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
		EditorModel reload = LoadModel(slotRoot, "zh-rTW");
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

	// --- round-trip fidelity ----------------------------------------------
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

	// --- load order, exposed on the model ----------------------------------
	// The editor now reads meta.json's load_order itself so its file pickers can
	// say which copy of a key the engine will actually use. T24-T26 check that
	// shortcut against the full merge replay, and against the engine.
	{
		check(!model.loadOrder.empty(), "T24 model exposes meta.json load_order");

		std::vector<int> byOrder = FileIdxInLoadOrder(model);
		bool sameSize = byOrder.size() == model.files.size();
		bool ascending = true;
		int prev = -1;
		for (int fi : byOrder) {
			const int o = model.files[(size_t)fi].order;
			if (o < 0) continue;              // unlisted files sort last
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
			if (kv.second < 2) continue;      // only keys that actually collide
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

	// --- line endings survive a save ---------------------------------------
	// Two poe1 dictionaries were silently converted CRLF->LF by an earlier
	// version of SaveFile. Copies, so the real files are never at risk.
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

	// --- adding a brand-new key reaches the engine -------------------------
	// The add-entry row is the only way to translate a string the engine has
	// never logged as missing. SetEntry has no delete path, so the file is
	// restored from a byte snapshot rather than by editing it back.
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

	// --- locale enumeration -------------------------------------------------
	{
		std::vector<std::string> l1 = ListLocales(BuiltinDictDir(exeDir, DictSlot::Poe1));
		std::vector<std::string> l2 = ListLocales(BuiltinDictDir(exeDir, DictSlot::Poe2));
		bool has1 = std::find(l1.begin(), l1.end(), std::string("zh-rTW")) != l1.end();
		bool has2 = std::find(l2.begin(), l2.end(), std::string("zh-rTW")) != l2.end();
		std::vector<std::string> lx = ListLocales(exeDir + L"Data\\poe9\\");
		check(has1 && has2, "T30 ListLocales finds zh-rTW for both games");
		check(!lx.empty(), "T31 ListLocales falls back instead of returning nothing");

		// The launcher's own labels are a dictionary of the same shape, so the
		// editor reaches them with no special-case code -- that is the entire
		// reason they were put under Data\launcher\<locale>\ rather than in a
		// settings file of their own.
		const std::wstring launcherRoot = BuiltinDictDir(exeDir, DictSlot::Launcher);
		std::vector<std::string> ll = ListLocales(launcherRoot);
		EditorModel lm = LoadModel(launcherRoot, "zh-rTW");
		check(std::find(ll.begin(), ll.end(), std::string("zh-rTW")) != ll.end(),
		      "T32 ListLocales sees the launcher-strings dictionary");
		check(lm.localeExists && lm.files.size() == 1 && lm.entries.size() >= 40 &&
		          lm.loadOrder.size() == 1 && lm.loadOrder[0] == "launcher.json",
		      "T33 the launcher dictionary loads like any other");

		// T33c -- a language nobody hardcoded. Dropping a folder in is the whole
		// point, so it is exercised end to end (list it, load it, remove it)
		// rather than asserted about zh-rTW, which proves nothing about "automatic".
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
			// Litter in the shipping Data\ directory is exactly what nobody notices.
			check(GetFileAttributesW(probe.c_str()) == INVALID_FILE_ATTRIBUTES,
			      "T33d the probe language folder was removed again");
		}

		// T33b -- which rows get the expanded editor. The About paragraph is the
		// case that prompted it (four lines crammed into a one-line box), and a
		// short label is the case that must NOT get a button cluttering it.
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
	// The entries table is an ImGuiListClipper over 110k rows and the clipper
	// sizes the whole scroll range from ONE row's height, so a single taller row
	// shortens the range and the rows past it cannot be reached. That is not
	// hypothetical: ui.json has a key 25 lines long, and one 3-line key inside a
	// 106-row filter was enough to strand the last entries off the bottom.
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
		// `folded > 0` is the part that keeps this honest: without it the sweep
		// would pass just as happily on a dictionary that has no multi-line key
		// at all, and prove nothing about the case it exists for.
		const std::string msg = "T33c2 no dictionary entry can draw a multi-line cell (" +
		                        std::to_string(folded) + " folded)";
		check(worst.empty() && folded > 0, msg.c_str());
	}

	// SaveFile backs a file up before overwriting it, so a run that edits and
	// restores four files leaves four .bak copies behind. Every value is already
	// restored and verified above, so the backups are litter -- and litter in the
	// shipping Data\ directory is exactly the kind of thing nobody notices.
	for (const EditorFile& f : model.files)
		DeleteFileW((f.path + L".bak").c_str());

	return 0;
}

int RunEditorSelftest(const std::string& games)
{
	g_pass = g_fail = 0;
	const std::wstring dir = exe_dir();

	// PINNED to the shipped dictionaries by passing an empty configured path.
	// This test edits real files and restores them; if it followed the user's
	// DataDir setting it would be rewriting a translator's working copy, and a
	// mistake there has no backup (dist\Data is not under version control).
	const std::wstring poe1Root = ResolveDictDir(dir, DictSlot::Poe1, L"").root;
	const std::wstring poe2Root = ResolveDictDir(dir, DictSlot::Poe2, L"").root;
	printf("editor selftest - dictionary folders: %s | %s\n",
	       narrow(poe1Root).c_str(), narrow(poe2Root).c_str());
	printf("  NOTE: this edits the dictionaries next to the exe (edit -> verify -> restore)\n");
	// Prove the pinning rather than trusting the comment: even with an external
	// folder configured, the ones used here must still be the built-in ones.
	check(poe1Root == BuiltinDictDir(dir, DictSlot::Poe1) &&
	          poe2Root == BuiltinDictDir(dir, DictSlot::Poe2),
	      "T34 selftest is pinned to the built-in dictionaries");

	const bool doPoe1 = (games != "poe2");
	const bool doPoe2 = (games != "poe1");
	if (doPoe1 && run_for_game(dir, poe1Root, "poe1") == 2) return 2;
	if (doPoe2 && run_for_game(dir, poe2Root, "poe2") == 2) return 2;

	// Everything below is poe1-only: it asserts the Volatility gem/passive
	// collision, and poe2's dictionary has no such data. Printed, not skipped
	// silently, so "did not run" stays visible.
	printf("\n=== source dictionaries (poe1 only) ===\n");
	SetEnvironmentVariableW(L"POB_GAME", L"poe1");
	SetEnvironmentVariableW(L"POB_LOCALE", L"zh-rTW");
	translation_reload();
	EditorModel model = LoadModel(poe1Root, "zh-rTW");
	std::vector<std::string> order = load_order(model);
	(void)order;

	// --- source dictionaries (issue #3) ------------------------------------
	// POB stores support gems under their SHORT name, so "Volatility" is both a
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

	// --- external dictionary root (POB_ZH_DATADIR) --------------------------
	//
	// Asks the REAL loader, not a re-implementation: build a throwaway dictionary
	// somewhere else, point the variable at it, and see whether a key that exists
	// ONLY there comes back. A key that exists only in the built-in dictionaries
	// must then be gone -- that is what separates "read the external root" from
	// "merged something extra on top", which would look identical otherwise.
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

	printf("\neditor selftest: %d passed, %d failed\n", g_pass, g_fail);
	if (g_fail == 0) {
		printf("結論：透過編輯器的修正確實會生效；但同一個英文鍵存在多個檔案時，\n"
		       "      只有 load_order 最後那個檔的那一份有效（T9/T10 已實證）。\n");
	}
	return g_fail == 0 ? 0 : 3;
}
