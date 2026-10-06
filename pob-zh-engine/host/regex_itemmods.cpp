// See regex_itemmods.h. Line references are exile-appraiser
// regex/src/pages/item-mods.ts @41decda.
#include "regex_itemmods.h"

#include "regex_numeric.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <json.hpp>

namespace RegexItemMods {

namespace {

using U16 = std::u16string;

// ---- UTF-8 <-> UTF-16 (JS strings are UTF-16) ----------------------------------

U16 ToU16(const std::string& s)
{
	U16 out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size();) {
		const unsigned char c = (unsigned char)s[i];
		char32_t cp;
		size_t n;
		if (c < 0x80) { cp = c; n = 1; }
		else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); n = 2; }
		else if ((c >> 4) == 14 && i + 2 < s.size()) {
			cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
			n = 3;
		} else if ((c >> 3) == 30 && i + 3 < s.size()) {
			cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
			n = 4;
		} else { cp = 0xFFFD; n = 1; }
		if (cp >= 0x10000) {
			cp -= 0x10000;
			out += (char16_t)(0xD800 + (cp >> 10));
			out += (char16_t)(0xDC00 + (cp & 0x3FF));
		} else {
			out += (char16_t)cp;
		}
		i += n;
	}
	return out;
}

std::string ToU8(const U16& s)
{
	std::string out;
	out.reserve(s.size() * 2);
	for (size_t i = 0; i < s.size(); i++) {
		char32_t cp = s[i];
		if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
			cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
			i++;
		}
		if (cp < 0x80) out += (char)cp;
		else if (cp < 0x800) {
			out += (char)(0xC0 | (cp >> 6));
			out += (char)(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			out += (char)(0xE0 | (cp >> 12));
			out += (char)(0x80 | ((cp >> 6) & 0x3F));
			out += (char)(0x80 | (cp & 0x3F));
		} else {
			out += (char)(0xF0 | (cp >> 18));
			out += (char)(0x80 | ((cp >> 12) & 0x3F));
			out += (char)(0x80 | ((cp >> 6) & 0x3F));
			out += (char)(0x80 | (cp & 0x3F));
		}
	}
	return out;
}

// JS toLowerCase for the scripts the game's English text uses: ASCII, Latin-1,
// Latin Extended-A, Greek, Cyrillic (simple mappings) and U+0130 -> "i̇"
// (the one full mapping that changes the length there).
U16 Lower(const U16& s)
{
	U16 out;
	out.reserve(s.size());
	for (char16_t c : s) {
		if (c >= u'A' && c <= u'Z') out += (char16_t)(c + 32);
		else if (c < 0xC0) out += c;
		else if (c <= 0xDE && c != 0xD7) out += (char16_t)(c + 32);
		else if (c == 0x130) { out += u'i'; out += (char16_t)0x307; }
		else if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) out += (char16_t)((c & 1) ? c : c + 1);
		else if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) out += (char16_t)((c & 1) ? c + 1 : c);
		else if (c == 0x178) out += (char16_t)0xFF;
		else if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) out += (char16_t)(c + 32);
		else if (c >= 0x410 && c <= 0x42F) out += (char16_t)(c + 32);
		else if (c >= 0x400 && c <= 0x40F) out += (char16_t)(c + 80);
		else out += c;
	}
	return out;
}

// JS WhiteSpace + LineTerminator (String.prototype.trim, \s).
bool IsJsSpace(char16_t c)
{
	return c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20 || c == 0xA0 || c == 0x1680 ||
	       (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 ||
	       c == 0xFEFF;
}

U16 Trim(const U16& s)
{
	size_t a = 0, b = s.size();
	while (a < b && IsJsSpace(s[a])) a++;
	while (b > a && IsJsSpace(s[b - 1])) b--;
	return s.substr(a, b - a);
}

std::vector<U16> SplitNl(const U16& s)
{
	std::vector<U16> out;
	size_t start = 0;
	for (;;) {
		const size_t p = s.find(u'\n', start);
		if (p == U16::npos) {
			out.push_back(s.substr(start));
			return out;
		}
		out.push_back(s.substr(start, p - start));
		start = p + 1;
	}
}

bool IsDigit(char16_t c) { return c >= u'0' && c <= u'9'; }

const char16_t kSlot = 0;   // :140 SLOT, a '#' (a run of digits)

// :147 stripPlus: drop '+' right before '#'
U16 StripPlus(const U16& s)
{
	U16 out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++)
		if (!(s[i] == u'+' && i + 1 < s.size() && s[i + 1] == u'#')) out += s[i];
	return out;
}

