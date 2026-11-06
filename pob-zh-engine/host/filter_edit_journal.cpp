#include "filter_edit_journal.h"
#include "filter_doc_editor.h"
#include "filter_parser.h"
#include "custom_rules_io.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp>   // nlohmann (deps/nlohmann)

#include <algorithm>
#include <fstream>
#include <iterator>

using nlohmann::ordered_json;

namespace {

bool IsSyntax(FilterLineKind k)
{
	return k == FilterLineKind::BlockHeader || k == FilterLineKind::Condition || k == FilterLineKind::Action ||
	       k == FilterLineKind::Unknown;
}

// "#!" after the indent: a line this editor disabled.
bool IsDisabled(const FilterLine& ln)
{
	return ln.kind == FilterLineKind::Comment && ln.raw.compare(ln.indent.size(), 2, "#!") == 0;
}

std::string Trim(const std::string& s)
{
	const size_t a = s.find_first_not_of(" \t\r");
	if (a == std::string::npos) return std::string();
	const size_t b = s.find_last_not_of(" \t\r");
	return s.substr(a, b - a + 1);
}

// What a line of a rule sets ("SetTextColor"): a disabled "#! SetTextColor ..."
// is the same setting switched off. The header is "header"; plain comments
// are "#comment".
std::string BaseSetting(const std::string& text)
{
	const FilterLine ln = ParseFilterLine(text);
	if (ln.kind == FilterLineKind::BlockHeader) return "header";
	if (IsSyntax(ln.kind)) return ln.keyword;
	if (IsDisabled(ln)) {
		const FilterLine in = ParseFilterLine(ln.raw.substr(ln.indent.size() + 2));
		if (IsSyntax(in.kind) && in.kind != FilterLineKind::BlockHeader) return in.keyword;
	}
	return "#comment";
}

// A rule's lines as (setting, text), a repeated setting numbered ("BaseType#2").
using Settings = std::vector<std::pair<std::string, std::string>>;

Settings ToSettings(const std::vector<std::string>& lines)
{
	Settings out;
	std::map<std::string, int> seen;
	for (const std::string& t : lines) {
		const std::string base = BaseSetting(t);
		const int n = ++seen[base];
		out.emplace_back(n == 1 ? base : base + "#" + std::to_string(n), t);
	}
	return out;
}

const std::string* Find(const Settings& s, const std::string& id)
{
	for (const auto& p : s)
		if (p.first == id) return &p.second;
	return nullptr;
}

std::string SettingName(const std::string& id)
{
	const size_t h = id.find('#', 1);
	return h == std::string::npos ? id : id.substr(0, h);
}

int FindByKey(const FilterFile& f, const std::string& key)
{
	for (int bi = 0; bi < (int)f.blocks.size(); bi++)
		if (FilterRuleKey(f, f.blocks[bi]) == key) return bi;
	return -1;
}

// Index of the rule's own last line (header through its last syntax / "#!").
int OwnEnd(const FilterFile& f, const FilterBlock& b)
{
	int end = b.headerLineIdx;
	for (int li : b.lineIdx) {
		const FilterLine& ln = f.lines[li];
		if (IsSyntax(ln.kind) || IsDisabled(ln)) end = (std::max)(end, li);
	}
	return end;
}

std::string CustomZoneText(const FilterFile& f)
{
	const CustomZone z = FindCustomZone(f);
	if (!z.present()) return std::string();
	std::string out;
	for (const FilterBlock& b : f.blocks) {
		if (b.headerLineIdx <= z.beginLine || b.headerLineIdx >= z.endLine) continue;
		if (!out.empty()) out += "\r\n";
		for (const std::string& t : FilterOwnLines(f, b)) out += t + "\r\n";
	}
	return out;
}

// Your edits merged onto NeverSink's new version of one rule. Returns the new
// lines; conflicts are reported (and resolved by `choices`, yours by default).
std::vector<std::string> Merge(const std::string& key, const JournalRule& r, const std::vector<std::string>& theirs,
                               const JournalChoices& choices, std::vector<JournalConflict>* conflicts)
{
	const Settings B = ToSettings(r.before), M = ToSettings(r.after), T = ToSettings(theirs);
	auto useNew = [&](const std::string& id) {
		auto it = choices.find(key + "|" + SettingName(id));
		return it != choices.end() && it->second;
	};
	std::vector<std::string> out;
	for (const auto& [id, t] : T) {
		const std::string* b = Find(B, id);
		const std::string* m = Find(M, id);
		const bool mineChanged = !(b && m && *b == *m) && !(!b && !m);
		const bool theirsChanged = !(b && *b == t);
		if (!mineChanged) { out.push_back(t); continue; }              // only NeverSink may have changed it
		if (!theirsChanged) { if (m) out.push_back(*m); continue; }    // only you changed it (or removed it)
		if (m && *m == t) { out.push_back(t); continue; }              // both made the same change
		conflicts->push_back({ key, SettingName(id), m ? *m : std::string(), t });
		if (useNew(id)) out.push_back(t);
		else if (m) out.push_back(*m);
	}
	// lines you added, or that NeverSink dropped while you had changed them
	for (const auto& [id, m] : M) {
		if (Find(T, id)) continue;
		const std::string* b = Find(B, id);
		if (b && *b == m) continue;   // you did not touch it; NeverSink removed it
		if (b) {
			conflicts->push_back({ key, SettingName(id), m, std::string() });
			if (useNew(id)) continue;
		}
		out.push_back(m);
	}
	return out;
}

// Make the rule's own lines `want`, keeping the uid of every line that stays
// (so the baseline's markers and undo keep working).
void Rewrite(FilterDocumentEditor& doc, const std::string& key, const std::vector<std::string>& want)
{
	FilterFile& f = *doc.file();
	int bi = FindByKey(f, key);
	if (bi < 0) return;
	const Settings W = ToSettings(want);
	// 1. lines that stay: changed in place
	{
		const FilterBlock& b = f.blocks[bi];
		const int end = OwnEnd(f, b);
		std::map<std::string, int> seen;
		for (int li = b.headerLineIdx; li <= end; li++) {
			const std::string text = FilterSerializeLine(f.lines[li]);
			const std::string base = BaseSetting(text);
			const int n = ++seen[base];
			const std::string id = n == 1 ? base : base + "#" + std::to_string(n);
			const std::string* w = Find(W, id);
			if (!w || *w == text) continue;
			FilterLine nl = ParseFilterLine(*w);
			nl.uid = f.lines[li].uid;
			f.lines[li] = nl;
			f.dirty = true;
		}
		doc.RebuildBlocks();
	}
	// 2. lines that go (from the bottom, so indices above stay valid)
	{
		bi = FindByKey(f, key);
		if (bi < 0) return;
		const FilterBlock& b = f.blocks[bi];
		const int end = OwnEnd(f, b);
		std::vector<int> drop;
		std::map<std::string, int> seen;
		for (int li = b.headerLineIdx; li <= end; li++) {
			const std::string base = BaseSetting(FilterSerializeLine(f.lines[li]));
			const int n = ++seen[base];
			if (!Find(W, n == 1 ? base : base + "#" + std::to_string(n)) && base != "#comment") drop.push_back(li);
		}
		for (auto it = drop.rbegin(); it != drop.rend(); ++it) doc.RemoveLine(*it);
	}
	// 3. lines that are new
	{
		bi = FindByKey(f, key);
		if (bi < 0) return;
		const Settings now = ToSettings(FilterOwnLines(f, f.blocks[bi]));
		for (const auto& [id, text] : W) {
			if (Find(now, id)) continue;
			FilterLine ln = ParseFilterLine(text);
			bool disable = false;
			if (IsDisabled(ln)) {
				ln = ParseFilterLine(ln.raw.substr(ln.indent.size() + 2));
				disable = true;
			}
			if (!IsSyntax(ln.kind) || ln.kind == FilterLineKind::BlockHeader) continue;
			bi = FindByKey(f, key);
			if (bi < 0) return;
			const int li = doc.InsertLine(bi, ln.keyword, ln.op, ln.values);
			if (disable && li >= 0) doc.CommentOutLine(li);
		}
	}
	f.dirty = true;
	doc.RebuildBlocks();
}

std::string Narrow(const std::wstring& w)
{
	if (w.empty()) return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

} // namespace

std::string FilterRuleKey(const FilterFile&, const FilterBlock& b)
{
	const std::string& h = b.headerComment;
	const size_t t = h.find("$type->");
	const size_t r = h.find("$tier->");
	if (t == std::string::npos) return std::string();
	auto token = [&](size_t at) {
		const size_t e = h.find_first_of(" \t", at);
		return h.substr(at, e == std::string::npos ? std::string::npos : e - at);
	};
	std::string key = token(t);
	if (r != std::string::npos) key += " " + token(r);
	return key;
}

std::string FilterBannerValue(const FilterFile& f, const char* field)
{
	const std::string want = std::string(field) + ":";
	const size_t n = (std::min)(f.lines.size(), (size_t)80);
	for (size_t i = 0; i < n; i++) {
		const FilterLine& ln = f.lines[i];
		if (ln.kind != FilterLineKind::Comment) continue;
		const std::string t = Trim(ln.raw.substr(ln.raw.find('#') + 1));
		if (t.compare(0, want.size(), want) == 0) return Trim(t.substr(want.size()));
	}
	return std::string();
}

std::vector<std::string> FilterOwnLines(const FilterFile& f, const FilterBlock& b)
{
	std::vector<std::string> out;
	if (b.headerLineIdx < 0) return out;
	const int end = OwnEnd(f, b);
	for (int li = b.headerLineIdx; li <= end; li++) out.push_back(FilterSerializeLine(f.lines[li]));
	return out;
}

void JournalRecord(EditJournal& j, const FilterFile& baseline, const FilterFile& current)
{
	j.filterType = FilterBannerValue(current, "TYPE");
	j.fromVersion = FilterBannerValue(current, "VERSION");
	std::map<std::string, const FilterBlock*> base;
	for (const FilterBlock& b : baseline.blocks) {
		const std::string k = FilterRuleKey(baseline, b);
		if (!k.empty()) base.emplace(k, &b);
	}
	for (const FilterBlock& b : current.blocks) {
		const std::string k = FilterRuleKey(current, b);
		if (k.empty()) continue;
		auto bit = base.find(k);
		if (bit == base.end()) continue;
		const std::vector<std::string> now = FilterOwnLines(current, b);
		auto it = j.rules.find(k);
		if (it != j.rules.end()) {
			it->second.after = now;
			if (it->second.after == it->second.before) j.rules.erase(it);
			continue;
		}
		const std::vector<std::string> was = FilterOwnLines(baseline, *bit->second);
		if (now != was) j.rules[k] = JournalRule{ was, now };
	}
	j.customZone = CustomZoneText(current);
}

JournalReport JournalApply(const EditJournal& j, FilterDocumentEditor& doc, const JournalChoices& choices, bool dryRun)
{
	if (dryRun) {
		// the same merge on a copy: the report is exactly what a real apply gives
		FilterFile copy = *doc.file();
		FilterDocumentEditor tmp;
		tmp.Attach(&copy);
		return JournalApply(j, tmp, choices, false);
	}
	JournalReport rep;
	FilterFile& f = *doc.file();
	for (const auto& [key, rule] : j.rules) {
		const int bi = FindByKey(f, key);
		if (bi < 0) { rep.missing.push_back(key); continue; }
		const std::vector<std::string> theirs = FilterOwnLines(f, f.blocks[bi]);
		if (theirs == rule.after) { rep.already++; continue; }
		const std::vector<std::string> want = Merge(key, rule, theirs, choices, &rep.conflicts);
		if (want == theirs) { rep.already++; continue; }
		Rewrite(doc, key, want);
		rep.applied++;
	}
	if (!j.customZone.empty()) {
		std::string err;
		const int n = ImportCustomRules(doc, j.customZone, &err);
		if (n > 0) rep.customImported = n;
	}
	return rep;
}

bool JournalSave(const std::wstring& path, const EditJournal& j, std::string* err)
{
	ordered_json doc;
	doc["schema"] = 1;
	doc["filterType"] = j.filterType;
	doc["fromVersion"] = j.fromVersion;
	doc["customZone"] = j.customZone;
	ordered_json rules = ordered_json::object();
	for (const auto& [k, r] : j.rules) rules[k] = { { "before", r.before }, { "after", r.after } };
	doc["rules"] = rules;
	const std::string text = doc.dump(1, '\t');
	const std::wstring tmp = path + L".tmp";
	{
		std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
		if (!o) { if (err) *err = "cannot write " + Narrow(tmp); return false; }
		o.write(text.data(), (std::streamsize)text.size());
		if (!o) { if (err) *err = "write failed"; return false; }
	}
	if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(tmp.c_str());
		if (err) *err = "cannot replace " + Narrow(path);
		return false;
	}
	return true;
}

