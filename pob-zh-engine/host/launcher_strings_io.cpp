#include "launcher_strings_io.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

using nlohmann::ordered_json;

namespace {

std::wstring widen(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

bool read_file_utf8(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 26)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

bool write_file_bytes(const std::wstring& path, const std::string& data)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	bool ok = data.empty() ||
	          (WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size());
	CloseHandle(h);
	return ok;
}

// The shipped dictionaries are CRLF; keep the exported ones in the same style so
// a first save from the translation editor is not an all-lines diff.
std::string to_crlf(const std::string& s)
{
	std::string out;
	out.reserve(s.size() + s.size() / 16);
	for (char c : s) {
		if (c == '\n') out.push_back('\r');
		out.push_back(c);
	}
	return out;
}

} // namespace

LauncherStringStore LoadLauncherStrings(const std::wstring& slotRoot, const std::wstring& locale)
{
	LauncherStringStore st;
	st.s = StringsFor(locale); // compiled base; every field already valid from here on

	std::string content;
	if (!read_file_utf8(slotRoot + locale + L"\\launcher.json", content)) return st;
	st.fileFound = true;

	ordered_json doc;
	try {
		doc = ordered_json::parse(content);
	} catch (...) {
		return st; // corrupt file: compiled defaults, nothing lost
	}
	if (!doc.contains("entries") || !doc["entries"].is_object()) return st;
	const ordered_json& entries = doc["entries"];

	for (size_t i = 0; i < kLauncherStringsFields; i++) {
		auto mem = kLauncherStringMembers[i];
		const char* key = STR_EN.*mem; // English string is the key, same as the POB dictionaries
		if (!key) continue;
		auto it = entries.find(key);
		if (it == entries.end() || !it->is_string()) continue;
		std::string v = it->get<std::string>();
		// An empty value would blank a button with no way to tell it apart from a
		// missing one; treat it as "not translated".
		if (v.empty()) continue;
		const char* base = st.s.*mem;
		if (base && v == base) continue; // same as shipped: nothing to own
		st.owned.push_back(std::move(v));
		st.s.*mem = st.owned.back().c_str();
		st.overridden++;
	}
	return st;
}

int RunLauncherStringsExport(const std::wstring& exeDir)
{
	// Only zh-rTW has anything to translate: the keys ARE the English strings, so
	// an "en" file would map every entry to itself.
	const std::wstring locale = L"zh-rTW";
	const std::wstring dir = exeDir + L"Data\\launcher\\" + locale + L"\\";

	// Every level: CreateDirectoryW does not create parents, and the selftest
	// exports into a fresh sandbox where none of them exist yet.
	CreateDirectoryW((exeDir + L"Data").c_str(), nullptr);
	CreateDirectoryW((exeDir + L"Data\\launcher").c_str(), nullptr);
	CreateDirectoryW(dir.c_str(), nullptr);

	ordered_json entries = ordered_json::object();
	for (size_t i = 0; i < kLauncherStringsFields; i++) {
		auto mem = kLauncherStringMembers[i];
		const char* key = STR_EN.*mem;
		const char* val = STR_ZHTW.*mem;
		if (!key || !val) continue;
		entries[key] = val;
	}
	ordered_json dict = ordered_json::object();
	dict["entries"] = entries;

	ordered_json meta = ordered_json::object();
	meta["version"] = "1.0.0";
	meta["locale"] = "zh-rTW";
	// What the launcher's language picker shows. Optional: a folder without it
	// falls back to its own name, which is how a translator can add a language
	// with nothing but a folder and a load_order.
	meta["display_name"] = u8"繁體中文";
	meta["source"] = "launcher";
	meta["load_order"] = ordered_json::array({ "launcher.json" });

	bool ok = write_file_bytes(dir + L"launcher.json", to_crlf(dict.dump(2) + "\n")) &&
	          write_file_bytes(dir + L"meta.json", to_crlf(meta.dump(2) + "\n"));
	// Deliberately NO MessageBox on failure: the selftest calls this function, and
	// a modal dialog turns a headless check into a hang with no output. Exit code
	// and stdout only.
	printf(ok ? "launcher strings exported to Data\\launcher\\zh-rTW\\ (%d entries)\n"
	          : "launcher strings export FAILED (%d entries prepared)\n",
	       (int)entries.size());
	return ok ? 0 : 1;
}