// :152 normLine
U16 NormLine(const U16& s, bool fold)
{
	U16 t = Trim(StripPlus(s));
	for (char16_t& c : t)
		if (c == u'#') c = kSlot;
	return fold ? Lower(t) : t;
}

int CountHash(const U16& s)
{
	int n = 0;
	for (char16_t c : s) n += (c == u'#');
	return n;
}

bool Contains(const U16& hay, const U16& needle) { return hay.find(needle) != U16::npos; }
bool StartsWith(const U16& s, const U16& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }

// :496 isWordBreak: /[\s,，、:：;；。.!！?？()（）「」『』[\]/·]/
bool IsWordBreak(char16_t c)
{
	if (IsJsSpace(c)) return true;
	static const U16 set = u",，、:：;；。.!！?？()（）「」『』[]/·";
	return set.find(c) != U16::npos;
}

const U16 kMeta = u".^$*+?()[]{}|\\";   // :392 META

U16 Escape16(const U16& s)
{
	U16 out;
	for (char16_t c : s) {
		if (kMeta.find(c) != U16::npos) out += u'\\';
		out += c;
	}
	return out;
}

} // namespace

// ---- index --------------------------------------------------------------------

struct Run {
	int line;
	U16 before, after;
};

struct ModIndex {
	std::vector<U16> lines;
	std::vector<Run> byAfter;   // sorted by `after`
	std::unordered_map<U16, int> lineNo;
	bool fold = false;
};

void IndexDeleter::operator()(ModIndex* p) const { delete p; }
size_t IndexLineCount(const ModIndex& idx) { return idx.lines.size(); }
size_t IndexRunCount(const ModIndex& idx) { return idx.byAfter.size(); }

namespace {

// :171 runsOf
void RunsOf(const U16& line, int idx, std::vector<Run>& out)
{
	size_t i = 0;
	while (i < line.size()) {
		const char16_t c = line[i];
		if (c == kSlot) {
			out.push_back({idx, line.substr(0, i), line.substr(i + 1)});
			i++;
		} else if (IsDigit(c)) {
			size_t j = i;
			while (j < line.size() && IsDigit(line[j])) j++;
			const size_t b = (i > 0 && line[i - 1] == u'+') ? i - 1 : i;
			out.push_back({idx, line.substr(0, b), line.substr(j)});
			i = j;
		} else {
			i++;
		}
	}
}

// :215 lowerBound / :227 prefixRange (on `after`)
size_t LowerBound(const std::vector<Run>& arr, const U16& key)
{
	size_t lo = 0, hi = arr.size();
	while (lo < hi) {
		const size_t mid = (lo + hi) >> 1;
		if (arr[mid].after < key) lo = mid + 1;
		else hi = mid;
	}
	return lo;
}

std::pair<size_t, size_t> PrefixRange(const std::vector<Run>& arr, const U16& prefix)
{
	const size_t a = LowerBound(arr, prefix);
	size_t lo = a, hi = arr.size();
	while (lo < hi) {
		const size_t mid = (lo + hi) >> 1;
		if (StartsWith(arr[mid].after, prefix)) lo = mid + 1;
		else hi = mid;
	}
	return {a, lo};
}

// :266 afterMatches
bool AfterMatches(const U16& other, const U16& s, bool exact)
{
	size_t i = 0, k = 0;
	while (k < s.size()) {
		if (i >= other.size()) return false;
		if (other[i] == kSlot) {
			if (s[k] == u'+') {
				k++;
				if (k >= s.size()) return true;
			}
			if (!IsDigit(s[k])) return false;
			while (k < s.size() && IsDigit(s[k])) k++;
			i++;
			continue;
		}
		if (other[i] != s[k]) return false;
		i++;
		k++;
	}
	return !exact || i == other.size();
}

// :292 suffixMatch
struct SuffixM {
	size_t n;
	bool whole;
};
SuffixM SuffixMatch(const U16& b, const U16& other)
{
	long i = (long)b.size() - 1;
	long o = (long)other.size() - 1;
	while (i >= 0 && o >= 0) {
		if (other[o] == kSlot) {
			if (!IsDigit(b[i])) break;
			while (i >= 0 && IsDigit(b[i])) i--;
			if (i >= 0 && b[i] == u'+') i--;
			o--;
			continue;
		}
		if (other[o] != b[i]) break;
		i--;
		o--;
	}
	return {(size_t)((long)b.size() - 1 - i), i < 0 && o < 0};
}

// :313 plainHead
U16 PlainHead(const U16& s)
{
	for (size_t i = 0; i < s.size(); i++)
		if (IsDigit(s[i])) return s.substr(0, i);
	return s;
}

constexpr size_t kMaxScan = 1500;   // :263 MAX_SCAN

} // namespace

