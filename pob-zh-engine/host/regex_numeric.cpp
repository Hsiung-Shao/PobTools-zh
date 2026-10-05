#include "regex_numeric.h"

#include <cmath>
#include <cstdio>
#include <vector>

// Port of exile-appraiser regex/src/numeric.ts; see the header. Line numbers in
// the comments refer to that file.

namespace RegexNumeric {

namespace {

// numeric.ts:27 Atom: digit class lo..hi, opt = followed by '?'.
struct Atom {
	int lo, hi;
	bool opt;
	bool operator==(const Atom& o) const { return lo == o.lo && hi == o.hi && opt == o.opt; }
};
using Pattern = std::vector<Atom>;

Atom MkAtom(int lo, int hi) { return Atom{lo, hi, false}; }   // numeric.ts:48 atom
Atom MkAtom(int v) { return Atom{v, v, false}; }
Atom D() { return MkAtom(0, 9); }                              // numeric.ts:51 D

// JS Math.max / Math.min: NaN wins (std::max would silently drop it).
double JsMax(double a, double b) { return (std::isnan(a) || std::isnan(b)) ? NAN : (a > b ? a : b); }
double JsMin(double a, double b) { return (std::isnan(a) || std::isnan(b)) ? NAN : (a < b ? a : b); }

// String(n) for an integral double in the range we use.
std::string IntStr(double v)
{
	char buf[64];
	snprintf(buf, sizeof buf, "%.0f", v);
	std::string s = buf;
	if (s == "-0") s = "0";
	return s;
}

std::string IntStr(long long v) { return std::to_string(v); }

long long Pow10(int n)
{
	long long r = 1;
	while (n-- > 0) r *= 10;
	return r;
}

// numeric.ts:54 sameLength: equal-length decimal strings [a, b] -> patterns.
std::vector<Pattern> SameLength(const std::string& a, const std::string& b)
{
	const size_t n = a.size();
	if (n == 0) return {Pattern{}};
	const int a0 = a[0] - '0';
	const int b0 = b[0] - '0';
	const std::string ra = a.substr(1), rb = b.substr(1);
	auto prefixed = [](int d, std::vector<Pattern> ps) {
		for (Pattern& p : ps) p.insert(p.begin(), MkAtom(d));
		return ps;
	};
	if (a0 == b0) return prefixed(a0, SameLength(ra, rb));
	const std::string zeros(n - 1, '0'), nines(n - 1, '9');
	std::vector<Pattern> out;
	int lo = a0, hi = b0;
	if (ra != zeros) {
		for (Pattern& p : prefixed(a0, SameLength(ra, nines))) out.push_back(std::move(p));
		lo++;
	}
	std::vector<Pattern> tail;
	if (rb != nines) {
		tail = prefixed(b0, SameLength(zeros, rb));
		hi--;
	}
	if (lo <= hi) {
		Pattern p{MkAtom(lo, hi)};
		for (size_t i = 0; i + 1 < n; i++) p.push_back(D());
		out.push_back(std::move(p));
	}
	for (Pattern& p : tail) out.push_back(std::move(p));
	return out;
}

// numeric.ts:81 renderAtom
std::string RenderAtom(const Atom& x, const char* any)
{
	std::string s;
	if (x.lo == 0 && x.hi == 9) s = any;
	else if (x.lo == x.hi) s = std::to_string(x.lo);
	else if (x.hi == x.lo + 1) s = "[" + std::to_string(x.lo) + std::to_string(x.hi) + "]";
	else s = "[" + std::to_string(x.lo) + "-" + std::to_string(x.hi) + "]";
	return x.opt ? s + "?" : s;
}

// numeric.ts:90 renderPattern
std::string RenderPattern(const Pattern& p, const char* any)
{
	std::string s;
	for (const Atom& x : p) s += RenderAtom(x, any);
	return s;
}

// numeric.ts:103 mergeOptional (atomEq :94, patternEq :98): `P` and `X P` -> `X?P`, until nothing merges.
// The merged pattern takes the shorter one's place; the scan restarts from the
// top after every merge (the TS `break outer`).
std::vector<Pattern> MergeOptional(std::vector<Pattern> out)
{
	bool changed = true;
	while (changed) {
		changed = false;
		for (size_t i = 0; i < out.size() && !changed; i++) {
			for (size_t j = 0; j < out.size(); j++) {
				if (i == j) continue;
				const Pattern& p = out[i];
				const Pattern& q = out[j];
				if (q.size() != p.size() + 1 || q[0].opt) continue;
				if (!std::equal(q.begin() + 1, q.end(), p.begin())) continue;
				Pattern merged;
				merged.push_back(Atom{q[0].lo, q[0].hi, true});
				merged.insert(merged.end(), p.begin(), p.end());
				out[i] = std::move(merged);
				out.erase(out.begin() + (std::ptrdiff_t)j);
				changed = true;
				break;
			}
		}
	}
	return out;
}

// numeric.ts:128 joinAlternatives
std::string JoinAlternatives(const std::vector<std::string>& parts)
{
	if (parts.empty()) return "";
	if (parts.size() == 1) return parts[0];
	std::string s = "(";
	for (size_t i = 0; i < parts.size(); i++) {
		if (i) s += '|';
		s += parts[i];
	}
	return s + ")";
}

// numeric.ts:183 readableSpanPatterns
std::vector<Pattern> ReadableSpanPatterns(long long lo, long long hi)
{
	std::vector<Pattern> pats;
	const int maxLen = (int)IntStr(hi).size();
	for (int len = 1; len <= maxLen; len++) {
		const long long first = len == 1 ? 0 : Pow10(len - 1);
		const long long last = Pow10(len) - 1;
		const long long a = lo > first ? lo : first;
		const long long b = hi < last ? hi : last;
		if (a > b) continue;
		for (Pattern& p : SameLength(IntStr(a), IntStr(b))) pats.push_back(std::move(p));
	}
	return MergeOptional(std::move(pats));
}

const char* const kAnyShort = "\\d";
const char* const kAnyReadable = "[0-9]";   // numeric.ts:181 ANY

} // namespace

double DomainMax(int digits) { return std::pow(10.0, digits) - 1; }

bool NormalizeRange(const NumRange& r, int digits, long long& lo, long long& hi)
{
	const double top = DomainMax(digits);
	const double l = JsMax(0, std::ceil(r.min ? *r.min : 0));
	const double h = JsMin(top, std::floor(r.max ? *r.max : top));
	if (!std::isfinite(l) || !std::isfinite(h) || l > h) return false;
	lo = (long long)l;
	hi = (long long)h;
	return true;
}

std::string RangeRegex(const NumRange& r, int digits)
{
	long long lo, hi;
	if (!NormalizeRange(r, digits, lo, hi)) return "";
	std::vector<Pattern> pats;
	for (int len = 1; len <= digits; len++) {
		const long long first = len == 1 ? 0 : Pow10(len - 1);
		const long long last = Pow10(len) - 1;
		const long long a = lo > first ? lo : first;
		const long long b = hi < last ? hi : last;
		if (a > b) continue;
		for (Pattern& p : SameLength(IntStr(a), IntStr(b))) {
			// ① a leading [1-9] becomes \d (len >= 2): printed numbers have no
			// leading zero, so the two are the same under whole-number matching.
			if (len >= 2 && p[0].lo == 1 && p[0].hi == 9) p[0] = D();
			pats.push_back(std::move(p));
		}
	}
	std::vector<std::string> parts;
	for (const Pattern& p : MergeOptional(std::move(pats))) parts.push_back(RenderPattern(p, kAnyShort));
	const std::string built = JoinAlternatives(parts);
	// A narrow range across a digit boundary (8-10) is shorter listed out.
	const std::string naive = NaiveRangeRegex(r, digits);
	return naive.size() < built.size() ? naive : built;
}

std::string ReadableRangeRegex(const NumRange& r, int digits, bool open)
{
	const bool openTop = open && !r.max;
	if (!openTop) {
		long long lo, hi;
		if (!NormalizeRange(r, digits, lo, hi)) return "";
		std::vector<std::string> parts;
		for (const Pattern& p : ReadableSpanPatterns(lo, hi)) parts.push_back(RenderPattern(p, kAnyReadable));
		const std::string built = JoinAlternatives(parts);
		const std::string naive = NaiveRangeRegex(r, digits);
		return naive.size() < built.size() ? naive : built;
	}
	const double l = JsMax(0, std::ceil(r.min ? *r.min : 0));
	if (!std::isfinite(l) || l > 1e15) return "";   // beyond 1e15 String() would differ; never an input
	const long long lo = (long long)l;
	const int n = (int)IntStr(lo).size();
	const std::string tail = "[1-9]" + std::string(kAnyReadable) + "{" + std::to_string(n) + ",}";
	if (n >= 2 && lo == Pow10(n - 1))
		return "[1-9]" + std::string(kAnyReadable) + "{" + std::to_string(n - 1) + ",}";
	std::vector<std::string> parts;
	for (const Pattern& p : ReadableSpanPatterns(lo, Pow10(n) - 1)) parts.push_back(RenderPattern(p, kAnyReadable));
	parts.push_back(tail);
	return JoinAlternatives(parts);
}

std::string NaiveRangeRegex(const NumRange& r, int digits)
{
	long long lo, hi;
	if (!NormalizeRange(r, digits, lo, hi)) return "";
	std::vector<std::string> nums;
	for (long long n = lo; n <= hi; n++) nums.push_back(IntStr(n));
	return JoinAlternatives(nums);
}

} // namespace RegexNumeric
