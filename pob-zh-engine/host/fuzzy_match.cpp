#include "fuzzy_match.h"

#include "atlas_stat_agg.h" // ToLowerAscii

#include <cstring> // strlen

// Punctuation that shows up in these names and would otherwise make a query
// typed without it fail. Listed as UTF-8 literals because they are multi-byte;
// a byte-wise filter would corrupt the surrounding CJK.
static const char* kDropTokens[] = {
	u8"：", u8"，", u8"。", u8"、", u8"（", u8"）", u8"「", u8"」", u8"·", u8"…",
	":", ",", ".", "'", "\"", "(", ")", "[", "]", "-", "/",
};

std::string FuzzyCompactKey(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	size_t i = 0;
	while (i < s.size()) {
		bool dropped = false;
		for (const char* tok : kDropTokens) {
			size_t n = strlen(tok);
			if (s.compare(i, n, tok) == 0) { i += n; dropped = true; break; }
		}
		if (dropped) continue;
		unsigned char c = (unsigned char)s[i];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
		out.push_back(s[i++]);
	}
	return out;
}

// Byte length of the UTF-8 code point starting at s[i].
static size_t utf8_len(const std::string& s, size_t i)
{
	unsigned char c = (unsigned char)s[i];
	size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
	return (i + n <= s.size()) ? n : 1;
}

// Do `needle`'s characters appear in `hay` in order (not necessarily adjacent)?
// Steps whole code points, never bytes: matching half of a CJK character
// against half of another would invent hits that make no sense to a reader.
static bool subsequence(const std::string& hay, const std::string& needle)
{
	size_t pos = 0;
	for (size_t i = 0; i < needle.size();) {
		size_t n = utf8_len(needle, i);
		size_t at = hay.find(needle.substr(i, n), pos);
		if (at == std::string::npos) return false;
		pos = at + n;
		i += n;
	}
	return true;
}

static bool all_tokens_in(const std::string& hay, const std::vector<std::string>& tokens)
{
	for (const std::string& t : tokens)
		if (hay.find(t) == std::string::npos) return false;
	return true;
}

FuzzyQuery MakeFuzzyQuery(const std::string& raw)
{
	FuzzyQuery q;
	q.lower = ToLowerAscii(raw);
	// trim so a trailing space does not turn into an empty token
	size_t b = q.lower.find_first_not_of(" \t\r\n");
	size_t e = q.lower.find_last_not_of(" \t\r\n");
	q.lower = (b == std::string::npos) ? std::string() : q.lower.substr(b, e - b + 1);
	q.compact = FuzzyCompactKey(q.lower);
	for (size_t i = 0; i <= q.lower.size();) {
		size_t sp = q.lower.find_first_of(" \t", i);
		std::string tok = q.lower.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
		if (!tok.empty()) q.tokens.push_back(tok);
		if (sp == std::string::npos) break;
		i = sp + 1;
	}
	return q;
}

int FuzzyNameScore(const std::string& key, const std::string& compact, const FuzzyQuery& q)
{
	if (key.empty() || q.empty()) return 0;
	if (key == q.lower) return 100;
	if (key.compare(0, q.lower.size(), q.lower) == 0) return 90;
	size_t at = key.find(q.lower);
	if (at != std::string::npos)
		return 80 - (int)(at < 9 ? at : 9);   // earlier hits read as more relevant
	if (!q.compact.empty() && compact.find(q.compact) != std::string::npos) return 70;
	if (q.tokens.size() > 1 && all_tokens_in(key, q.tokens)) return 60;
	if (!q.compact.empty() && subsequence(compact, q.compact)) return 40;
	return 0;
}

int FuzzyTextScore(const std::string& key, const FuzzyQuery& q)
{
	if (key.empty() || q.empty()) return 0;
	if (key.find(q.lower) != std::string::npos) return 30;
	if (q.tokens.size() > 1 && all_tokens_in(key, q.tokens)) return 20;
	return 0;
}