// :184 buildModIndex
ModIndexPtr BuildIndex(const std::vector<StatLite>& stats, Lang lang)
{
	ModIndexPtr idx(new ModIndex);
	idx->fold = (lang == Lang::En);
	for (const StatLite& s : stats)
		for (const std::string& str : s.strings)
			for (const U16& part : SplitNl(ToU16(str))) {
				U16 n = NormLine(part, idx->fold);
				if (n.empty() || idx->lineNo.count(n)) continue;
				idx->lineNo.emplace(n, (int)idx->lines.size());
				idx->lines.push_back(std::move(n));
			}
	std::vector<Run> runs;
	for (size_t i = 0; i < idx->lines.size(); i++) RunsOf(idx->lines[i], (int)i, runs);
	std::stable_sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.after < b.after; });
	idx->byAfter = std::move(runs);
	return idx;
}

// :336 chooseAnchor
std::optional<ModAnchor> ChooseAnchor(const ModIndex& idx, const std::string& tmpl, bool plusHint, int limit,
                                      const std::vector<std::string>& sameLines)
{
	const U16 shown = Trim(StripPlus(ToU16(tmpl)));
	const U16 own = idx.fold ? Lower(shown) : shown;
	const size_t at = own.find(u'#');
	if (at == U16::npos || own.find(u'#', at + 1) != U16::npos || own.size() != shown.size()) return std::nullopt;
	U16 ownNorm = own;
	for (char16_t& c : ownNorm)
		if (c == u'#') c = kSlot;
	std::unordered_set<int> same;
	{
		auto it = idx.lineNo.find(ownNorm);
		if (it != idx.lineNo.end()) same.insert(it->second);
	}
	for (const std::string& s : sameLines)
		for (const U16& part : SplitNl(ToU16(s))) {
			auto it = idx.lineNo.find(NormLine(part, idx.fold));
			if (it != idx.lineNo.end()) same.insert(it->second);
		}
	const U16 B = ownNorm.substr(0, at);
	const U16 A = ownNorm.substr(at + 1);
	const U16 shownB = shown.substr(0, at);
	const U16 shownA = shown.substr(at + 1);
	std::vector<size_t> pStarts{0};
	for (size_t i = 1; i <= shownB.size(); i++)
		if (IsWordBreak(shownB[i - 1])) pStarts.push_back(i);
	if (std::find(pStarts.begin(), pStarts.end(), shownB.size()) == pStarts.end()) pStarts.push_back(shownB.size());
	std::vector<size_t> sEnds;
	for (size_t k = 0; k < shownA.size(); k++)
		if (IsWordBreak(shownA[k])) sEnds.push_back(k);
	sEnds.push_back(shownA.size());

	struct Best {
		U16 p, s;
		bool caret, dollar, plus;
		int cost;
	};
	std::optional<Best> best;
	auto consider = [&](size_t j, bool caret, size_t k, bool dollar) {
		const U16 p = shownB.substr(shownB.size() - j);
		const U16 s = shownA.substr(0, k);
		if (p.empty() && s.empty() && !caret && !dollar) return;
		const bool plus = plusHint || !p.empty() || caret;
		const int cost = (int)Escape16(p).size() + (int)Escape16(s).size() + (caret ? 1 : 0) + (dollar ? 1 : 0);
		if (cost > limit) return;
		if (!best || cost > best->cost || (cost == best->cost && s.size() > best->s.size()))
			best = Best{p, s, caret, dollar, plus, cost};
	};
	struct NeedP {
		size_t j;
		bool caret;
	};
	auto needP = [&](const std::vector<const Run*>& conflicts) -> std::optional<NeedP> {
		size_t j = 0;
		bool caret = false;
		for (const Run* c : conflicts) {
			if (same.count(c->line)) continue;
			const SuffixM m = SuffixMatch(B, c->before);
			if (m.n >= B.size()) {
				if (m.whole) return std::nullopt;
				caret = true;
			} else {
				j = std::max(j, m.n + 1);
			}
		}
		if (caret) return NeedP{B.size(), true};
		return NeedP{j, false};
	};
	auto conflictsOf = [&](const U16& s, bool exact, std::vector<const Run*>& out) -> bool {
		out.clear();
		const U16 head = PlainHead(s);
		std::pair<size_t, size_t> r = PrefixRange(idx.byAfter, head);
		size_t a = r.first, b = r.second;
		if (exact && head == s) {
			size_t e = a;
			while (e < b && idx.byAfter[e].after == s) e++;
			b = e;
		} else if (b - a > kMaxScan && !exact) {
			return false;
		}
		for (size_t i = a; i < b; i++) {
			const Run& run = idx.byAfter[i];
			if (head == s ? (!exact || run.after == s) : AfterMatches(run.after, s, exact)) out.push_back(&run);
		}
		return true;
	};
	std::vector<const Run*> list;
	auto trySide = [&](size_t k, bool dollar) {
		if (!conflictsOf(A.substr(0, k), dollar, list)) return;
		const std::optional<NeedP> n = needP(list);
		if (!n) return;
		for (size_t start : pStarts) {
			const size_t j = shownB.size() - start;
			if (n->caret) {
				if (start == 0) consider(j, true, k, dollar);
				continue;
			}
			if (j >= n->j) consider(j, false, k, dollar);
			if (start == 0) consider(j, true, k, dollar);
		}
	};
	trySide(A.size(), true);
	for (size_t k : sEnds) trySide(k, false);
	if (!best) return std::nullopt;
	ModAnchor out;
	out.p = ToU8(best->p);
	out.s = ToU8(best->s);
	out.caret = best->caret;
	out.dollar = best->dollar;
	out.plus = best->plus;
	out.cost = best->cost;
	return out;
}