// ---- selftest ---------------------------------------------------------------

namespace {

void rm_tree(const std::wstring& dir)
{
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			std::wstring name = fd.cFileName;
			if (name == L"." || name == L"..") continue;
			std::wstring p = dir + L"\\" + name;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(p);
			else DeleteFileW(p.c_str());
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

// Write <root>launcher\zh-rTW\launcher.json with the given raw body.
bool put_overlay(const std::wstring& slotRoot, const std::string& body)
{
	CreateDirectoryW(slotRoot.c_str(), nullptr);
	CreateDirectoryW((slotRoot + L"zh-rTW").c_str(), nullptr);
	return write_file_bytes(slotRoot + L"zh-rTW\\launcher.json", body);
}

} // namespace

int RunLauncherStringsSelfTest(const std::wstring& exeDir)
{
	std::string report;
	int failures = 0;
	auto check = [&](const char* name, bool ok, const std::string& detail = "") {
		report += std::string(ok ? "PASS " : "FAIL ") + name +
		          (detail.empty() ? "" : "  (" + detail + ")") + "\n";
		if (!ok) failures++;
	};

	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	std::wstring sandbox = std::wstring(tmp) + L"pobtools_lstr_selftest";
	rm_tree(sandbox);
	CreateDirectoryW(sandbox.c_str(), nullptr);
	// The slot root: the folder that directly holds the <locale> sub-folders.
	const std::wstring root = sandbox + L"\\launcher\\";

	// L1 -- the JSON keys off the English string, so two fields sharing one English
	// string would be silently forced to share a translation. Catch it here rather
	// than from a screenshot.
	{
		std::unordered_set<std::string> seen;
		std::string dup;
		for (size_t i = 0; i < kLauncherStringsFields; i++) {
			const char* en = STR_EN.*kLauncherStringMembers[i];
			if (!en) continue;
			if (!seen.insert(en).second) dup = std::string(kLauncherStringNames[i]) + " = \"" + en + "\"";
		}
		check("L1 English strings are pairwise unique (they are the JSON keys)", dup.empty(), dup);
	}

	// L2 -- no file at all: every field is the compiled pointer, not a copy.
	{
		LauncherStringStore st = LoadLauncherStrings(root, L"zh-rTW");
		bool same = !st.fileFound && st.overridden == 0;
		for (size_t i = 0; i < kLauncherStringsFields && same; i++)
			same = st.s.*kLauncherStringMembers[i] == STR_ZHTW.*kLauncherStringMembers[i];
		check("L2 missing file falls back to every compiled string", same);
	}

	// L3 -- partial overlay: the named field changes, the others stay compiled.
	{
		ordered_json e = ordered_json::object();
		e[STR_EN.tabHome] = "首頁測試";
		ordered_json d = ordered_json::object();
		d["entries"] = e;
		put_overlay(root, d.dump(2));
		LauncherStringStore st = LoadLauncherStrings(root, L"zh-rTW");
		bool ok = st.fileFound && st.overridden == 1 &&
		          std::string(st.s.tabHome) == "首頁測試" &&
		          st.s.tabSettings == STR_ZHTW.tabSettings &&
		          st.s.title == STR_ZHTW.title;
		check("L3 partial overlay leaves untouched fields on the compiled string", ok,
		      "overridden=" + std::to_string(st.overridden));
	}

	// L4 -- keys the binary does not know are ignored, not appended anywhere.
	{
		ordered_json e = ordered_json::object();
		e["A string no build ever had"] = "x";
		e[""] = "y";
		ordered_json d = ordered_json::object();
		d["entries"] = e;
		put_overlay(root, d.dump(2));
		LauncherStringStore st = LoadLauncherStrings(root, L"zh-rTW");
		check("L4 unknown keys are ignored", st.overridden == 0);
	}

	// L5 -- empty and identical values must not count as translations: an empty
	// one would blank a button, an identical one would allocate for nothing.
	{
		ordered_json e = ordered_json::object();
		e[STR_EN.close] = "";
		e[STR_EN.font] = STR_ZHTW.font;
		ordered_json d = ordered_json::object();
		d["entries"] = e;
		put_overlay(root, d.dump(2));
		LauncherStringStore st = LoadLauncherStrings(root, L"zh-rTW");
		check("L5 empty / unchanged values are not overrides",
		      st.overridden == 0 && st.s.close == STR_ZHTW.close && st.s.font == STR_ZHTW.font);
	}

	// L6 -- POINTER LIFETIME. Override every field, then read them all back. The
	// store hands out raw pointers into its own container; if that container ever
	// moved its elements (a std::vector would, on growth) the earlier fields would
	// now point at freed memory and this comparison reads garbage.
	{
		ordered_json e = ordered_json::object();
		std::vector<std::string> expect(kLauncherStringsFields);
		for (size_t i = 0; i < kLauncherStringsFields; i++) {
			expect[i] = "ZZ" + std::to_string(i) + "_" + kLauncherStringNames[i];
			e[STR_EN.*kLauncherStringMembers[i]] = expect[i];
		}
		ordered_json d = ordered_json::object();
		d["entries"] = e;
		put_overlay(root, d.dump(2));
		LauncherStringStore st = LoadLauncherStrings(root, L"zh-rTW");
		std::string bad;
		for (size_t i = 0; i < kLauncherStringsFields; i++) {
			const char* got = st.s.*kLauncherStringMembers[i];
			if (!got || expect[i] != got) { bad = kLauncherStringNames[i]; break; }
		}
		check("L6 all fields overridden stay readable (no container reallocation)",
		      bad.empty() && st.overridden == (int)kLauncherStringsFields,
		      bad.empty() ? ("overridden=" + std::to_string(st.overridden)) : ("first bad: " + bad));
	}

	// L7 -- a corrupt file must degrade to the compiled table, not crash or blank.
	{
		put_overlay(root, "{ this is not json");
		LauncherStringStore st = LoadLauncherStrings(root, L"zh-rTW");
		check("L7 corrupt JSON falls back to compiled strings",
		      st.fileFound && st.overridden == 0 && st.s.title == STR_ZHTW.title);
	}

	// L8 -- export round-trip: what --launcher-strings-export writes must read back
	// as exactly the compiled table, which is what stops the shipped JSON from
	// drifting away from the binary.
	{
		std::wstring exportRoot = sandbox + L"\\export\\";
		CreateDirectoryW((sandbox + L"\\export").c_str(), nullptr);
		int rc = RunLauncherStringsExport(sandbox + L"\\export\\");
		LauncherStringStore st = LoadLauncherStrings(exportRoot + L"Data\\launcher\\", L"zh-rTW");
		bool same = rc == 0 && st.fileFound;
		std::string bad;
		for (size_t i = 0; i < kLauncherStringsFields && same; i++) {
			const char* got = st.s.*kLauncherStringMembers[i];
			const char* want = STR_ZHTW.*kLauncherStringMembers[i];
			if (!got || !want || std::string(want) != got) { same = false; bad = kLauncherStringNames[i]; }
		}
		// Everything matched the compiled table, so nothing needed owning.
		check("L8 exported JSON reads back as the compiled table", same && st.overridden == 0,
		      bad.empty() ? "" : ("first mismatch: " + bad));

		std::string meta;
		bool metaOk = read_file_utf8(exportRoot + L"Data\\launcher\\zh-rTW\\meta.json", meta) &&
		              meta.find("\"load_order\"") != std::string::npos &&
		              meta.find("launcher.json") != std::string::npos &&
		              meta.find("\r\n") != std::string::npos;
		check("L9 exported meta.json carries load_order and is CRLF like the others", metaOk);
	}

	rm_tree(sandbox);

	report += failures ? "RESULT FAIL\n" : "RESULT PASS\n";
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	write_file_bytes(exeDir + L"PobTools\\launcher_strings_selftest.txt", report);
	return failures ? 2 : 0;
}
