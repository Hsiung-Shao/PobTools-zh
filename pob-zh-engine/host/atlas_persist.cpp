#include "atlas_persist.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#include <cstdlib> // free
#include <cstring> // strlen

#include "../engine/common/base64.h" // curl-derived, extern "C", malloc'd outputs

using nlohmann::ordered_json;

// ---- file helpers (same conventions as atlas_tree_data.cpp) -------------------

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

static bool write_file_utf8(const std::wstring& path, const std::string& content)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	bool ok = content.empty() ||
		(WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr) && written == content.size());
	CloseHandle(h);
	return ok;
}

static std::vector<int> parse_alloc_array(const ordered_json& arr)
{
	std::vector<int> out;
	for (const auto& jid : arr)
		if (jid.is_number_integer()) out.push_back(jid.get<int>());
	return out;
}

// ---- AtlasBuildFile -----------------------------------------------------------

std::wstring AtlasBuildFile::PathOf(const std::wstring& exeDir)
{
	return exeDir + L"PobTools\\atlas_build_poe1.json";
}

bool AtlasBuildFile::ParseDoc(const std::string& json)
{
	try {
		ordered_json doc = ordered_json::parse(json);
		if (doc.contains("builds") && doc["builds"].is_array()) {
			// current multi-build schema
			std::vector<AtlasBuildEntry> parsed;
			for (const auto& b : doc["builds"]) {
				if (!b.is_object() || !b.contains("alloc") || !b["alloc"].is_array()) return false;
				AtlasBuildEntry e;
				e.name = b.value("name", std::string());
				if (e.name.empty()) e.name = u8"預設";
				e.alloc = parse_alloc_array(b["alloc"]);
				parsed.push_back(std::move(e));
			}
			if (parsed.empty()) return false;
			builds = std::move(parsed);
			version = doc.value("version", std::string());
			active = doc.value("active", 0);
			Active(); // clamp
			return true;
		}
		if (doc.contains("alloc") && doc["alloc"].is_array()) {
			// legacy single-build file: wrap it as the first project
			AtlasBuildEntry e;
			e.name = u8"預設";
			e.alloc = parse_alloc_array(doc["alloc"]);
			builds.assign(1, std::move(e));
			version = doc.value("version", std::string());
			active = 0;
			return true;
		}
		return false;
	} catch (...) {
		return false;
	}
}

std::string AtlasBuildFile::SerializeDoc() const
{
	ordered_json doc;
	doc["format"] = "pobtools-atlas-builds";
	doc["version"] = version;
	doc["active"] = active;
	ordered_json arr = ordered_json::array();
	for (const AtlasBuildEntry& b : builds) {
		ordered_json e;
		e["name"] = b.name;
		e["alloc"] = b.alloc;
		arr.push_back(std::move(e));
	}
	doc["builds"] = std::move(arr);
	return doc.dump();
}

bool AtlasBuildFile::Load(const std::wstring& exeDir)
{
	std::string content;
	if (read_file_utf8(PathOf(exeDir), content) && ParseDoc(content)) return true;
	builds.assign(1, AtlasBuildEntry{ u8"預設", {} });
	version.clear();
	active = 0;
	return false;
}

bool AtlasBuildFile::Save(const std::wstring& exeDir) const
{
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr); // ok if it exists
	return write_file_utf8(PathOf(exeDir), SerializeDoc());
}

AtlasBuildEntry& AtlasBuildFile::Active()
{
	if (builds.empty()) builds.assign(1, AtlasBuildEntry{ u8"預設", {} });
	if (active < 0) active = 0;
	if (active >= (int)builds.size()) active = (int)builds.size() - 1;
	return builds[active];
}

int AtlasBuildFile::AddBuild(const std::string& name)
{
	builds.push_back(AtlasBuildEntry{ UniqueName(name.empty() ? u8"新專案" : name), {} });
	return (int)builds.size() - 1;
}

int AtlasBuildFile::DuplicateBuild(int idx)
{
	if (idx < 0 || idx >= (int)builds.size()) return -1;
	AtlasBuildEntry copy = builds[idx];
	copy.name = UniqueName(copy.name + u8"（複製）");
	builds.push_back(std::move(copy));
	return (int)builds.size() - 1;
}

bool AtlasBuildFile::RemoveBuild(int idx)
{
	if (idx < 0 || idx >= (int)builds.size() || builds.size() <= 1) return false;
	builds.erase(builds.begin() + idx);
	if (active >= (int)builds.size()) active = (int)builds.size() - 1;
	else if (active > idx) active--;
	return true;
}