// ---- small pure pieces -----------------------------------------------------------

const char* PageId(const std::string& game) { return game == "poe2" ? "item_mod_values_poe2" : "item_mod_values"; }
bool IsPageId(const std::string& id) { return id == "item_mod_values" || id == "item_mod_values_poe2"; }

static const char* const kCatZh[kCategoryCount] = {u8"生命", u8"魔力", u8"能量護盾", u8"抗性", u8"屬性",
                                                   u8"攻速 / 施速", u8"傷害", u8"移動速度", u8"其他"};
static const char* const kCatEn[kCategoryCount] = {"Life", "Mana", "Energy Shield", "Resistances", "Attributes",
                                                   "Attack / Cast Speed", "Damage", "Movement Speed", "Other"};
const char* CategoryZh(int i) { return i >= 0 && i < kCategoryCount ? kCatZh[i] : ""; }
const char* CategoryEn(int i) { return i >= 0 && i < kCategoryCount ? kCatEn[i] : ""; }

namespace {

// JS \b with the ASCII word class [A-Za-z0-9_] (the ref is lower-cased first).
bool IsWordChar(char16_t c)
{
	return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'_';
}

bool HasWord(const U16& r, const U16& w)
{
	for (size_t p = r.find(w); p != U16::npos; p = r.find(w, p + 1)) {
		const bool l = p == 0 || !IsWordChar(r[p - 1]);
		const bool rr = p + w.size() >= r.size() || !IsWordChar(r[p + w.size()]);
		if (l && rr) return true;
	}
	return false;
}

} // namespace

// :75 itemModCategory
Category CategoryOf(const std::string& ref)
{
	const U16 r = Lower(ToU16(ref));
	if (HasWord(r, u"movement speed")) return Category::Move;
	if (HasWord(r, u"resistance") || HasWord(r, u"resistances")) return Category::Resist;
	if (HasWord(r, u"strength") || HasWord(r, u"dexterity") || HasWord(r, u"intelligence") || HasWord(r, u"attributes"))
		return Category::Attr;
	if (HasWord(r, u"energy shield")) return Category::Es;
	if (HasWord(r, u"life")) return Category::Life;
	if (HasWord(r, u"mana")) return Category::Mana;
	if (HasWord(r, u"attack speed") || HasWord(r, u"cast speed")) return Category::Speed;
	if (HasWord(r, u"damage")) return Category::Damage;
	return Category::Other;
}

std::string JsLower(const std::string& s) { return ToU8(Lower(ToU16(s))); }

std::string EscapeFragText(const std::string& s) { return ToU8(Escape16(ToU16(s))); }

