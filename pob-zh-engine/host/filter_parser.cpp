#include "filter_parser.h"

#include <unordered_set>
#include <cstring>
#include <cstdlib>

namespace {

inline bool is_space(char c) { return c == ' ' || c == '\t'; }

const std::unordered_set<std::string>& headers()
{
	static const std::unordered_set<std::string> s = { "Show", "Hide", "Minimal" };
	return s;
}

// Recognised condition keywords. Misclassifying a rare/new condition as Unknown
// is harmless (Unknown lines round-trip verbatim and are read-only in Stage 1);
// this set just lets the UI render nicer condition summaries.
const std::unordered_set<std::string>& conditions()
{
	static const std::unordered_set<std::string> s = {
		"Class","BaseType","Rarity","ItemLevel","DropLevel","Quality","Sockets",
		"LinkedSockets","SocketGroup","StackSize","AreaLevel","Corrupted","Identified",
		"ShaperItem","ElderItem","FracturedItem","SynthesisedItem","HasInfluence",
		"HasExplicitMod","HasEnchantment","AnyEnchantment","EnchantmentPassiveNode",
		"EnchantmentPassiveNum","GemLevel","MapTier","Height","Width","Replica",
		"Scourged","Mirrored","ElderMap","ShapedMap","BlightedMap","UberBlightedMap",
		"HasSearingExarchImplicit","HasEaterOfWorldsImplicit","ArchnemesisMod",
		"GemQualityType","BaseDefencePercentile","BaseArmour","BaseEvasion",
		"BaseEnergyShield","BaseWard","CorruptedMods","DefencePercentile",
		"HasImplicitMod","TransfiguredGem","AlternateQuality","Prophecy","WaystoneTier",
		"MemoryStrands","ItemRarity","Continue",
	};
	return s;
}

// Recognised action keywords. This set MUST be accurate: it drives action-line
// detection (and the per-block action cache) in the editor.
const std::unordered_set<std::string>& actions()
{
	static const std::unordered_set<std::string> s = {
		"SetTextColor","SetBorderColor","SetBackgroundColor","SetFontSize",
		"PlayAlertSound","PlayAlertSoundPositional","CustomAlertSound",
		"CustomAlertSoundOptional","DisableDropSound","EnableDropSound",
		"DisableDropSoundIfAlertSound","MinimapIcon","PlayEffect",
	};
	return s;
}

// Comparison operators, longest first so ">=" is not split into ">" and "!=" not
// into "!".
const char* const kOps[] = { ">=", "<=", "==", "!=", "=", "!", "<", ">" };

// Split a string into whitespace-separated tokens, treating "..." as one quoted
// token (the surrounding quotes are not stored in text; quoted is set true).
std::vector<FilterToken> tokenize(const std::string& s)
{
	std::vector<FilterToken> out;
	size_t i = 0, n = s.size();
	while (i < n) {
		while (i < n && is_space(s[i])) i++;
		if (i >= n) break;
		FilterToken t;
		if (s[i] == '"') {
			i++;
			size_t start = i;
			while (i < n && s[i] != '"') i++;
			t.text = s.substr(start, i - start);
			t.quoted = true;
			if (i < n) i++; // consume closing quote
		} else {
			size_t start = i;
			while (i < n && !is_space(s[i])) i++;
			t.text = s.substr(start, i - start);
			t.quoted = false;
		}
		out.push_back(std::move(t));
	}
	return out;
}

} // namespace

