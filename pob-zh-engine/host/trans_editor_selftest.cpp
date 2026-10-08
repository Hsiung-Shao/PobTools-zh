// --trans-editor-selftest: the translation editor's decisions, headless.
//
//   E1  the add-entry notice (new / overwrite / shadowed / whitespace, plus the
//       "another file has it but yours wins" case), on a synthetic model and on
//       the shipped dictionaries against a full load-order replay
//   E2  the undo stack: several steps, back to the original value, additions
//   E3  the multi-line editor's three checks
//   E4  filling a missing string, filling all, withdrawing, and undoing both
//   E5  saving changes exactly the edited bytes (every shipped dictionary
//       round-trips byte for byte; one edit changes one line; an addition keeps
//       every existing key where it was)
//   E6  "only modified": the count, and a value typed back is not modified
//   E7  the panel itself: switching dictionary / language, reloading and saving
//       empty the undo stack, through the same entry points the UI uses
//   E8  the missing-strings scan: MISS is a forward miss, REV / FLAVOUR /
//       PROPERTY are paste-side lines
//
// Never touches the dictionaries next to the exe: everything that writes works on
// a copy in %TEMP%, and the originals are hashed before and after to prove it.

#include "trans_editor_logic.h"
#include "editor_data.h"
#include "launcher_editor.h"
#include "tool_panel.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

struct Report {
	std::string text;
	int pass = 0, fail = 0;
	void check(const std::string& what, bool ok, const std::string& detail = std::string())
	{
		(ok ? pass : fail)++;
		text += (ok ? "PASS " : "FAIL ") + what + (detail.empty() ? "" : "  [" + detail + "]") + "\n";
	}
	void line(const std::string& s) { text += s + "\n"; }
};

std::string narrow(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

bool readAll(const std::wstring& p, std::string& out)
{
	out.clear();
	HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	char buf[1 << 16];
	DWORD got = 0;
	while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
	CloseHandle(h);
	return true;
}

bool writeAll(const std::wstring& p, const std::string& data)
{
	HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD w = 0;
	const bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size());
	CloseHandle(h);
	return ok;
}

unsigned long long fnv(const std::string& s)
{
	unsigned long long h = 1469598103934665603ull;
	for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
	return h;
}

std::vector<std::wstring> listFiles(const std::wstring& dir, const wchar_t* pattern)
{
	std::vector<std::wstring> out;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + pattern).c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return out;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	std::sort(out.begin(), out.end());
	return out;
}

bool copyDir(const std::wstring& from, const std::wstring& to)
{
	CreateDirectoryW(to.c_str(), nullptr);
	bool ok = true;
	for (const std::wstring& f : listFiles(from, L"*.json"))
		if (!CopyFileW((from + f).c_str(), (to + f).c_str(), FALSE)) ok = false;
	return ok;
}

