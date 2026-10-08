// A small backtracking regex matcher with JavaScript RegExp semantics, for the
// fragments the Poe Regex tool builds (numeric ranges, property lines, socket
// links) and for checking them against corpus lines.
//
// Why not std::regex: MSVC's implementation is pathologically slow on many
// short strings (agent-data memory error_msvc_std_regex_slow.md), and the
// self-test alone runs tens of millions of searches. Why not RE2: the host links
// the static CRT and carries no such dependency. And the reference we have to
// agree with is exile-appraiser's `new RegExp(src, 'i').test(line)`, so the
// semantics here follow ECMAScript (non-unicode mode, Annex B leniencies), not
// any other dialect.
//
// Supported syntax
//   literals          any character; UTF-8 text is matched per code point
//   .                 any code point except \n \r U+2028 U+2029 (dotAll: any)
//   [...] [^...]      ranges, escapes inside (\d \s \w \D \S \W \] \\ \- \b=U+0008 ...)
//   quantifiers       ? * + {n} {n,} {n,m}, each optionally lazy with a trailing ?
//   groups            (...) (?:...) (?<name>...) and lookahead (?=...) (?!...)
//   alternation       |
//   anchors           ^ $ (whole input; no multiline flag) \b \B
//   escapes           \d \D \s \S \w \W \t \n \v \f \r \0 \xHH \uHHHH \cX and any
//                     other escaped character as itself (\+ \) \. \- ...)
//   Annex B           a '{' that does not start a valid quantifier, and a lone
//                     ']' or '}', are literals -- as in a browser.
// Rejected (error)
//   unbalanced ( ) / unterminated [ / trailing \ / nothing to repeat / {m,n}
//   out of order / [z-a] / lookbehind / backreferences \1-\9 / invalid UTF-8.
// Known differences from JS
//   * case folding (icase) is ES Canonicalize (non-unicode: toUpperCase per BMP
//     unit) from a table generated with node 24 / Unicode 16
//     (regex_case_table.inc); a different JS engine's Unicode version could
//     differ for a handful of recently added letters;
//   * matching is per code point, JS non-unicode mode per UTF-16 unit: only an
//     astral character (outside the BMP) can see a difference with '.' or [^x];
//   * '$' is end of input; JS without the m flag is the same.
//
// Runaway protection: a search gives up after `stepLimit` node visits or a
// recursion depth of `depthLimit`, and says so (RxStatus::Aborted) instead of
// guessing a result. Fragments the tool builds use a few dozen steps.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct RxFlags {
	bool icase = true;     // the game's search bar ignores case; so does `new RegExp(.., 'i')`
	bool dotAll = false;   // JS 's' flag
};

class Rx {
public:
	struct Node;
	struct Range { char32_t lo, hi; };
	std::vector<Node> nodes;
	std::vector<std::vector<Range>> classes;   // class ranges, by Node::cls
	int root = -1;
	int groups = 0;
	RxFlags flags;
	bool anchoredStart = false;   // pattern can only match at position 0
};

struct Rx::Node {
	enum Kind : uint8_t {
		Empty, Char, Any, Class, Begin, End, WordB, NotWordB, Seq, Alt, Repeat, Look, NegLook
	} kind = Empty;
	bool negated = false;    // Class
	bool greedy = true;      // Repeat
	char32_t ch = 0;         // Char (already folded when icase)
	int cls = -1;            // Class: index into Rx::classes
	int min = 0, max = -1;   // Repeat; max -1 = unbounded
	std::vector<int> kids;   // Seq / Alt / Repeat(1) / Look(1)
};

enum class RxStatus { NoMatch, Match, Aborted };

struct RxLimits {
	uint64_t stepLimit = 2000000;
	int depthLimit = 1000;   // nested M() calls; keeps a group repeat off the 1 MB main-thread stack
};

// Compile; nullopt with *err set on a syntax error (the message names the
// position in code points).
std::optional<Rx> RxCompile(std::string_view pattern, std::string* err, RxFlags flags = {});

// RegExp.prototype.test: is there a match anywhere in `text` (UTF-8)?
// Aborted counts as false; use RxSearchEx to tell the two apart.
bool RxSearch(const Rx& rx, std::string_view text);
RxStatus RxSearchEx(const Rx& rx, std::string_view text, const RxLimits& lim = {},
                    uint64_t* steps = nullptr);

// Same, on text already decoded to code points (the self-test decodes once and
// searches many times).
RxStatus RxSearchCps(const Rx& rx, const std::u32string& text, const RxLimits& lim = {},
                     uint64_t* steps = nullptr);

// ES Canonicalize as used for icase (exposed for the self-test).
char32_t RxCanonicalize(char32_t c);

// UTF-8 -> code points. Invalid sequences decode to U+FFFD each byte; returns
// false if any were found.
bool RxDecodeUtf8(std::string_view s, std::u32string& out);
