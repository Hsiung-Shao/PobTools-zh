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

// The line's content without indent and without a "#!" switch-off.
std::string Canon(const std::string& text)
{
	std::string t = Trim(text);
	if (t.compare(0, 2, "#!") == 0) t = Trim(t.substr(2));
	return t;
}

// The condition's operator ("" for actions), to pair "ItemLevel >= 75" with
// "ItemLevel >= 70" rather than with "ItemLevel < 84".
std::string OpOf(const std::string& text)
{
	return ParseFilterLine(Canon(text)).op;
}

// A rule's lines as (setting id, text). Ids are anchored to NeverSink's old
// version B: a line in M or T takes the id of the B line it is (same text,
// then same content switched on/off, then same keyword and operator, then the
// same keyword in order); what is left is a new line. So a repeated keyword
// (three HasExplicitMod) is followed by content, not by position.
struct Line { std::string id, text; };
using Settings = std::vector<Line>;

Settings AnchorB(const std::vector<std::string>& lines)
{
	Settings out;
	std::map<std::string, int> seen;
	for (const std::string& t : lines) {
		const std::string base = BaseSetting(t);
		out.push_back({ base == "header" ? base : base + "#" + std::to_string(++seen[base]), t });
	}
	return out;
}

Settings AlignTo(const Settings& B, const std::vector<std::string>& lines, const char* newTag)
{
	Settings out(lines.size());
	std::vector<bool> used(B.size(), false);
	std::vector<std::string> base(lines.size());
	for (size_t i = 0; i < lines.size(); i++) {
		out[i].text = lines[i];
		base[i] = BaseSetting(lines[i]);
	}
	auto pass = [&](auto same) {
		for (size_t i = 0; i < lines.size(); i++) {
			if (!out[i].id.empty()) continue;
			for (size_t k = 0; k < B.size(); k++) {
				if (used[k] || BaseSetting(B[k].text) != base[i] || !same(B[k].text, lines[i])) continue;
				out[i].id = B[k].id;
				used[k] = true;
				break;
			}
		}
	};
	pass([](const std::string& b, const std::string& x) { return b == x; });
	pass([](const std::string& b, const std::string& x) { return Canon(b) == Canon(x); });
	pass([](const std::string& b, const std::string& x) { return OpOf(b) == OpOf(x); });
	pass([](const std::string&, const std::string&) { return true; });
	std::map<std::string, int> fresh;
	for (size_t i = 0; i < lines.size(); i++)
		if (out[i].id.empty()) out[i].id = base[i] + "#" + newTag + std::to_string(++fresh[base[i]]);
	return out;
}

const std::string* Find(const Settings& s, const std::string& id)
{
	for (const Line& l : s)
		if (l.id == id) return &l.text;
	return nullptr;
}

std::string SettingName(const std::string& id)
{
	const size_t h = id.find('#', 1);
	return h == std::string::npos ? id : id.substr(0, h);
}

// NeverSink's own rules by identity. A rule in the custom zone is the user's
// (a 複製成新的自訂規則 copy keeps NeverSink's header) and never stands for the
// NeverSink rule; an identity that occurs twice cannot be told apart and is
// left out altogether.
std::map<std::string, int> RuleIndex(const FilterFile& f)
{
	const CustomZone z = FindCustomZone(f);
	std::map<std::string, int> idx;
	std::map<std::string, int> count;
	for (int bi = 0; bi < (int)f.blocks.size(); bi++) {
		const int h = f.blocks[bi].headerLineIdx;
		if (z.present() && h > z.beginLine && h < z.endLine) continue;
		const std::string k = FilterRuleKey(f, f.blocks[bi]);
		if (k.empty()) continue;
		if (++count[k] == 1) idx[k] = bi;
	}
	for (const auto& [k, n] : count)
		if (n > 1) idx.erase(k);
	return idx;
}