// :418 itemModFragment
std::optional<std::string> Fragment(const ModAnchor& a, const AlgoValue& v)
{
	using RegexFrag::RangeOp;
	const RangeOp op = RegexFrag::RangeOpOf(v);
	if (op == RangeOp::None) return std::nullopt;
	if (op != RangeOp::Ge && (*v.max > kItemModMax || *v.max < 0)) return std::nullopt;
	RegexNumeric::NumRange r;
	if (op != RangeOp::Le) r.min = v.min;
	if (op != RangeOp::Ge) r.max = v.max;
	const std::string num = RegexNumeric::ReadableRangeRegex(r, 3, op == RangeOp::Ge);
	if (num.empty()) return std::nullopt;
	const bool bounded = op != RangeOp::Ge;
	const std::string left = bounded && a.p.empty() && !a.caret ? "(^|[^0-9])" : "";
	const std::string right = bounded && a.s.empty() && !a.dollar ? "([^0-9]|$)" : "";
	std::string p = EscapeFragText(a.p);
	if (!a.caret && !p.empty() && p[0] == '!') p = "\\" + p;
	return std::string(a.caret ? "^" : "") + p + left + (a.plus ? "\\+?" : "") + num + EscapeFragText(a.s) + right +
	       (a.dollar ? "$" : "");
}

static const char* const kReasonIds[kReasonCount] = {"decimal", "multi_value", "multi_form", "multiline",
                                                     "missing_lang", "no_unique", "too_long"};
const char* ReasonId(int r) { return r >= 0 && r < kReasonCount ? kReasonIds[r] : ""; }

// ---- data file -------------------------------------------------------------------

bool ParseFile(const std::string& body, std::vector<StatLite>& zh, std::vector<StatLite>& en, std::string* err)
{
	using nlohmann::json;
	zh.clear();
	en.clear();
	try {
		const json doc = json::parse(body);
		const auto st = doc.find("stats");
		if (st == doc.end() || !st->is_array()) {
			if (err) *err = u8"資料檔缺少 stats 陣列";
			return false;
		}
		zh.reserve(st->size() + 1);
		en.reserve(st->size() + 1);
		for (const json& s : *st) {
			if (!s.is_object()) continue;
			StatLite base;
			base.ref = s.value("ref", std::string());
			base.statId = s.value("id", std::string());
			base.hasId = true;
			base.dp = s.value("dp", false);
			for (int li = 0; li < 2; li++) {
				StatLite l = base;
				const auto rows = s.find(li == 0 ? "zh" : "en");
				if (rows != s.end() && rows->is_array())
					for (const json& r : *rows) {
						if (!r.is_array() || r.size() < 2 || !r[0].is_string()) continue;
						const std::string text = r[0].get<std::string>();
						const int kind = r[1].is_number_integer() ? r[1].get<int>() : 0;
						l.strings.push_back(text);
						if (kind == 0) l.plain.push_back(text);
						if (kind != 1) l.same.push_back(text);
					}
				(li == 0 ? zh : en).push_back(std::move(l));
			}
		}
		for (int li = 0; li < 2; li++) {
			StatLite other;
			const auto o = doc.find(li == 0 ? "otherZh" : "otherEn");
			if (o != doc.end() && o->is_array())
				for (const json& x : *o)
					if (x.is_string()) other.strings.push_back(x.get<std::string>());
			(li == 0 ? zh : en).push_back(std::move(other));
		}
	} catch (const std::exception& ex) {
		if (err) *err = std::string(u8"解析失敗：") + ex.what();
		return false;
	}
	return true;
}

namespace {

std::string KeyOf(const StatLite& s) { return s.hasId ? s.statId + "|" + s.ref : std::string(); }

// :477 templateOf: the template text, or the reason there is none.
bool TemplateOf(const StatLite& s, U16& t, Reason& why)
{
	if (s.dp) { why = Reason::Decimal; return false; }
	std::vector<U16> single;
	for (const std::string& m : s.plain) {
		U16 u = ToU16(m);
		if (CountHash(u) == 1) single.push_back(std::move(u));
	}
	if (single.empty()) { why = Reason::MultiValue; return false; }
	t = Trim(single[0]);
	if (single.size() > 1) {
		std::vector<U16> forms;
		for (const U16& m : single) forms.push_back(Trim(m));
		const U16* core = nullptr;
		for (const U16& f : forms) {
			bool all = true;
			for (const U16& o : forms)
				if (!Contains(o, f)) { all = false; break; }
			if (all) { core = &f; break; }
		}
		if (!core) { why = Reason::MultiForm; return false; }
		t = *core;
	}
	if (t.find(u'\n') != U16::npos) { why = Reason::Multiline; return false; }
	return true;
}

bool HasPlusHash(const std::string& s) { return s.find("+#") != std::string::npos; }

// :549 longOnly
bool LongOnly(const ModIndex& idx, const std::string& tmpl, const std::optional<ModAnchor>& found,
              const std::vector<std::string>& same)
{
	if (found) return false;
	return ChooseAnchor(idx, tmpl, false, 400, same).has_value();
}

} // namespace

