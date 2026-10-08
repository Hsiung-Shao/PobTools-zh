#include "regex_data.h"
#include "error_log.h"

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

// j[key][sub] as a string array; missing at either level reads as empty, the
// same way StringArray treats a missing key.
std::vector<std::string> NestedStringArray(const ordered_json& j, const char* key,
                                           const char* sub)
{
	auto it = j.find(key);
	if (it == j.end() || !it->is_object()) return {};
	return StringArray(*it, sub);
}

// data.ts:196 parseLabels, one language: non-string values are skipped, text
// goes through RegexNormalizeLabel.
std::vector<std::pair<std::string, std::string>> LabelMap(const ordered_json& labels,
                                                          const char* lang)
{
	std::vector<std::pair<std::string, std::string>> out;
	auto it = labels.find(lang);
	if (it == labels.end() || !it->is_object()) return out;
	for (auto kv = it->begin(); kv != it->end(); ++kv) {
		if (!kv.value().is_string()) continue;
		out.emplace_back(kv.key(), RegexNormalizeLabel(kv.value().get<std::string>()));
	}
	return out;
}

} // namespace

std::string RegexNormalizeLabel(const std::string& s)
{
	// .replace(/\[([^\]|]*)\|([^\]]*)\]/g, '$2')
	// .replace(/\[([^\]|]*)\]/g, '$1')
	// .replace(/\{\d+\}/g, '#')
	// Each pattern is deterministic (the classes exclude the delimiter that
	// must follow), so a left-to-right scan that skips one character on a
	// failed start is exactly the global replace. All delimiters are ASCII, so
	// scanning bytes never splits a UTF-8 character.
	std::string a;
	for (size_t i = 0; i < s.size();) {
		if (s[i] == '[') {
			size_t j = i + 1;
			while (j < s.size() && s[j] != ']' && s[j] != '|') j++;
			if (j < s.size() && s[j] == '|') {
				size_t k = j + 1;
				while (k < s.size() && s[k] != ']') k++;
				if (k < s.size()) {
					a.append(s, j + 1, k - j - 1);
					i = k + 1;
					continue;
				}
			}
		}
		a += s[i++];
	}
	std::string b;
	for (size_t i = 0; i < a.size();) {
		if (a[i] == '[') {
			size_t j = i + 1;
			while (j < a.size() && a[j] != ']' && a[j] != '|') j++;
			if (j < a.size() && a[j] == ']') {
				b.append(a, i + 1, j - i - 1);
				i = j + 1;
				continue;
			}
		}
		b += a[i++];
	}
	std::string c;
	for (size_t i = 0; i < b.size();) {
		if (b[i] == '{') {
			size_t j = i + 1;
			while (j < b.size() && b[j] >= '0' && b[j] <= '9') j++;
			if (j > i + 1 && j < b.size() && b[j] == '}') {
				c += '#';
				i = j + 1;
				continue;
			}
		}
		c += b[i++];
	}
	return c;
}

RegexPageKind RegexPageKindFrom(const std::string& s)
{
	if (s == "names") return RegexPageKind::Names;
	if (s == "numeric") return RegexPageKind::Numeric;
	if (s == "sockets") return RegexPageKind::Sockets;
	return RegexPageKind::Mods;
}

const std::string* RegexLabels::Find(bool zhSide, const std::string& key) const
{
	for (const auto& kv : zhSide ? zh : en)
		if (kv.first == key) return &kv.second;
	return nullptr;
}

RegexLabels RegexMergeLabels(const RegexLabels& primary, const RegexLabels& fallback)
{
	// {...fallback, ...primary}: fallback's keys in their order (a primary value
	// replacing the text in place), then primary's other keys in theirs.
	RegexLabels out;
	out.present = true;
	for (int side = 0; side < 2; side++) {
		const auto& f = side == 0 ? fallback.zh : fallback.en;
		const auto& p = side == 0 ? primary.zh : primary.en;
		auto& o = side == 0 ? out.zh : out.en;
		o = f;
		for (const auto& kv : p) {
			bool done = false;
			for (auto& x : o)
				if (x.first == kv.first) { x.second = kv.second; done = true; break; }
			if (!done) o.push_back(kv);
		}
	}
	return out;
}