FilterLine ParseFilterLine(const std::string& raw)
{
	FilterLine line;
	line.raw = raw;

	size_t i = 0;
	while (i < raw.size() && is_space(raw[i])) i++;
	line.indent = raw.substr(0, i);
	std::string code = raw.substr(i);

	if (code.empty()) { line.kind = FilterLineKind::Blank; return line; }
	if (code[0] == '#') { line.kind = FilterLineKind::Comment; return line; }

	// Trailing comment: first '#' outside quotes, including the whitespace gap.
	bool inq = false;
	size_t hash = std::string::npos;
	for (size_t k = 0; k < code.size(); k++) {
		char c = code[k];
		if (c == '"') inq = !inq;
		else if (c == '#' && !inq) { hash = k; break; }
	}
	std::string codePart = code;
	if (hash != std::string::npos) {
		size_t start = hash;
		while (start > 0 && is_space(code[start - 1])) start--;
		line.trailingComment = code.substr(start);
		codePart = code.substr(0, start);
	}

	// keyword = first token of codePart.
	size_t k = 0;
	while (k < codePart.size() && !is_space(codePart[k])) k++;
	line.keyword = codePart.substr(0, k);
	std::string rest = codePart.substr(k);
	size_t r = 0;
	while (r < rest.size() && is_space(rest[r])) r++;
	std::string restTrim = rest.substr(r);

	if (headers().count(line.keyword)) {
		line.kind = FilterLineKind::BlockHeader;
	} else {
		bool isCond = conditions().count(line.keyword) > 0;
		bool isAct = actions().count(line.keyword) > 0;
		std::string afterOp = restTrim;
		for (const char* op : kOps) {
			size_t ol = std::strlen(op);
			if (restTrim.compare(0, ol, op) == 0) {
				line.op = op;
				afterOp = restTrim.substr(ol);
				break;
			}
		}
		line.values = tokenize(afterOp);
		line.kind = isAct ? FilterLineKind::Action
		          : (isCond ? FilterLineKind::Condition : FilterLineKind::Unknown);
	}
	return line;
}

std::string FilterSerializeLine(const FilterLine& ln)
{
	if (!ln.dirty) return ln.raw;
	std::string out = ln.indent;
	out += ln.keyword;
	if (!ln.op.empty()) { out += ' '; out += ln.op; }
	for (const FilterToken& t : ln.values) {
		out += ' ';
		if (t.quoted) { out += '"'; out += t.text; out += '"'; }
		else out += t.text;
	}
	out += ln.trailingComment;
	return out;
}

void RebuildFilterBlocks(FilterFile& f)
{
	f.blocks.clear();

	// Group lines into blocks (header .. next header). Leading lines before the
	// first header belong to no block but still serialize in order.
	int curBlock = -1;
	for (int idx = 0; idx < (int)f.lines.size(); idx++) {
		const FilterLine& ln = f.lines[idx];
		if (ln.kind == FilterLineKind::BlockHeader) {
			FilterBlock b;
			b.headerLineIdx = idx;
			b.hide = (ln.keyword == "Hide");
			b.lineIdx.push_back(idx);
			const std::string& c = ln.trailingComment;
			size_t p = 0;
			while (p < c.size() && (is_space(c[p]) || c[p] == '#')) p++;
			b.headerComment = c.substr(p);
			f.blocks.push_back(std::move(b));
			curBlock = (int)f.blocks.size() - 1;
		} else if (curBlock >= 0) {
			f.blocks[curBlock].lineIdx.push_back(idx);
		}
	}

	// Cache each block's action-line indices (first occurrence wins).
	for (FilterBlock& b : f.blocks) {
		for (int li : b.lineIdx) {
			const FilterLine& ln = f.lines[li];
			if (ln.kind != FilterLineKind::Action) continue;
			const std::string& kw = ln.keyword;
			auto first = [&](int& slot) { if (slot < 0) slot = li; };
			if (kw == "SetTextColor") first(b.idxTextColor);
			else if (kw == "SetBorderColor") first(b.idxBorderColor);
			else if (kw == "SetBackgroundColor") first(b.idxBgColor);
			else if (kw == "SetFontSize") first(b.idxFontSize);
			else if (kw == "PlayAlertSound" || kw == "PlayAlertSoundPositional") first(b.idxAlertSound);
			else if (kw == "CustomAlertSound" || kw == "CustomAlertSoundOptional") first(b.idxCustomSound);
			else if (kw == "DisableDropSound" || kw == "EnableDropSound" || kw == "DisableDropSoundIfAlertSound") first(b.idxDisableDropSound);
			else if (kw == "MinimapIcon") first(b.idxMinimapIcon);
			else if (kw == "PlayEffect") first(b.idxPlayEffect);
		}
	}
}

bool FilterIsKnownCondition(const std::string& keyword) { return conditions().count(keyword) > 0; }
bool FilterIsKnownAction(const std::string& keyword) { return actions().count(keyword) > 0; }