// :497 buildItemModData
Data BuildData(const std::string& game, const std::vector<StatLite>& zhStats, const std::vector<StatLite>& enStats)
{
	Data d;
	d.game = game;
	const ModIndexPtr zhIdx = BuildIndex(zhStats, Lang::Zh);
	const ModIndexPtr enIdx = BuildIndex(enStats, Lang::En);
	std::unordered_map<std::string, const StatLite*> enByKey;
	for (const StatLite& s : enStats) {
		const std::string k = KeyOf(s);
		if (!k.empty() && !enByKey.count(k)) enByKey.emplace(k, &s);
	}
	auto exclude = [&](Reason r, const std::string& ref) {
		d.excluded[(int)r]++;
		if (d.samples[(int)r].size() < 8) d.samples[(int)r].push_back(ref);
	};
	std::unordered_set<std::string> seenKey, usedId;
	std::unordered_set<U16> seenText;
	for (const StatLite& zs : zhStats) {
		const std::string key = KeyOf(zs);
		if (key.empty() || seenKey.count(key)) continue;
		seenKey.insert(key);
		d.itemStats++;
		auto eit = enByKey.find(key);
		if (eit == enByKey.end()) { exclude(Reason::MissingLang, zs.ref); continue; }
		const StatLite& es = *eit->second;
		U16 zt, et;
		Reason why;
		if (!TemplateOf(zs, zt, why)) { exclude(why, zs.ref); continue; }
		if (!TemplateOf(es, et, why)) { exclude(why, zs.ref); continue; }
		const U16 textKey = StripPlus(zt) + u'\x01' + Lower(StripPlus(et));
		if (seenText.count(textKey)) { d.merged++; continue; }
		const std::string zt8 = ToU8(zt), et8 = ToU8(et);
		const bool plusHint = HasPlusHash(zs.ref);
		const std::optional<ModAnchor> za = ChooseAnchor(*zhIdx, zt8, plusHint || HasPlusHash(zt8), kMaxAnchorZh, zs.same);
		const std::optional<ModAnchor> ea = ChooseAnchor(*enIdx, et8, plusHint || HasPlusHash(et8), kMaxAnchorEn, es.same);
		if (!za || !ea) {
			const bool tooLong = LongOnly(*zhIdx, zt8, za, zs.same) || LongOnly(*enIdx, et8, ea, es.same);
			exclude(tooLong ? Reason::TooLong : Reason::NoUnique, zs.ref);
			continue;
		}
		seenText.insert(textKey);
		const std::string id = usedId.count(zs.statId) ? key : zs.statId;
		usedId.insert(id);
		Entry e;
		e.id = id;
		e.ref = zs.ref;
		e.zh = zt8;
		e.en = et8;
		e.cat = CategoryOf(zs.ref);
		e.percent = zt8.find("#%") != std::string::npos || et8.find("#%") != std::string::npos;
		e.anchors[0] = *za;
		e.anchors[1] = *ea;
		d.entries.push_back(std::move(e));
	}
	return d;
}

bool LoadFile(const std::wstring& exeDir, const std::string& game, Data& out, std::string* err)
{
	const std::wstring path = exeDir + L"Data\\regex_itemmods_" + (game == "poe2" ? L"poe2" : L"poe1") + L".json";
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		if (err) *err = u8"找不到 Data\\regex_itemmods_" + game + ".json";
		return false;
	}
	std::string body;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 28)) {
		body.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = ReadFile(h, &body[0], (DWORD)body.size(), &read, nullptr) && read == body.size();
	}
	CloseHandle(h);
	if (!ok) {
		if (err) *err = u8"讀不到 Data\\regex_itemmods_" + game + ".json";
		return false;
	}
	std::vector<StatLite> zh, en;
	if (!ParseFile(body, zh, en, err)) return false;
	body.clear();
	body.shrink_to_fit();
	out = BuildData(game, zh, en);
	return true;
}

