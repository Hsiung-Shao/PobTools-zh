#include "regex_data.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp>   // nlohmann::ordered_json (deps/nlohmann)

using nlohmann::ordered_json;

namespace {

// The game id is "poe1" / "poe2" and nothing else, so narrowing it for a message
// is a cast, not a conversion. Spelled out because the implicit form warns, and a
// silenced warning here would also silence the day someone passes real text.
std::string NarrowAscii(const std::wstring& w)
{
	std::string out;
	out.reserve(w.size());
	for (wchar_t c : w) out += (c > 0 && c < 128) ? (char)c : '?';
	return out;
}

bool ReadFileUtf8(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                       OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 26)) {
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

} // namespace

bool RegexDataset::Load(const std::wstring& exeDir, const std::wstring& preferred,
                        std::string* err)
{
	pages_.clear();
	source_.clear();

	// Preferred first so the combo opens on the game the launcher is set to, but
	// never ONLY the preferred one: a catalogue that exists is worth offering, and
	// hiding PoE1's map modifiers because the launcher is pointed at PoE2 would be
	// a worse answer than a labelled page.
	std::vector<std::wstring> games;
	if (preferred == L"poe1" || preferred == L"poe2") games.push_back(preferred);
	for (const wchar_t* g : {L"poe1", L"poe2"}) {
		if (games.empty() || games[0] != g) games.push_back(g);
	}
	bool any = false;
	for (const std::wstring& g : games)
		any |= LoadOne(exeDir, g, err);
	if (!any) {
		if (err) *err = u8"安裝目錄的 Data 底下沒有任何 regex_*.json 清單檔";
		return false;
	}
	if (err) err->clear();
	return true;
}

bool RegexDataset::LoadOne(const std::wstring& exeDir, const std::wstring& game,
                           std::string* err)
{
	const std::wstring path = exeDir + L"Data\\regex_" + game + L".json";
	std::string body;
	if (!ReadFileUtf8(path, body)) return false;
	const std::string gameId = NarrowAscii(game);
	const size_t before = pages_.size();
	try {
		ordered_json doc = ordered_json::parse(body);
		if (source_.empty()) source_ = doc.value("source", std::string());
		const auto pages = doc.find("pages");
		if (pages == doc.end() || !pages->is_array()) {
			if (err) *err = u8"資料檔缺少 pages 陣列";
			return false;
		}
		for (const auto& p : *pages) {
			if (!p.is_object()) continue;
			RegexPageDef page;
			page.game = gameId;
			page.id = p.value("id", std::string());
			page.title = p.value("title", std::string());
			page.note = p.value("note", std::string());
			page.limit = p.value("limit", 250);
			page.groups = StringArray(p, "groups");
			const auto entries = p.find("entries");
			if (entries == p.end() || !entries->is_array()) continue;
			page.entries.reserve(entries->size());
			for (const auto& e : *entries) {
				if (!e.is_object()) continue;
				RegexEntryDef d;
				d.id = e.value("id", std::string());
				d.group = e.value("g", 0);
				d.t17 = e.value("t17", false);
				d.affixZh = e.value("affixZh", std::string());
				d.zh = StringArray(e, "zh");
				d.en = StringArray(e, "en");
				// An entry with no printed text has nothing to search for, and
				// keeping it would put a row in the list that can never be
				// resolved into a token.
				if (d.zh.empty() && d.en.empty()) continue;
				if (d.group < 0 || d.group >= (int)page.groups.size()) d.group = 0;
				page.entries.push_back(std::move(d));
			}
			if (!page.entries.empty()) pages_.push_back(std::move(page));
		}
	} catch (const std::exception& ex) {
		// One bad file must not take the other game's catalogue down with it, so
		// only this file's pages are rolled back.
		if (err) *err = u8"regex_" + gameId + u8".json 解析失敗：" + ex.what();
		while (pages_.size() > before) pages_.pop_back();
		return false;
	}
	return pages_.size() > before;
}

bool RegexDataset::HasGame(const std::string& game) const
{
	for (const RegexPageDef& p : pages_)
		if (p.game == game) return true;
	return false;
}