FilterFile ParseFilter(const std::string& content0)
{
	FilterFile f;
	std::string content = content0;

	// UTF-8 BOM.
	if (content.size() >= 3 && (unsigned char)content[0] == 0xEF &&
		(unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF) {
		f.hadBom = true;
		content.erase(0, 3);
	}

	// Line ending + trailing-newline detection (from the first '\n').
	size_t firstNl = content.find('\n');
	if (firstNl == std::string::npos) {
		f.crlf = true;            // default (POE writes CRLF); single line w/o newline
		f.finalNewline = false;
	} else {
		f.crlf = (firstNl > 0 && content[firstNl - 1] == '\r');
		f.finalNewline = !content.empty() && content.back() == '\n';
	}

	// Split on '\n'; a trailing '\n' does not create a phantom empty last line.
	std::vector<std::string> rawLines;
	{
		size_t pos = 0;
		for (;;) {
			size_t e = content.find('\n', pos);
			if (e == std::string::npos) {
				std::string tail = content.substr(pos);
				if (!(f.finalNewline && tail.empty())) rawLines.push_back(tail);
				break;
			}
			rawLines.push_back(content.substr(pos, e - pos));
			pos = e + 1;
		}
	}
	for (std::string& ln : rawLines)
		if (!ln.empty() && ln.back() == '\r') ln.pop_back();

	// Classify each line, then group into blocks.
	for (const std::string& raw : rawLines)
		f.lines.push_back(ParseFilterLine(raw));
	RebuildFilterBlocks(f);

	return f;
}

std::string SerializeFilter(const FilterFile& f)
{
	std::string out;
	const char* nlSeq = f.crlf ? "\r\n" : "\n";
	if (f.hadBom) out += "\xEF\xBB\xBF";

	for (size_t i = 0; i < f.lines.size(); i++) {
		out += FilterSerializeLine(f.lines[i]);
		bool last = (i + 1 == f.lines.size());
		if (!last || f.finalNewline) out += nlSeq;
	}
	return out;
}

// ---- value accessors -------------------------------------------------------

int FilterValueInt(const FilterLine& line, size_t idx, int def)
{
	if (idx >= line.values.size()) return def;
	const std::string& s = line.values[idx].text;
	if (s.empty()) return def;
	char* end = nullptr;
	long v = std::strtol(s.c_str(), &end, 10);
	if (end == s.c_str()) return def;
	return (int)v;
}

void FilterSetValueInt(FilterLine& line, size_t idx, int v)
{
	while (line.values.size() <= idx) line.values.push_back(FilterToken{});
	line.values[idx].text = std::to_string(v);
	line.values[idx].quoted = false;
	line.dirty = true;
}

void FilterSetValueStr(FilterLine& line, size_t idx, const std::string& s, bool quoted)
{
	while (line.values.size() <= idx) line.values.push_back(FilterToken{});
	line.values[idx].text = s;
	line.values[idx].quoted = quoted;
	line.dirty = true;
}

void FilterGetColor(const FilterLine& line, int& r, int& g, int& b, int& a, bool& hasAlpha)
{
	r = FilterValueInt(line, 0, 0);
	g = FilterValueInt(line, 1, 0);
	b = FilterValueInt(line, 2, 0);
	hasAlpha = line.values.size() >= 4;
	a = hasAlpha ? FilterValueInt(line, 3, 255) : 255;
}

void FilterSetColor(FilterLine& line, int r, int g, int b, int a, bool hasAlpha)
{
	auto clamp255 = [](int x) { return x < 0 ? 0 : (x > 255 ? 255 : x); };
	FilterSetValueInt(line, 0, clamp255(r));
	FilterSetValueInt(line, 1, clamp255(g));
	FilterSetValueInt(line, 2, clamp255(b));
	if (hasAlpha) {
		FilterSetValueInt(line, 3, clamp255(a));
	} else if (line.values.size() > 3) {
		line.values.resize(3);
		line.dirty = true;
	}
}

bool FilterHasValue(const FilterLine& line, const std::string& text)
{
	for (const FilterToken& t : line.values)
		if (t.text == text) return true;
	return false;
}

size_t FilterAddValue(FilterLine& line, const std::string& text, bool quoted)
{
	FilterToken t;
	t.text = text;
	t.quoted = quoted;
	line.values.push_back(std::move(t));
	line.dirty = true;
	return line.values.size() - 1;
}

bool FilterRemoveValue(FilterLine& line, const std::string& text)
{
	for (size_t i = 0; i < line.values.size(); i++) {
		if (line.values[i].text == text) {
			line.values.erase(line.values.begin() + i);
			line.dirty = true;
			return true;
		}
	}
	return false;
}
