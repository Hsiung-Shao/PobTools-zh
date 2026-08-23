#include "regex_state.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp>   // nlohmann::ordered_json (deps/nlohmann)

using nlohmann::ordered_json;

namespace {

std::wstring StatePath(const std::wstring& exeDir)
{
	return exeDir + L"PobTools\\regex_ui.json";
}

bool ReadAll(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                       OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 24)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() ||
		     (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

std::vector<std::string> StringArray(const ordered_json& j, const char* key)
{
	std::vector<std::string> out;
	auto it = j.find(key);
	if (it == j.end() || !it->is_array()) return out;
	for (const auto& v : *it)
		if (v.is_string()) out.push_back(v.get<std::string>());
	return out;
}

// A mode or language read back from disk is only allowed to be one of the values
// this build knows. An unknown one is a file from a newer version (or a hand
// edit), and silently carrying it into the UI would put the panel into a state
// no control can express.
std::string OneOf(const std::string& v, const char* a, const char* b, const char* c)
{
	if (v == a || v == b || (c && v == c)) return v;
	return a;
}

} // namespace

RegexPagePicks& RegexUiState::PicksFor(const std::string& pageId)
{
	for (RegexPagePicks& p : current)
		if (p.page == pageId) return p;
	current.push_back(RegexPagePicks{pageId, {}, {}});
	return current.back();
}

bool RegexUiState::Load(const std::wstring& exeDir)
{
	std::string body;
	if (!ReadAll(StatePath(exeDir), body)) return false;
	try {
		ordered_json doc = ordered_json::parse(body);
		page = doc.value("page", std::string());
		mode = OneOf(doc.value("mode", std::string("any")), "any", "all", "none");
		lang = OneOf(doc.value("lang", std::string("zh")), "zh", "en", nullptr);
		bilingual = doc.value("bilingual", true);

		current.clear();
		auto cur = doc.find("current");
		if (cur != doc.end() && cur->is_array()) {
			for (const auto& p : *cur) {
				if (!p.is_object()) continue;
				RegexPagePicks rec;
				rec.page = p.value("page", std::string());
				if (rec.page.empty()) continue;
				rec.keys = StringArray(p, "keys");
				rec.alt = StringArray(p, "alt");
				current.push_back(std::move(rec));
			}
		}

		bookmarks.clear();
		auto bm = doc.find("bookmarks");
		if (bm != doc.end() && bm->is_array()) {
			for (const auto& b : *bm) {
				if (!b.is_object()) continue;
				RegexBookmark rec;
				rec.name = b.value("name", std::string());
				rec.page = b.value("page", std::string());
				rec.mode = OneOf(b.value("mode", std::string("any")), "any", "all", "none");
				rec.lang = OneOf(b.value("lang", std::string("zh")), "zh", "en", nullptr);
				rec.keys = StringArray(b, "keys");
				rec.alt = StringArray(b, "alt");
				// A nameless or empty bookmark is not something the UI can offer,
				// and keeping it would put a blank row in the list forever.
				if (rec.name.empty() || rec.page.empty() || rec.keys.empty()) continue;
				bookmarks.push_back(std::move(rec));
			}
		}
	} catch (const std::exception&) {
		// A corrupt file must not take the tool down with it. The defaults are a
		// perfectly usable starting state, and the next Save rewrites the file.
		current.clear();
		bookmarks.clear();
		return false;
	}
	return true;
}

bool RegexUiState::Save(const std::wstring& exeDir) const
{
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);   // ok if it exists

	ordered_json doc;
	doc["schema"] = 1;
	doc["page"] = page;
	doc["mode"] = mode;
	doc["lang"] = lang;
	doc["bilingual"] = bilingual;
	ordered_json cur = ordered_json::array();
	for (const RegexPagePicks& p : current) {
		if (p.keys.empty()) continue;   // nothing ticked: nothing worth writing
		ordered_json o;
		o["page"] = p.page;
		o["keys"] = p.keys;
		o["alt"] = p.alt;
		cur.push_back(std::move(o));
	}
	doc["current"] = std::move(cur);
	ordered_json bm = ordered_json::array();
	for (const RegexBookmark& b : bookmarks) {
		ordered_json o;
		o["name"] = b.name;
		o["page"] = b.page;
		o["mode"] = b.mode;
		o["lang"] = b.lang;
		o["keys"] = b.keys;
		o["alt"] = b.alt;
		bm.push_back(std::move(o));
	}
	doc["bookmarks"] = std::move(bm);
	const std::string out = doc.dump(1, '\t');

	// Written beside the real file and moved into place. Bookmarks are the only
	// thing in this tool the player cannot get back from anywhere else, and a
	// write interrupted halfway would leave a truncated JSON that the loader
	// above would throw away wholesale.
	const std::wstring dst = StatePath(exeDir);
	const std::wstring tmp = dst + L".tmp";
	HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
	                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	DWORD wrote = 0;
	const bool ok = WriteFile(f, out.data(), (DWORD)out.size(), &wrote, nullptr) &&
	                wrote == out.size();
	CloseHandle(f);
	if (!ok) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	if (!MoveFileExW(tmp.c_str(), dst.c_str(),
	                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	return true;
}

int RegexResolveKeys(const std::vector<std::string>& keys,
                     const std::vector<std::string>& alt,
                     const std::vector<std::string>& entryKeys,
                     const std::vector<std::string>& entryAlt,
                     std::vector<char>& picked)
{
	picked.assign(entryKeys.size(), 0);
	int missed = 0;
	for (size_t k = 0; k < keys.size(); k++) {
		int hit = -1;
		// English first: it is the language the data is keyed by, so it survives a
		// revision of our own Chinese wording.
		if (!keys[k].empty())
			for (size_t i = 0; i < entryKeys.size() && hit < 0; i++)
				if (entryKeys[i] == keys[k]) hit = (int)i;
		// Chinese second, for the rarer case where GGG reworded the English and
		// left the translation alone.
		if (hit < 0 && k < alt.size() && !alt[k].empty())
			for (size_t i = 0; i < entryAlt.size() && hit < 0; i++)
				if (entryAlt[i] == alt[k]) hit = (int)i;
		if (hit >= 0) picked[hit] = 1;
		else missed++;
	}
	return missed;
}