int FindByKey(const FilterFile& f, const std::string& key)
{
	const std::map<std::string, int> idx = RuleIndex(f);
	const auto it = idx.find(key);
	return it == idx.end() ? -1 : it->second;
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

// Three-way on one value: yours if only you changed it, NeverSink's if only it
// did, a conflict when both did differently (decided by `useNew`).
const std::string* Pick3(const std::string* b, const std::string* m, const std::string* t, bool* conflict, bool useNew)
{
	auto eq = [](const std::string* x, const std::string* y) { return (!x && !y) || (x && y && *x == *y); };
	*conflict = false;
	if (eq(m, b)) return t;
	if (eq(t, b)) return m;
	if (eq(m, t)) return t;
	*conflict = true;
	return useNew ? t : m;
}

// A header "Show # $type->..." as its keyword and the rest (NeverSink's comment).
void SplitHeader(const std::string& text, std::string* indent, std::string* kw, std::string* rest)
{
	const size_t a = text.find_first_not_of(" \t");
	const size_t e = a == std::string::npos ? text.size() : text.find_first_of(" \t#", a);
	*indent = text.substr(0, a == std::string::npos ? text.size() : a);
	*kw = a == std::string::npos ? std::string() : text.substr(a, (e == std::string::npos ? text.size() : e) - a);
	*rest = e == std::string::npos ? std::string() : text.substr(e);
}

// Your edits merged onto NeverSink's new version of one rule: the lines to have
// (by setting id) and the ids of NeverSink's new lines, in their order.
// Conflicts are reported (and resolved by `choices`, yours by default).
Settings Merge(const std::string& key, const JournalRule& r, const std::vector<std::string>& theirs,
               const JournalChoices& choices, std::vector<JournalConflict>* conflicts, Settings* tOut)
{
	const Settings B = AnchorB(r.before);
	Settings M = AlignTo(B, r.after, "m");
	Settings T = AlignTo(B, theirs, "t");
	// a line both of you added, word for word, is the same line
	for (Line& m : M) {
		if (Find(B, m.id)) continue;
		for (const Line& t : T)
			if (!Find(B, t.id) && t.text == m.text) { m.id = t.id; break; }
	}
	*tOut = T;
	auto useNew = [&](const std::string& setting) {
		auto it = choices.find(key + "|" + setting);
		return it != choices.end() && it->second;
	};
	Settings out;
	for (const Line& t : T) {
		const std::string* b = Find(B, t.id);
		const std::string* m = Find(M, t.id);
		if (t.id == "header" && b && m) {
			// Show/Hide is yours to change, the comment NeverSink's: one setting each
			std::string bi, bk, br, mi, mk, mr, ti, tk, tr;
			SplitHeader(*b, &bi, &bk, &br);
			SplitHeader(*m, &mi, &mk, &mr);
			SplitHeader(t.text, &ti, &tk, &tr);
			bool c1 = false, c2 = false;
			const std::string* kw = Pick3(&bk, &mk, &tk, &c1, useNew("Show/Hide"));
			const std::string* rest = Pick3(&br, &mr, &tr, &c2, useNew("header"));
			if (c1) conflicts->push_back({ key, "Show/Hide", mk, tk });
			if (c2) conflicts->push_back({ key, "header", *m, t.text });
			out.push_back({ t.id, ti + (kw ? *kw : tk) + (rest ? *rest : tr) });
			continue;
		}
		bool c = false;
		const std::string* pick = Pick3(b, m, &t.text, &c, useNew(SettingName(t.id)));
		if (c) conflicts->push_back({ key, SettingName(t.id), m ? *m : std::string(), t.text });
		if (pick) out.push_back({ t.id, *pick });
	}
	// lines NeverSink's new version does not have: yours to add, or one you
	// changed that NeverSink dropped (a conflict)
	for (const Line& m : M) {
		if (Find(T, m.id)) continue;
		const std::string* b = Find(B, m.id);
		bool c = false;
		const std::string* pick = Pick3(b, &m.text, nullptr, &c, useNew(SettingName(m.id)));
		// switched off by you, removed by NeverSink: both mean "not this line"
		if (c && b && m.text.find("#!") != std::string::npos && Canon(m.text) == Canon(*b)) c = false, pick = nullptr;
		if (c) conflicts->push_back({ key, SettingName(m.id), m.text, std::string() });
		if (pick) out.push_back({ m.id, *pick });
	}
	return out;
}

// Make the rule's own lines `want`, keeping the uid of every line that stays
// (so the baseline's markers and undo keep working). `tIds` are the setting ids
// of the rule's current own lines, in order (from Merge).
void Rewrite(FilterDocumentEditor& doc, const std::string& key, const Settings& tIds, const Settings& want)
{
	FilterFile& f = *doc.file();
	int bi = FindByKey(f, key);
	if (bi < 0) return;
	// 1. lines that stay: changed in place
	{
		const FilterBlock& b = f.blocks[bi];
		const int end = OwnEnd(f, b);
		for (int li = b.headerLineIdx, i = 0; li <= end && i < (int)tIds.size(); li++, i++) {
			const std::string* w = Find(want, tIds[i].id);
			if (!w || *w == FilterSerializeLine(f.lines[li])) continue;
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
		for (int li = b.headerLineIdx, i = 0; li <= end && i < (int)tIds.size(); li++, i++)
			if (!Find(want, tIds[i].id) && SettingName(tIds[i].id) != "#comment") drop.push_back(li);
		for (auto it = drop.rbegin(); it != drop.rend(); ++it) doc.RemoveLine(*it);
	}
	// 3. lines that are new: inserted where the editor puts that kind of line,
	//    then set to the exact text (indent, trailing comment, "#!")
	for (const Line& w : want) {
		if (Find(tIds, w.id)) continue;
		FilterLine ln = ParseFilterLine(w.text);
		if (IsDisabled(ln)) ln = ParseFilterLine(ln.raw.substr(ln.indent.size() + 2));
		if (!IsSyntax(ln.kind) || ln.kind == FilterLineKind::BlockHeader) continue;
		bi = FindByKey(f, key);
		if (bi < 0) return;
		const int li = doc.InsertLine(bi, ln.keyword, ln.op, ln.values);
		if (li < 0) continue;
		FilterLine exact = ParseFilterLine(w.text);
		exact.uid = f.lines[li].uid;
		f.lines[li] = exact;
		doc.RebuildBlocks();
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
	// The baseline is NeverSink's untouched new version when its version differs
	// from the journal's: an entry carried onto it (the rule now differs from
	// the baseline) takes that version as its "before", so the next update
	// compares against what NeverSink shipped last, not what it shipped years
	// ago. An entry not carried onto it yet stays as it is (still pending).
	const std::string baseVersion = FilterBannerValue(baseline, "VERSION");
	const std::map<std::string, int> base = RuleIndex(baseline);
	const std::map<std::string, int> cur = RuleIndex(current);
	for (const auto& [k, ci] : cur) {
		const auto bit = base.find(k);
		if (bit == base.end()) continue;
		const std::vector<std::string> now = FilterOwnLines(current, current.blocks[ci]);
		const std::vector<std::string> was = FilterOwnLines(baseline, baseline.blocks[bit->second]);
		auto it = j.rules.find(k);
		if (it != j.rules.end()) {
			JournalRule& r = it->second;
			const std::string& rv = r.version.empty() ? j.fromVersion : r.version;
			if (!baseVersion.empty() && !rv.empty() && rv != baseVersion) {
				if (now == was) continue;   // not carried onto this version yet: keep it pending
				r.before = was;             // the baseline is that version as NeverSink shipped it
			}
			r.version = baseVersion.empty() ? rv : baseVersion;
			r.after = now;
			if (r.after == r.before) j.rules.erase(it);
			continue;
		}
		if (now != was) j.rules[k] = JournalRule{ was, now, baseVersion };
	}
	j.filterType = FilterBannerValue(current, "TYPE");
	j.fromVersion = FilterBannerValue(current, "VERSION");
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
		Settings tIds;
		const Settings want = Merge(key, rule, theirs, choices, &rep.conflicts, &tIds);
		std::vector<std::string> wantText;
		for (const Line& l : want) wantText.push_back(l.text);
		if (wantText == theirs) { rep.already++; continue; }
		Rewrite(doc, key, tIds, want);
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
	for (const auto& [k, r] : j.rules) rules[k] = { { "before", r.before }, { "after", r.after }, { "version", r.version } };
	doc["rules"] = rules;
	std::string text;
	try {
		// a line that is not UTF-8 (an old editor's Big5 comment) becomes U+FFFD, not a crash
		text = doc.dump(1, '\t', false, ordered_json::error_handler_t::replace);
	} catch (...) {
		if (err) *err = "cannot encode the journal";
		return false;
	}
	const std::wstring tmp = path + L".tmp";
	{
		std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
		if (!o) { if (err) *err = "cannot write " + Narrow(tmp); return false; }
		o.write(text.data(), (std::streamsize)text.size());
		if (!o) {
			o.close();
			DeleteFileW(tmp.c_str());
			if (err) *err = "write failed";
			return false;
		}
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
	try {
		out.filterType = doc.value("filterType", std::string());
		out.fromVersion = doc.value("fromVersion", std::string());
		out.customZone = doc.value("customZone", std::string());
		if (doc.contains("rules") && doc["rules"].is_object())
			for (auto& [k, v] : doc["rules"].items()) {
				JournalRule r;
				if (v.contains("before") && v["before"].is_array()) r.before = v["before"].get<std::vector<std::string>>();
				if (v.contains("after") && v["after"].is_array()) r.after = v["after"].get<std::vector<std::string>>();
				if (v.contains("version") && v["version"].is_string()) r.version = v["version"].get<std::string>();
				out.rules[k] = std::move(r);
			}
	} catch (...) {
		return false;   // a hand-edited journal with a wrong type: unreadable, not a crash
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