std::string AtlasBuildFile::UniqueName(const std::string& want) const
{
	auto taken = [&](const std::string& n) {
		for (const AtlasBuildEntry& b : builds)
			if (b.name == n) return true;
		return false;
	};
	if (!taken(want)) return want;
	for (int i = 2; i < 1000; i++) {
		std::string cand = want + " (" + std::to_string(i) + ")";
		if (!taken(cand)) return cand;
	}
	return want;
}

// ---- export / share code ------------------------------------------------------

std::string AtlasExportJson(const AtlasBuildEntry& b, const std::string& treeVersion)
{
	ordered_json doc;
	doc["format"] = "pobtools-atlas-build";
	doc["version"] = treeVersion;
	doc["name"] = b.name;
	doc["alloc"] = b.alloc;
	return doc.dump();
}

bool AtlasParseExportJson(const std::string& json, AtlasBuildEntry* out, std::string* err)
{
	auto fail = [&](const char* m) {
		if (err) *err = m;
		return false;
	};
	try {
		ordered_json doc = ordered_json::parse(json);
		if (doc.value("format", std::string()) != "pobtools-atlas-build") {
			if (doc.value("format", std::string()) == "pobtools-atlas-builds")
				return fail(u8"這是完整存檔而不是單一專案的匯出檔");
			return fail(u8"不是 PobTools 圖譜專案匯出檔（format 欄位不符）");
		}
		if (!doc.contains("alloc") || !doc["alloc"].is_array())
			return fail(u8"匯出檔缺少 alloc 陣列");
		out->name = doc.value("name", std::string());
		if (out->name.empty()) out->name = u8"匯入的專案";
		out->alloc = parse_alloc_array(doc["alloc"]);
		return true;
	} catch (const std::exception& e) {
		if (err) *err = std::string(u8"JSON 解析失敗: ") + e.what();
		return false;
	}
}

static const char* kShareCodePrefix = "PTAT1|";

std::string AtlasBuildShareCode(const AtlasBuildEntry& b, const std::string& treeVersion)
{
	std::string json = AtlasExportJson(b, treeVersion);
	char* enc = nullptr;
	size_t encLen = 0;
	if (!Base64Encode(json.c_str(), json.size(), &enc, &encLen)) return std::string();
	std::string code = kShareCodePrefix + std::string(enc, encLen);
	free(enc);
	return code;
}

bool AtlasParseShareCode(const std::string& code, AtlasBuildEntry* out, std::string* err)
{
	auto fail = [&](const char* m) {
		if (err) *err = m;
		return false;
	};
	// clipboard content commonly carries stray whitespace / line breaks
	std::string s;
	s.reserve(code.size());
	for (char c : code)
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n') s.push_back(c);
	if (s.compare(0, strlen(kShareCodePrefix), kShareCodePrefix) != 0)
		return fail(u8"不是 PobTools 圖譜分享碼（缺少 PTAT1 前綴）");
	unsigned char* dec = nullptr;
	size_t decLen = 0;
	if (!Base64Decode(s.c_str() + strlen(kShareCodePrefix), &dec, &decLen) || !dec)
		return fail(u8"分享碼的 base64 內容無法解碼");
	std::string json((const char*)dec, decLen);
	free(dec);
	return AtlasParseExportJson(json, out, err);
}

// ---- AtlasUiState -------------------------------------------------------------

static std::wstring ui_state_path(const std::wstring& exeDir)
{
	return exeDir + L"PobTools\\atlas_ui.json";
}

bool AtlasUiState::Load(const std::wstring& exeDir)
{
	std::string content;
	if (!read_file_utf8(ui_state_path(exeDir), content)) return false;
	try {
		ordered_json doc = ordered_json::parse(content);
		panelW = doc.value("panelW", 0.0f);
		if (!(panelW > 0.0f && panelW < 4096.0f)) panelW = 0.0f;
		season = doc.value("season", std::string());
		return true;
	} catch (...) {
		panelW = 0.0f;
		season.clear();
		return false;
	}
}

bool AtlasUiState::Save(const std::wstring& exeDir) const
{
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	ordered_json doc;
	doc["panelW"] = panelW;
	if (!season.empty()) doc["season"] = season;
	return write_file_utf8(ui_state_path(exeDir), doc.dump());
}
