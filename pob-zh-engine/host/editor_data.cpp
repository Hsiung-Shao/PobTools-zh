#include "editor_data.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <unordered_set>

using nlohmann::ordered_json;

// Structured-entry translation key "翻譯" (UTF-8 E7 BF BB E8 AD AF), spelled as
// bytes so it matches regardless of how this file's encoding is interpreted.
static const char* kTransKey = "\xe7\xbf\xbb\xe8\xad\xaf";

// ---- small Win32 / encoding helpers ---------------------------------------

static std::wstring widen(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

static std::string narrow(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

static bool dir_exists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Read a whole file into a UTF-8 string using a wide path (the exe may live in
// a non-ASCII directory). Returns false if the file cannot be opened.
static bool read_file_utf8(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 30)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

// Atomic write: write a temp file next to the target, then rename over it.
static bool write_file_atomic(const std::wstring& path, const std::string& data)
{
	std::wstring tmp = path + L".tmp";
	HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	bool ok = true;
	DWORD written = 0;
	if (!data.empty())
		ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
	CloseHandle(h);
	if (!ok) { DeleteFileW(tmp.c_str()); return false; }
	if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	return true;
}

// Strip SimpleGraphic colour escapes (^7 / ^xRRGGBB) — mirrors the engine, so a
// miss logged with colour codes still matches an uncoloured dictionary key.
static std::string strip_color_codes(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '^' && i + 1 < s.size()) {
			if ((s[i + 1] == 'x' || s[i + 1] == 'X') && i + 7 < s.size()) { i += 7; continue; }
			if (s[i + 1] >= '0' && s[i + 1] <= '9') { i += 1; continue; }
		}
		out += s[i];
	}
	return out;
}

// ---- model loading ---------------------------------------------------------

static std::wstring locale_dir(const std::wstring& exeDir, const std::string& game, const std::string& locale)
{
	return exeDir + L"Data\\" + widen(game) + L"\\" + widen(locale) + L"\\";
}

EditorModel LoadModel(const std::wstring& exeDir, const std::string& game, const std::string& locale)
{
	EditorModel model;
	model.dataDir = locale_dir(exeDir, game, locale);
	if (!dir_exists(model.dataDir)) {
		model.localeExists = false;
		return model;
	}
	model.localeExists = true;

	// Collect *.json file names first, sorted for a deterministic UI order.
	std::vector<std::wstring> names;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((model.dataDir + L"*.json").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				names.push_back(fd.cFileName);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	std::sort(names.begin(), names.end());

	for (const std::wstring& wname : names) {
		std::string content;
		if (!read_file_utf8(model.dataDir + wname, content)) continue;

		ordered_json doc;
		try {
			doc = ordered_json::parse(content);
		} catch (...) {
			continue; // skip unparseable file
		}
		// Only treat documents with an "entries" object as dictionary files
		// (this naturally excludes meta.json / synonyms.json / item_metadata.json).
		if (!doc.contains("entries") || !doc["entries"].is_object()) continue;

		EditorFile ef;
		ef.name = narrow(wname);
		ef.path = model.dataDir + wname;
		ef.doc = std::move(doc);
		model.files.push_back(std::move(ef));
		int fileIdx = (int)model.files.size() - 1;

		for (auto& [key, val] : model.files[fileIdx].doc["entries"].items()) {
			EditorEntry e;
			e.key = key;
			e.fileIdx = fileIdx;
			if (val.is_string()) {
				e.structured = false;
				e.value = val.get<std::string>();
			} else if (val.is_object()) {
				e.structured = true;
				if (val.contains(kTransKey) && val[kTransKey].is_string())
					e.value = val[kTransKey].get<std::string>();
				else
					e.value.clear();
			} else {
				continue; // unsupported value type
			}
			model.entries.push_back(std::move(e));
		}
	}
	return model;
}

int FindFileIdx(const EditorModel& model, const std::string& name)
{
	for (size_t i = 0; i < model.files.size(); i++)
		if (model.files[i].name == name) return (int)i;
	return -1;
}

size_t SetEntry(EditorModel& model, int fileIdx, const std::string& key, const std::string& value)
{
	for (size_t i = 0; i < model.entries.size(); i++) {
		if (model.entries[i].fileIdx == fileIdx && model.entries[i].key == key) {
			model.entries[i].value = value;
			model.files[fileIdx].dirty = true;
			return i;
		}
	}
	EditorEntry e;
	e.key = key;
	e.value = value;
	e.structured = false;
	e.fileIdx = fileIdx;
	model.entries.push_back(std::move(e));
	model.files[fileIdx].dirty = true;
	return model.entries.size() - 1;
}

bool SaveFile(EditorFile& file, std::string* err)
{
	if (!file.doc.contains("entries") || !file.doc["entries"].is_object()) {
		if (err) *err = file.name + ": no entries object";
		return false;
	}
	// Back up the on-disk file before overwriting (best-effort).
	CopyFileW(file.path.c_str(), (file.path + L".bak").c_str(), FALSE);

	std::string out;
	try {
		out = file.doc.dump(2, ' ', /*ensure_ascii=*/false);
	} catch (const std::exception& e) {
		if (err) *err = file.name + ": " + e.what();
		return false;
	}
	out.push_back('\n'); // trailing newline, matches typical JSON files
	if (!write_file_atomic(file.path, out)) {
		if (err) *err = file.name + ": write failed";
		return false;
	}
	file.dirty = false;
	return true;
}

// Push the current in-memory entry values back into a file's JSON document,
// preserving structured fields and key order, then return the doc unchanged.
static void sync_entries_into_doc(EditorModel& model, int fileIdx)
{
	auto& entries = model.files[fileIdx].doc["entries"];
	for (const EditorEntry& e : model.entries) {
		if (e.fileIdx != fileIdx) continue;
		if (e.structured && entries.contains(e.key) && entries[e.key].is_object()) {
			entries[e.key][kTransKey] = e.value;
		} else {
			entries[e.key] = e.value; // string value; inserts new keys at the end
		}
	}
}

int SaveAll(EditorModel& model, std::string* err)
{
	int saved = 0;
	for (size_t i = 0; i < model.files.size(); i++) {
		if (!model.files[i].dirty) continue;
		sync_entries_into_doc(model, (int)i);
		std::string fileErr;
		if (SaveFile(model.files[i], &fileErr)) {
			saved++;
		} else if (err && err->empty()) {
			*err = fileErr;
		}
	}
	return saved;
}

int DirtyCount(const EditorModel& model)
{
	int n = 0;
	for (const EditorFile& f : model.files) if (f.dirty) n++;
	return n;
}

std::vector<MissEntry> ScanMisses(const std::wstring& exeDir, const EditorModel& model, bool* logFound)
{
	std::vector<MissEntry> out;
	std::string content;
	if (!read_file_utf8(exeDir + L"translate_misses.log", content)) {
		if (logFound) *logFound = false;
		return out;
	}
	if (logFound) *logFound = true;

	// All dictionary keys (raw + colour-stripped) for fast "already present" tests.
	std::unordered_set<std::string> keys;
	keys.reserve(model.entries.size() * 2 + 16);
	for (const EditorEntry& e : model.entries) {
		keys.insert(e.key);
		std::string stripped = strip_color_codes(e.key);
		if (stripped != e.key) keys.insert(stripped);
	}

	std::unordered_set<std::string> seen;
	size_t pos = 0;
	while (pos < content.size()) {
		size_t nl = content.find('\n', pos);
		std::string line = content.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
		pos = (nl == std::string::npos) ? content.size() : nl + 1;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#') continue;

		size_t bar = line.find('|');
		if (bar == std::string::npos) continue;
		std::string tag = line.substr(0, bar);
		// trim tag
		while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t')) tag.pop_back();
		std::string text = line.substr(bar + 1);
		if (text.empty()) continue;

		bool present = keys.count(text) || keys.count(strip_color_codes(text));
		if (present) continue;
		if (!seen.insert(text).second) continue; // dedup

		MissEntry m;
		m.text = text;
		m.reverse = (tag == "REV");
		out.push_back(std::move(m));
	}
	return out;
}