bool JournalLoad(const std::wstring& path, EditJournal* j)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	ordered_json doc;
	try { doc = ordered_json::parse(text); } catch (...) { return false; }
	if (!doc.is_object()) return false;
	EditJournal out;
	out.filterType = doc.value("filterType", std::string());
	out.fromVersion = doc.value("fromVersion", std::string());
	out.customZone = doc.value("customZone", std::string());
	if (doc.contains("rules") && doc["rules"].is_object())
		for (auto& [k, v] : doc["rules"].items()) {
			JournalRule r;
			if (v.contains("before") && v["before"].is_array()) r.before = v["before"].get<std::vector<std::string>>();
			if (v.contains("after") && v["after"].is_array()) r.after = v["after"].get<std::vector<std::string>>();
			out.rules[k] = std::move(r);
		}
	*j = std::move(out);
	return true;
}

OnlineFilterHeader ParseOnlineHeader(const std::string& text)
{
	OnlineFilterHeader h;
	size_t p = 0;
	if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) p = 3;
	bool first = true;
	while (p < text.size()) {
		size_t e = text.find('\n', p);
		if (e == std::string::npos) e = text.size();
		std::string line = text.substr(p, e - p);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (first) {
			if (line != "#Online Item Filter") return h;
			h.online = true;
			first = false;
		} else {
			const size_t c = line.find(':');
			if (line.size() < 2 || line[0] != '#' || c == std::string::npos || line.compare(0, 2, "#=") == 0) break;
			const std::string k = line.substr(1, c - 1), v = Trim(line.substr(c + 1));
			if (k == "name") h.name = v;
			else if (k == "filterVersion") h.filterVersion = v;
		}
		p = e + 1;
	}
	return h;
}

std::string StripOnlineHeader(const std::string& text)
{
	size_t p = 0;
	if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) p = 3;
	bool first = true;
	while (p < text.size()) {
		size_t e = text.find('\n', p);
		const size_t next = e == std::string::npos ? text.size() : e + 1;
		std::string line = text.substr(p, (e == std::string::npos ? text.size() : e) - p);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (first) {
			if (line != "#Online Item Filter") return text;
			first = false;
		} else {
			const size_t c = line.find(':');
			if (line.size() < 2 || line[0] != '#' || c == std::string::npos || line.compare(0, 2, "#=") == 0) break;
		}
		p = next;
	}
	return text.substr(p);
}