// :555 itemModPage
RegexAlgo::AlgoPage MakePage(const std::string& game, const Data* data)
{
	using namespace RegexAlgo;
	AlgoPage page;
	page.game = game;
	page.id = PageId(game);
	page.kind = RegexPageKind::Numeric;
	page.title = u8"物品詞綴數值";
	page.titleEn = "Item mod values";
	page.note = std::string(game == "poe2" ? "PoE2" : "PoE1") +
	            u8" 物品詞綴（前綴 / 後綴 / 固定 / 工藝 / 腐化）的數值條件，每條各自一個條件（同時成立）。"
	            u8"模板取自遊戲檔（GGPK），片段預設是整行（行首 / 行尾錨點），太長才以詞為單位往回縮，"
	            u8"並保證在同一份詞綴描述檔的全部文字中唯一；只收恰好一個數值的詞綴（「附加 # 至 # 火焰傷害」這類不收），"
	            u8"小數與負值不支援。";
	page.limit = 250;
	for (int i = 0; i < kCategoryCount; i++) {
		page.groups.push_back(kCatZh[i]);
		page.groupsEn.push_back(kCatEn[i]);
	}
	if (!data) return page;
	page.entries.reserve(data->entries.size());
	for (const Entry& d : data->entries) {
		AlgoEntry e;
		e.def.id = d.id;
		e.def.group = (int)d.cat;
		e.def.zh = {d.zh};
		e.def.en = {d.ref};
		// The English client's template (search reads it too).
		e.def.hiddenEn = {d.en};
		e.input.kind = InputKind::Range;
		e.input.digits = 3;
		e.input.percent = d.percent;
		e.input.ops = {RangeOp::Ge, RangeOp::Le, RangeOp::Range};
		e.input.lo = 0;
		e.input.hi = 999;
		e.input.def.min = 1;
		const ModAnchor az = d.anchors[0], ae = d.anchors[1];
		e.fragment = [az, ae](const AlgoValue& v, Lang lang) { return Fragment(lang == Lang::Zh ? az : ae, v); };
		page.entries.push_back(std::move(e));
	}
	return page;
}

// :601 filterItemMods
Filtered FilterRows(const RegexAlgo::AlgoPage& page, const std::vector<int>& picked, const Filter& f, int cap)
{
	std::vector<U16> words;
	{
		const U16 s = Lower(ToU16(f.search));
		U16 cur;
		for (char16_t c : s) {
			if (IsJsSpace(c)) {
				if (!cur.empty()) words.push_back(cur);
				cur.clear();
			} else {
				cur += c;
			}
		}
		if (!cur.empty()) words.push_back(cur);
	}
	const std::set<int> pickedSet(picked.begin(), picked.end());
	auto match = [&](int i) {
		const RegexAlgo::AlgoEntry& e = page.entries[i];
		if (f.group >= 0 && e.def.group != f.group) return false;
		if (words.empty()) return true;
		std::string hay;
		bool first = true;
		for (const auto* v : {&e.def.zh, &e.def.en, &e.def.hiddenEn})
			for (const std::string& l : *v) {
				if (!first) hay += '\n';
				hay += l;
				first = false;
			}
		const U16 h = Lower(ToU16(hay));
		for (const U16& w : words)
			if (!Contains(h, w)) return false;
		return true;
	};
	Filtered out;
	for (int i : pickedSet)
		if (i >= 0 && i < (int)page.entries.size()) out.rows.push_back(i);
	out.total = (int)out.rows.size();
	if (f.pickedOnly) return out;
	for (int i = 0; i < (int)page.entries.size(); i++) {
		if (pickedSet.count(i) || !match(i)) continue;
		out.total++;
		if ((int)out.rows.size() < cap + (int)pickedSet.size()) out.rows.push_back(i);
	}
	return out;
}

// :624 itemModGroupCounts
std::vector<int> GroupCounts(const RegexAlgo::AlgoPage& page)
{
	std::vector<int> n(kCategoryCount, 0);
	for (const RegexAlgo::AlgoEntry& e : page.entries)
		if (e.def.group >= 0 && e.def.group < kCategoryCount) n[e.def.group]++;
	return n;
}

} // namespace RegexItemMods