const RegexLabels* RegexDataset::Labels(const std::string& game) const
{
	for (const auto& g : labels_)
		if (g.first == game) return &g.second;
	return nullptr;
}

bool RegexDataset::Load(const std::wstring& exeDir, const std::wstring& preferred,
                        std::string* err)
{
	pages_.clear();
	labels_.clear();
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
		PobLog::Error("data", "no regex_*.json found under Data\\ (Poe Regex has nothing to show)");
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
			page.kind = RegexPageKindFrom(p.value("kind", std::string("mods")));
			page.title = p.value("title", std::string());
			page.titleEn = p.value("titleEn", std::string());
			page.note = p.value("note", std::string());
			page.limit = p.value("limit", 250);
			page.groups = StringArray(p, "groups");
			page.groupsEn = StringArray(p, "groupsEn");
			page.ambientZh = StringArray(p, "ambientZh");
			page.ambientEn = StringArray(p, "ambientEn");
			page.namePrefixZh = NestedStringArray(p, "nameWordsZh", "prefix");
			page.nameSuffixZh = NestedStringArray(p, "nameWordsZh", "suffix");
			page.namePrefixEn = NestedStringArray(p, "nameWordsEn", "prefix");
			page.nameSuffixEn = NestedStringArray(p, "nameWordsEn", "suffix");
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
				d.hiddenZh = StringArray(e, "hiddenZh");
				d.hiddenEn = StringArray(e, "hiddenEn");
				// An entry with no printed text has nothing to search for, and
				// keeping it would put a row in the list that can never be
				// resolved into a token.
				if (d.zh.empty() && d.en.empty()) continue;
				if (d.group < 0 || d.group >= (int)page.groups.size()) d.group = 0;
				page.entries.push_back(std::move(d));
			}
			if (!page.entries.empty()) pages_.push_back(std::move(page));
		}
		RegexLabels labels;
		const auto lab = doc.find("labels");
		if (lab != doc.end() && lab->is_object()) {
			labels.present = true;
			labels.zh = LabelMap(*lab, "zh");
			labels.en = LabelMap(*lab, "en");
		}
		// node.ts:38 loadLabelsFor: the data file's labels win key by key, the
		// stand-in Data\regex_labels_<game>.json (exile-appraiser
		// data/regex/labels.<game>.json, verbatim) fills the keys it lacks
		// (data.ts:186 mergeLabels). A missing or broken stand-in = no fallback.
		{
			std::string fb;
			if (ReadFileUtf8(exeDir + L"Data\\regex_labels_" + game + L".json", fb)) {
				try {
					ordered_json fdoc = ordered_json::parse(fb);
					const auto inner = fdoc.find("labels");
					const ordered_json& m = (inner != fdoc.end() && inner->is_object()) ? *inner : fdoc;
					if (m.is_object()) {
						RegexLabels fallback;
						fallback.zh = LabelMap(m, "zh");
						fallback.en = LabelMap(m, "en");
						labels = RegexMergeLabels(labels, fallback);
					}
				} catch (const std::exception& ex) {
					PobLog::Error("data", "regex_labels_" + gameId + ".json parse failed: " + ex.what());
				}
			}
		}
		if (pages_.size() > before) labels_.emplace_back(gameId, std::move(labels));
	} catch (const std::exception& ex) {
		// One bad file must not take the other game's catalogue down with it, so
		// only this file's pages are rolled back.
		if (err) *err = u8"regex_" + gameId + u8".json 解析失敗：" + ex.what();
		PobLog::Error("data", "regex_" + gameId + ".json parse failed: " + ex.what());
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
