#include "regex_frag.h"

#include "regex_match.h"
#include "regex_numeric.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

// Port of exile-appraiser regex/src/pages/frag.ts; see the header. Line numbers
// in the comments refer to that file.

namespace RegexFrag {

namespace {

using RegexNumeric::NumRange;

void AppendUtf8(std::string& out, char32_t c)
{
	if (c < 0x80) {
		out += (char)c;
	} else if (c < 0x800) {
		out += (char)(0xC0 | (c >> 6));
		out += (char)(0x80 | (c & 0x3F));
	} else if (c < 0x10000) {
		out += (char)(0xE0 | (c >> 12));
		out += (char)(0x80 | ((c >> 6) & 0x3F));
		out += (char)(0x80 | (c & 0x3F));
	} else {
		out += (char)(0xF0 | (c >> 18));
		out += (char)(0x80 | ((c >> 12) & 0x3F));
		out += (char)(0x80 | ((c >> 6) & 0x3F));
		out += (char)(0x80 | (c & 0x3F));
	}
}

std::string Encode(const std::u32string& s)
{
	std::string out;
	for (char32_t c : s) AppendUtf8(out, c);
	return out;
}

// ECMAScript WhiteSpace + LineTerminator: what /\s/u and String.prototype.trim
// both use.
bool IsJsSpace(char32_t c)
{
	return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0xA0 || c == 0x1680 ||
	       (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
	       c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

std::string JsTrim(const std::string& s)
{
	std::u32string u;
	RxDecodeUtf8(s, u);
	size_t a = 0, b = u.size();
	while (a < b && IsJsSpace(u[a])) a++;
	while (b > a && IsJsSpace(u[b - 1])) b--;
	return Encode(u.substr(a, b - a));
}

// The {min, max} the fragment functions hand to the range builders: a >=
// condition drops max, a <= drops min (frag.ts:26, :52, :85).
NumRange RangeFor(const AlgoValue& v, RangeOp op)
{
	NumRange r;
	if (op != RangeOp::Le) r.min = v.min;
	if (op != RangeOp::Ge) r.max = v.max;
	return r;
}

// frag.ts:113 permutations: every distinct ordering of a multiset.
std::vector<std::string> Permutations(std::string letters)
{
	std::vector<std::string> out;
	std::sort(letters.begin(), letters.end());
	std::vector<bool> used(letters.size(), false);
	std::string cur;
	struct Rec {
		const std::string& s;
		std::vector<bool>& used;
		std::string& cur;
		std::vector<std::string>& out;
		void operator()()
		{
			if (cur.size() == s.size()) { out.push_back(cur); return; }
			for (size_t i = 0; i < s.size(); i++) {
				if (used[i]) continue;
				if (i > 0 && s[i] == s[i - 1] && !used[i - 1]) continue;
				used[i] = true;
				cur.push_back(s[i]);
				(*this)();
				cur.pop_back();
				used[i] = false;
			}
		}
	};
	Rec{letters, used, cur, out}();
	return out;
}

// frag.ts:134 Trie; children kept in insertion order (a JS Map).
struct Trie {
	std::vector<std::pair<char, std::unique_ptr<Trie>>> next;
	Trie* Child(char c)
	{
		for (auto& kv : next)
			if (kv.first == c) return kv.second.get();
		next.emplace_back(c, std::make_unique<Trie>());
		return next.back().second.get();
	}
};

// frag.ts:137 renderTrie: siblings with identical subtrees merge into a class
// ([rg]-b); parentheses only below the top and only for several branches.
std::string RenderTrie(const Trie& t, bool top)
{
	if (t.next.empty()) return "";
	std::vector<std::pair<std::string, std::string>> groups;   // subtree -> chars, insertion order
	for (const auto& kv : t.next) {
		std::string s = RenderTrie(*kv.second, false);
		auto it = std::find_if(groups.begin(), groups.end(),
		                       [&](const auto& g) { return g.first == s; });
		if (it != groups.end()) it->second.push_back(kv.first);
		else groups.emplace_back(s, std::string(1, kv.first));
	}
	std::vector<std::string> alts;
	for (auto& g : groups) {
		std::string chs = g.second;
		std::string head;
		if (chs.size() == 1) {
			head = chs;
		} else {
			std::sort(chs.begin(), chs.end());
			head = "[" + chs + "]";
		}
		alts.push_back(head + g.first);
	}
	if (alts.size() == 1) return alts[0];
	std::string joined;
	for (size_t i = 0; i < alts.size(); i++) {
		if (i) joined += '|';
		joined += alts[i];
	}
	return top ? joined : "(" + joined + ")";
}

bool IsColor(char c) { return c == 'r' || c == 'g' || c == 'b' || c == 'w'; }
char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

} // namespace

RangeOp RangeOpOf(const AlgoValue& v)
{
	const bool hasMin = v.min && std::isfinite(*v.min);
	const bool hasMax = v.max && std::isfinite(*v.max);
	if (hasMin && hasMax) return RangeOp::Range;
	if (hasMin) return RangeOp::Ge;
	if (hasMax) return RangeOp::Le;
	return RangeOp::None;
}

std::string LabelBase(const std::string& label)
{
	// label.replace(/#/g, '').replace(/[\s:：]+$/u, '').trim()
	std::u32string u;
	RxDecodeUtf8(label, u);
	u.erase(std::remove(u.begin(), u.end(), U'#'), u.end());
	while (!u.empty() && (IsJsSpace(u.back()) || u.back() == U':' || u.back() == U'：')) u.pop_back();
	return JsTrim(Encode(u));
}

std::optional<std::string> PropertyFragment(const std::string& label, const AlgoValue& v,
                                            int digits, bool percent, bool anchorStart)
{
	const RangeOp op = RangeOpOf(v);
	if (op == RangeOp::None) return std::nullopt;
	const std::string re = RegexNumeric::RangeRegex(RangeFor(v, op), digits);
	if (re.empty()) return std::nullopt;
	const std::string base = LabelBase(label);
	if (base.empty()) return std::nullopt;
	const bool needStart = op != RangeOp::Ge || !percent;
	const bool needEnd = !percent && op != RangeOp::Ge;
	return std::string(anchorStart ? "^" : "") + base + (needStart ? ".*[^\\d]" : ".*") + re +
	       (percent ? "%" : "") + (needEnd ? "$" : "");
}

std::optional<std::string> StrictPropertyFragment(const std::string& label, const AlgoValue& v,
                                                  int digits, bool percent)
{
	const RangeOp op = RangeOpOf(v);
	if (op == RangeOp::None) return std::nullopt;
	const std::string re = RegexNumeric::ReadableRangeRegex(RangeFor(v, op), digits, percent);
	if (re.empty()) return std::nullopt;
	const std::string base = LabelBase(label);
	if (base.empty()) return std::nullopt;
	// R10: the game prints "怪群大小: +13% (augmented)" / "物品數量: +68% (augmented)"
	// (both games, copied from the client): half-width colon, one space, no space before %.
	if (percent) return base + ": \\+?" + re + "%";
	return base + u8"[:：]? *\\+?" + re + (op == RangeOp::Ge ? "" : "([^0-9]|$)");
}

bool IsTierNameLine(const std::string& line)
{
	// /（階級 *#）|\(Tier #\)/i
	static const std::optional<Rx> rx = RxCompile(u8"（階級 *#）|\\(Tier #\\)", nullptr);
	return rx && RxSearch(*rx, line);
}

std::optional<std::string> MapTierFragment(const AlgoValue& v, int digits, Lang lang, int hi)
{
	const RangeOp op = RangeOpOf(v);
	if (op == RangeOp::None) return std::nullopt;
	NumRange r = RangeFor(v, op);
	// R10: a >= condition on a tier with a known top ("≥15" of 1..16) is the closed
	// range up to it: "1[56]" instead of "(1[5-9]|[2-9][0-9])". A minimum above the
	// top (a stored value from outside the input's range) keeps the open form.
	if (op == RangeOp::Ge && hi > 0 && *v.min <= (double)hi) r.max = (double)hi;
	const std::string re = RegexNumeric::ReadableRangeRegex(r, digits);
	if (re.empty()) return std::nullopt;
	// frag.ts:72 TIER_NAME_FORMAT
	if (lang == Lang::Zh) return u8"階級 *" + re + u8"）";
	return "Tier " + re + "\\)";
}

std::optional<std::string> RarityFragment(const std::string& label, const std::string& value)
{
	const std::string base = LabelBase(label);
	const std::string val = JsTrim(value);
	if (base.empty() || val.empty()) return std::nullopt;
	return base + u8"[:：] *" + val;
}

std::optional<std::string> WholeLine(const std::vector<std::string>& labels)
{
	std::vector<std::string> xs;
	for (const std::string& l : labels) {
		std::string b = LabelBase(l);
		if (b.empty() || std::find(xs.begin(), xs.end(), b) != xs.end()) continue;
		xs.push_back(std::move(b));
	}
	if (xs.empty()) return std::nullopt;
	if (xs.size() == 1) return "^" + xs[0] + "$";
	std::string s = "^(";
	for (size_t i = 0; i < xs.size(); i++) {
		if (i) s += '|';
		s += xs[i];
	}
	return s + ")$";
}

std::optional<std::string> LinkedSockets(int n)
{
	if (n < 2 || n > 6) return std::nullopt;
	std::string s;
	for (int i = 0; i < n; i++) {
		if (i) s += '-';
		s += '.';
	}
	return s;
}

std::optional<std::string> LinkColors(const std::string& choice)
{
	std::string letters;
	for (char c : choice) {
		c = LowerAscii(c);
		if (IsColor(c)) letters += c;
	}
	if (letters.size() < 2 || letters.size() > 6) return std::nullopt;
	Trie root;
	for (const std::string& p : Permutations(letters)) {
		Trie* node = &root;
		for (size_t i = 0; i < p.size(); i++) {
			if (i) node = node->Child('-');
			node = node->Child(p[i]);
		}
	}
	return RenderTrie(root, true);
}

std::optional<std::string> SocketColorCount(const std::string& label, const std::string& color, int n)
{
	const std::string base = LabelBase(label);
	// 'rgbw'.includes(c) && c.length === 1 -- one ASCII colour letter, any case.
	if (base.empty() || color.size() != 1 || !IsColor(LowerAscii(color[0])) || n < 1 || n > 6)
		return std::nullopt;
	const char c = LowerAscii(color[0]);
	std::string s = base;
	for (int i = 0; i < n; i++) {
		s += ".*";
		s += c;
	}
	return s;
}

} // namespace RegexFrag