void removeTree(const std::wstring& dir)
{
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring n = fd.cFileName;
			if (n == L"." || n == L"..") continue;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(dir + n + L"\\");
			else {
				SetFileAttributesW((dir + n).c_str(), FILE_ATTRIBUTE_NORMAL);
				DeleteFileW((dir + n).c_str());
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

// What nlohmann would write for a string (the dictionary format).
std::string jsonStr(const std::string& s) { return nlohmann::ordered_json(s).dump(-1, ' ', false); }

// A model built by hand: a(0) b(1) c(2) listed, d unlisted.
EditorModel synth()
{
	EditorModel m;
	m.localeExists = true;
	const char* names[] = { "a.json", "b.json", "c.json", "d.json" };
	const int orders[] = { 0, 1, 2, -1 };
	for (int i = 0; i < 4; i++) {
		EditorFile f;
		f.name = names[i];
		f.order = orders[i];
		f.doc = nlohmann::ordered_json::parse("{\"entries\":{}}");
		m.files.push_back(f);
	}
	m.loadOrder = { "a.json", "b.json", "c.json" };
	auto add = [&](int fi, const char* k, const char* v) {
		EditorEntry e;
		e.key = k;
		e.value = v;
		e.orig = v;
		e.fileIdx = fi;
		m.entries.push_back(e);
	};
	add(0, "K1", u8"甲一");
	add(2, "K1", u8"丙一");
	add(1, "K2", u8"乙二");
	add(3, "K4", u8"丁四");
	return m;
}

} // namespace

int RunTransEditorSelftest(const std::wstring& exeDir)
{
	using namespace TransEd;
	Report rep;
	rep.line("trans-editor selftest");

	// ---- E1 add-entry notice ----------------------------------------------------
	{
		EditorModel m = synth();
		AddHint h;
		h = ClassifyAdd(m, "K3", 1);
		rep.check("E1a a key no file has is New", h.kind == AddKind::New && h.owners.empty());
		h = ClassifyAdd(m, "K1", 2);
		rep.check("E1b the target already has it: OverwriteSame, with the old value",
		          h.kind == AddKind::OverwriteSame && h.oldValue == u8"丙一");
		h = ClassifyAdd(m, "K1", 0);
		rep.check("E1c a later-loading file has it: Shadowed, winner c.json",
		          h.kind == AddKind::Shadowed && h.winner == 2);
		h = ClassifyAdd(m, "K1", 1);
		rep.check("E1d b.json loads before c.json: also Shadowed", h.kind == AddKind::Shadowed && h.winner == 2);
		h = ClassifyAdd(m, "K2", 2);
		rep.check("E1e another file has it but the target loads later: TargetWins",
		          h.kind == AddKind::TargetWins && h.owners.size() == 1 && h.owners[0] == 1);
		h = ClassifyAdd(m, "K2", 3);
		rep.check("E1f an unlisted target never wins", h.kind == AddKind::Shadowed && h.winner == 1);
		h = ClassifyAdd(m, " K3", 1);
		rep.check("E1g leading space: Whitespace", h.kind == AddKind::Whitespace);
		rep.check("E1h TrimKey strips spaces, tabs, CR/LF and U+3000",
		          TrimKey(" \tK3\r\n") == "K3" && TrimKey(u8"　K3　") == "K3" && TrimKey("K 3") == "K 3" &&
		              TrimKey("   ").empty());
		rep.check("E1i empty key: Empty", ClassifyAdd(m, "", 0).kind == AddKind::Empty);
	}
	// against the shipped dictionaries: Shadowed exactly when a full replay of
	// load_order says another file's copy is the one POB uses
	{
		const std::wstring slot = exeDir + L"Data\\poe1\\";
		EditorModel m = LoadModel(slot, "zh-rTW");
		std::map<std::string, std::string> winner;
		for (const std::string& fname : m.loadOrder)
			for (const EditorEntry& e : m.entries)
				if (m.files[(size_t)e.fileIdx].name == fname) winner[e.key] = fname;
		std::map<std::string, int> occ;
		for (const EditorEntry& e : m.entries) occ[e.key]++;
		int checked = 0, wrong = 0;
		std::string first;
		for (const auto& kv : occ) {
			if (kv.second < 2 || checked >= 150) continue;   // ClassifyAdd scans every row
			const auto w = winner.find(kv.first);
			if (w == winner.end()) continue;
			for (size_t fi = 0; fi < m.files.size(); fi++) {
				if (m.files[fi].order < 0) continue;
				const AddHint h = ClassifyAdd(m, kv.first, (int)fi);
				const bool shadowedByReplay = m.files[fi].name != w->second &&
				                              m.files[fi].order < (int)(std::find(m.loadOrder.begin(), m.loadOrder.end(), w->second) - m.loadOrder.begin());
				const bool shadowed = h.kind == AddKind::Shadowed;
				if (shadowed != shadowedByReplay || (shadowed && m.files[(size_t)h.winner].name != w->second)) {
					if (!wrong) first = kv.first + " -> " + m.files[fi].name;
					wrong++;
				}
			}
			checked++;
		}
		rep.check("E1j on poe1 zh-rTW, Shadowed agrees with a load-order replay", m.entries.size() > 1000 && checked >= 150 && wrong == 0,
		          std::to_string(checked) + " shared keys x every listed file, " + std::to_string(wrong) + " wrong" +
		              (first.empty() ? "" : ", first " + first));
	}

	// ---- E2 undo ------------------------------------------------------------------
	{
		EditorModel m = synth();
		UndoStack u;
		const size_t n0 = m.entries.size();
		rep.check("E2a nothing modified at the start", CountModified(m) == 0 && DirtyCount(m) == 0);
		SetWithUndo(m, u, 1, "K2", u8"乙二改");
		SetWithUndo(m, u, 1, "K2", u8"乙二再改");
		const bool reshaped = SetWithUndo(m, u, 0, "K9", u8"新的");
		rep.check("E2b three steps; an addition reports a reshaped list",
		          u.Size() == 3 && reshaped && m.entries.size() == n0 + 1 && CountModified(m) == 2 && DirtyCount(m) == 2);
		rep.check("E2c a no-op edit records nothing", !SetWithUndo(m, u, 1, "K2", u8"乙二再改") && u.Size() == 3);
		bool r = false;
		u.Undo(m, nullptr, &r);
		rep.check("E2d undo 1: the added key is gone again", r && m.entries.size() == n0 && FindEntry(m, 0, "K9") < 0 &&
		                                                          DirtyCount(m) == 1);
		u.Undo(m, nullptr, &r);
		rep.check("E2e undo 2: the first edit's value is back", StateOf(m, 1, "K2").value == u8"乙二改" && CountModified(m) == 1);
		u.Undo(m, nullptr, &r);
		rep.check("E2f undo 3: back to the original -- no longer modified, file clean",
		          StateOf(m, 1, "K2").value == u8"乙二" && CountModified(m) == 0 && DirtyCount(m) == 0 && u.Empty());
		rep.check("E2g undo on an empty stack does nothing", !u.Undo(m, nullptr, &r));
		// typing a value back to what it was is not a change either
		SetEntry(m, 1, "K2", "x");
		const bool was = m.entries[(size_t)FindEntry(m, 1, "K2")].edited;
		SetEntry(m, 1, "K2", u8"乙二");
		rep.check("E2h a value set back to the original stops counting", was && CountModified(m) == 0 && DirtyCount(m) == 0);
		// the stack is bounded
		UndoStack big;
		for (int i = 0; i < (int)UndoStack::kMax + 20; i++) SetWithUndo(m, big, 1, "K2", "v" + std::to_string(i));
		rep.check("E2i the stack keeps at most kMax steps", big.Size() == UndoStack::kMax);
	}

	// ---- E3 checks --------------------------------------------------------------
	{
		const std::string key = "^xE05030Life Regeneration^7 cannot be modified\nwhile you are Cursed";
		TextChecks c = CheckTranslation(key, u8"^xe05030生命再生^7無法被修改\n當你受到詛咒時");
		rep.check("E3a matching colours (hex case ignored), lines, no placeholders: all pass", c.AllOk() && c.colourKey == 2 &&
		                                                                                         c.linesKey == 2 && c.linesValue == 2);
		c = CheckTranslation(key, u8"^xE05030生命再生無法被修改\n當你受到詛咒時");
		rep.check("E3b a dropped ^7 fails the colour check and names it",
		          !c.colourOk && c.colourMissing.size() == 1 && c.colourMissing[0] == "^7" && c.linesOk);
		c = CheckTranslation("Life", u8"^x33FF77生命");
		rep.check("E3c a key without colours: a translation may add its own", c.colourOk);
		c = CheckTranslation("one\\ntwo", u8"一\n二");
		rep.check("E3d the two characters \\n count as a line break", c.linesOk && c.linesKey == 2);
		c = CheckTranslation(key, u8"^xE05030生命再生^7無法被修改");
		rep.check("E3e one line short: lines 1 / 2", !c.linesOk && c.linesValue == 1 && c.linesKey == 2);
		c = CheckTranslation("{0}% increased {1}", u8"{1} 提高 {0}%");
		rep.check("E3f reordered placeholders are consistent", c.placeholdersOk && c.placeholdersKey.size() == 2);
		c = CheckTranslation("{0}% increased {1}", u8"提高 {0}%");
		rep.check("E3g a missing {1} is reported", !c.placeholdersOk && c.placeholdersMissing.size() == 1 &&
		                                                c.placeholdersMissing[0] == "{1}");
		c = CheckTranslation("{0} more", u8"{0:+d} 多 {2}");
		rep.check("E3h {0:+d} matches {0}; an extra {2} is reported",
		          !c.placeholdersOk && c.placeholdersMissing.empty() && c.placeholdersExtra.size() == 1 &&
		              c.placeholdersExtra[0] == "{2}");
		rep.check("E3i a bare caret or broken ^x is not a colour", ColourEscapes("50^ ^xZZ0000 ^5").size() == 1);
	}

	// ---- E4 fill / withdraw -------------------------------------------------------
	{
		EditorModel m = synth();
		UndoStack u;
		std::vector<MissEntry> misses(4);
		misses[0].text = "New A"; misses[0].tag = "MISS";
		misses[1].text = u8"中文行"; misses[1].reverse = true; misses[1].tag = "REV";
		misses[2].text = "New B"; misses[2].tag = "MISS";
		misses[3].text = "K2"; misses[3].tag = "MISS";   // a key b.json already has
		std::vector<MissRow> rows(4);
		for (MissRow& r : rows) r.target = 1;
		rows[0].trans = u8"新甲";
		rows[1].trans = u8"不該寫";
		rows[3].trans = u8"乙二覆蓋";
		bool sh = false;
		rep.check("E4a a paste-side (REV) row is never written as a key", !FillMiss(m, u, misses, rows, 1, &sh) &&
		                                                                     FindEntry(m, 1, u8"中文行") < 0);
		rep.check("E4b an empty translation is not filled", !FillMiss(m, u, misses, rows, 2, &sh));
		const int n = FillAllMisses(m, u, misses, rows, &sh);
		rep.check("E4c fill all: the two typed forward rows, as ONE step",
		          n == 2 && u.Size() == 1 && rows[0].filled && rows[3].filled && !rows[1].filled && !rows[2].filled &&
		              StateOf(m, 1, "New A").value == u8"新甲" && StateOf(m, 1, "K2").value == u8"乙二覆蓋");
		WithdrawMiss(m, u, misses, rows, 3, &sh);
		rep.check("E4d withdraw puts the overwritten value back", !rows[3].filled && StateOf(m, 1, "K2").value == u8"乙二" &&
		                                                              u.Size() == 2);
		WithdrawMiss(m, u, misses, rows, 0, &sh);
		rep.check("E4e withdrawing an added key removes it", !rows[0].filled && FindEntry(m, 1, "New A") < 0 && sh);
		UndoInto(m, u, rows, &sh);
		rep.check("E4f undoing the withdrawal fills again", rows[0].filled && StateOf(m, 1, "New A").value == u8"新甲");
		UndoInto(m, u, rows, &sh);
		UndoInto(m, u, rows, &sh);
		rep.check("E4g undoing back to the start: nothing filled, nothing modified",
		          !rows[0].filled && !rows[3].filled && FindEntry(m, 1, "New A") < 0 && CountModified(m) == 0 && u.Empty());
		rows[2].trans = u8"新乙";
		FillMiss(m, u, misses, rows, 2, &sh);
		rep.check("E4h a single fill is one step with its mark", u.Size() == 1 && rows[2].filled && rows[2].filledFile == 1);
	}

	// ---- the scratch copy ---------------------------------------------------------
	wchar_t tmpBuf[MAX_PATH] = L"";
	GetTempPathW(MAX_PATH, tmpBuf);
	const std::wstring root = std::wstring(tmpBuf) + L"pobtools_te_selftest_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
	removeTree(root);
	CreateDirectoryW(root.c_str(), nullptr);
	CreateDirectoryW((root + L"Data\\").c_str(), nullptr);
	struct Src { const wchar_t* slot; const wchar_t* locale; };
	const Src srcs[] = { { L"poe1", L"zh-rTW" }, { L"poe2", L"zh-rTW" }, { L"launcher", L"zh-rTW" } };
	std::map<std::wstring, unsigned long long> origHash;   // the real files, before
	bool copied = true;
	for (const Src& s : srcs) {
		const std::wstring from = exeDir + L"Data\\" + s.slot + L"\\" + s.locale + L"\\";
		const std::wstring to = root + L"Data\\" + s.slot + L"\\" + s.locale + L"\\";
		CreateDirectoryW((root + L"Data\\" + s.slot + L"\\").c_str(), nullptr);
		for (const std::wstring& f : listFiles(from, L"*.json")) {
			std::string b;
			if (readAll(from + f, b)) origHash[from + f] = fnv(b);
		}
		if (!copyDir(from, to)) copied = false;
	}
	rep.check("E5- scratch copy of poe1 / poe2 / launcher zh-rTW", copied && origHash.size() > 10, narrow(root));

	// ---- E5 byte-exact saving -------------------------------------------------------
	{
		// every shipped dictionary file, saved unchanged, comes back byte for byte
		int files = 0, same = 0;
		std::string firstDiff;
		for (const Src& s : srcs) {
			const std::wstring dir = root + L"Data\\" + s.slot + L"\\" + s.locale + L"\\";
			EditorModel m = LoadModel(root + L"Data\\" + s.slot + L"\\", narrow(s.locale));
			for (EditorFile& f : m.files) {
				std::string before;
				readAll(f.path, before);
				EditorFile probe = f;
				probe.path = f.path + L".rt";
				std::string err;
				std::string after;
				if (SaveFile(probe, &err)) readAll(probe.path, after);
				files++;
				if (after == before) same++;
				else if (firstDiff.empty()) firstDiff = narrow(f.path);
				DeleteFileW(probe.path.c_str());
				DeleteFileW((probe.path + L".bak").c_str());
			}
		}
		rep.check("E5a every dictionary file round-trips byte for byte", files > 10 && same == files,
		          std::to_string(same) + "/" + std::to_string(files) + (firstDiff.empty() ? "" : " first diff " + firstDiff));

		// one edit: exactly that value's bytes change, other files untouched
		const std::wstring slot = root + L"Data\\poe1\\";
		const std::wstring dir = slot + L"zh-rTW\\";
		std::map<std::wstring, std::string> before;
		for (const std::wstring& f : listFiles(dir, L"*.json")) readAll(dir + f, before[f]);
		EditorModel m = LoadModel(slot, "zh-rTW");
		const int ui = FindFileIdx(m, "ui.json");
		std::map<std::string, int> occ;
		for (const EditorEntry& e : m.entries) occ[e.key]++;
		long long pick = -1;
		for (size_t i = 0; i < m.entries.size() && pick < 0; i++) {
			const EditorEntry& e = m.entries[i];
			if (e.fileIdx == ui && !e.structured && occ[e.key] == 1 && e.key.size() > 8 && !e.value.empty() &&
			    e.value.find('\n') == std::string::npos && e.value.find('"') == std::string::npos)
				pick = (long long)i;
		}
		bool exact = false, othersSame = true, crlfKept = true;
		std::string detail;
		if (pick >= 0) {
			const std::string key = m.entries[(size_t)pick].key, oldV = m.entries[(size_t)pick].value;
			const std::string newV = oldV + u8"（自我測試）";
			SetEntry(m, ui, key, newV);
			std::string err;
			const int saved = SaveAll(m, &err);
			std::string after;
			readAll(dir + L"ui.json", after);
			const std::string& orig = before[L"ui.json"];
			const std::string needle = jsonStr(key) + ": " + jsonStr(oldV);
			const size_t at = orig.find(needle);
			std::string expect = orig;
			if (at != std::string::npos && orig.find(needle, at + 1) == std::string::npos)
				expect.replace(at + jsonStr(key).size() + 2, jsonStr(oldV).size(), jsonStr(newV));
			exact = saved == 1 && at != std::string::npos && after == expect;
			detail = "key " + key.substr(0, 40);
			for (const auto& kv : before) {
				if (kv.first == L"ui.json") continue;
				std::string now;
				readAll(dir + kv.first, now);
				if (now != kv.second) { othersSame = false; detail += " changed " + narrow(kv.first); }
			}
			const bool wasCrlf = orig.find("\r\n") != std::string::npos;
			if (wasCrlf) crlfKept = std::count(after.begin(), after.end(), '\r') == std::count(after.begin(), after.end(), '\n');
		}
		rep.check("E5b one edit: ui.json differs from the original in exactly that value", pick >= 0 && exact, detail);
		rep.check("E5c the other dictionary files are untouched", othersSame);
		rep.check("E5d line endings kept", crlfKept);
		DeleteFileW((dir + L"ui.json.bak").c_str());

		// an addition goes at the end of "entries"; every existing key keeps its place
		{
			EditorModel m2 = LoadModel(slot, "zh-rTW");
			const int ti = FindFileIdx(m2, "ui.json");
			std::vector<std::string> keysBefore;
			for (auto& [k, v] : m2.files[(size_t)ti].doc["entries"].items()) { (void)v; keysBefore.push_back(k); }
			SetEntry(m2, ti, "__pobtools_te_selftest__", u8"新增");
			std::string err;
			SaveAll(m2, &err);
			std::string after;
			readAll(dir + L"ui.json", after);
			std::vector<std::string> keysAfter;
			try {
				auto doc = nlohmann::ordered_json::parse(after);
				for (auto& [k, v] : doc["entries"].items()) { (void)v; keysAfter.push_back(k); }
			} catch (...) {}
			const bool order = keysAfter.size() == keysBefore.size() + 1 &&
			                   std::equal(keysBefore.begin(), keysBefore.end(), keysAfter.begin()) &&
			                   keysAfter.back() == "__pobtools_te_selftest__";
			rep.check("E5e an added key is appended; no existing key moves", order,
			          std::to_string(keysBefore.size()) + " -> " + std::to_string(keysAfter.size()));
			DeleteFileW((dir + L"ui.json.bak").c_str());
		}
	}

	// ---- E6 only modified -----------------------------------------------------------
	{
		EditorModel m = LoadModel(root + L"Data\\poe2\\", "zh-rTW");
		UndoStack u;
		int done = 0;
		std::vector<int> files;
		for (size_t i = 0; i < m.entries.size() && done < 3; i++) {
			const EditorEntry& e = m.entries[i];
			if (e.structured) continue;
			if (done == 2 && std::find(files.begin(), files.end(), e.fileIdx) != files.end()) continue;
			if (done < 2 && !files.empty() && files[0] != e.fileIdx) continue;
			files.push_back(e.fileIdx);
			SetWithUndo(m, u, e.fileIdx, e.key, e.value + "x");
			done++;
		}
		rep.check("E6a three edits in two files: 3 modified, 2 dirty files", CountModified(m) == 3 && DirtyCount(m) == 2,
		          std::to_string(CountModified(m)) + " / " + std::to_string(DirtyCount(m)));
		int listed = 0;
		for (const EditorEntry& e : m.entries) if (e.edited) listed++;
		rep.check("E6b the filter's rows are exactly the modified ones", listed == 3);
		bool r = false;
		u.Undo(m, nullptr, &r);
		rep.check("E6c undoing one: 2 modified, 1 dirty file", CountModified(m) == 2 && DirtyCount(m) == 1);
	}

	// ---- E7 the panel ---------------------------------------------------------------
	{
		// a second language for poe1, so a language switch has somewhere to go
		const std::wstring xx = root + L"Data\\poe1\\xx-TEST\\";
		CreateDirectoryW(xx.c_str(), nullptr);
		writeAll(xx + L"meta.json", "{\"display_name\":\"Probe\",\"load_order\":[\"probe.json\"]}");
		writeAll(xx + L"probe.json", "{\"entries\":{\"Chaos Orb\":\"PROBE\"}}");

		ToolPanelHost host;
		host.exeDir = root;
		host.game = L"poe1";
		host.locale = L"zh-rTW";
		host.embedded = true;
		IToolPanel* p = CreateTranslationEditorPanel();
		const bool inited = p->Init(host);
		rep.check("E7a the panel loads the scratch copy", inited);
		if (inited) {
			auto op = [&](const char* o) { return TranslationEditorTestOp(p, o); };
			auto has = [](const std::string& s, const char* part) { return s.find(part) != std::string::npos; };
			std::string s = op("edit");
			s = op("edit");
			rep.check("E7b two edits: two undo steps, one modified entry", has(s, "undo=2 ") && has(s, "modified=1 "), s);
			s = op("onlymodified");
			rep.check("E7c 'only modified' shows exactly the modified rows", has(s, "shown=1 "), s);
			s = op("switchgame:1");
			rep.check("E7d switching with unsaved edits asks first (nothing switched yet)",
			          has(s, "pending=game") && has(s, "game=0 ") && has(s, "undo=2 "), s);
			s = op("answer:cancel");
			rep.check("E7e cancel keeps everything", has(s, "pending=none") && has(s, "game=0 ") && has(s, "undo=2 "), s);
			op("switchgame:1");
			s = op("answer:discard");
			rep.check("E7f don't save: switched to PoE2, undo stack empty, nothing modified",
			          has(s, "game=1 ") && has(s, "undo=0 ") && has(s, "modified=0 "), s);
			op("switchgame:0");
			s = op("edit");
			s = op("switchlocale:xx-TEST");
			rep.check("E7g a language switch asks too", has(s, "pending=locale") && has(s, "locale=zh-rTW"), s);
			s = op("answer:discard");
			rep.check("E7h ... and clears the stack", has(s, "locale=xx-TEST") && has(s, "undo=0 ") && has(s, "entries=1"), s);
			op("switchlocale:zh-rTW");
			op("edit");
			s = op("reload");
			rep.check("E7i reload with edits asks", has(s, "pending=reload") && has(s, "undo=1 "), s);
			s = op("answer:discard");
			rep.check("E7j ... and reloading clears the stack", has(s, "undo=0 ") && has(s, "modified=0 "), s);
			op("edit");
			s = op("undo");
			rep.check("E7k undo through the panel: back to unmodified", has(s, "undo=0 ") && has(s, "modified=0 "), s);
			op("edit");
			s = op("save");
			rep.check("E7l saving writes the scratch copy and clears the stack",
			          has(s, "undo=0 ") && has(s, "modified=0 ") && has(s, "savefail=0"), s);
			op("edit");
			op("switchgame:1");
			s = op("answer:save");
			rep.check("E7m 'save' in the prompt saves, then switches", has(s, "game=1 ") && has(s, "undo=0 ") &&
			                                                               has(s, "modified=0 ") && has(s, "savefail=0"), s);
			p->RequestClose();
			rep.check("E7n a clean panel closes at once", p->CloseState() == ToolCloseState::Closed);
		}
		delete p;
	}

	// ---- E8 the missing-strings scan ----------------------------------------------
	{
		writeAll(root + L"translate_misses.log",
		         "# untranslated strings (locale=zh-rTW)\n"
		         "MISS|Brand New Missing String\n"
		         "REV |\xe7\xa9\xa2\xe7\x94\x9f \xe6\xb8\xac\xe8\xa9\xa6\n"
		         "FLAVOUR |\xe9\xa2\xa8\xe5\x91\xb3\n"
		         "PROPERTY |\xe5\xb1\xac\xe6\x80\xa7: 1\n"
		         "MISS|Brand New Missing String\n"
		         "MISS|Chaos Orb\n");
		EditorModel m = LoadModel(root + L"Data\\poe1\\", "zh-rTW");
		bool found = false;
		int logged = 0;
		unsigned long long wt = 0;
		const std::vector<MissEntry> mm = ScanMisses(root, m, &found, &logged, &wt);
		int fwd = 0, rev = 0;
		for (const MissEntry& e : mm) (e.reverse ? rev : fwd)++;
		rep.check("E8a MISS is forward; REV / FLAVOUR / PROPERTY are paste-side", found && fwd == 1 && rev == 3,
		          std::to_string(fwd) + " forward, " + std::to_string(rev) + " paste");
		rep.check("E8b duplicates and keys already in the dictionary are dropped; the log's count is kept",
		          mm.size() == 4 && logged == 5 && wt != 0, std::to_string(mm.size()) + " rows, " + std::to_string(logged) + " logged");
		rep.check("E8c the tag is kept", mm.size() == 4 && mm[1].tag == "REV" && mm[2].tag == "FLAVOUR" && mm[3].tag == "PROPERTY");
	}

	// ---- clean up, and prove the real dictionaries were never touched -----------------
	removeTree(root);
	rep.check("E9a the scratch copy is gone", GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES);
	{
		int same = 0;
		for (const auto& kv : origHash) {
			std::string b;
			if (readAll(kv.first, b) && fnv(b) == kv.second) same++;
		}
		bool noLitter = true;
		for (const Src& s : srcs) {
			const std::wstring dir = exeDir + L"Data\\" + s.slot + L"\\" + s.locale + L"\\";
			if (!listFiles(dir, L"*.bak").empty() || !listFiles(dir, L"*.tmp").empty() || !listFiles(dir, L"*.rt").empty())
				noLitter = false;
		}
		rep.check("E9b the dictionaries next to the exe are byte-identical to before",
		          same == (int)origHash.size() && noLitter, std::to_string(same) + "/" + std::to_string(origHash.size()));
	}

	rep.line("");
	rep.line(std::to_string(rep.pass) + " passed, " + std::to_string(rep.fail) + " failed");
	rep.line(rep.fail ? "RESULT FAIL" : "RESULT PASS");
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	writeAll(exeDir + L"PobTools\\trans_editor_selftest.txt", rep.text);
	fputs(rep.text.c_str(), stdout);
	return rep.fail ? 1 : 0;
}
