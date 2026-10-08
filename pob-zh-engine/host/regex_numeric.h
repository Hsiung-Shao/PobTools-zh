// Number range -> search-bar regex fragment. Ported from exile-appraiser
// `regex/src/numeric.ts` (self-written there too; nothing from poe.re, which has
// no LICENSE). Each function names the TS file:line it mirrors; the output must
// be byte-identical, which the golden fixture in regex_selftest.cpp checks.
//
//   RangeRegex({16, -}, 2)              -> "(1[6-9]|[2-9]\d)"
//   RangeRegex({-, 5}, 1)               -> "[0-5]"
//   ReadableRangeRegex({30, -}, 3, open) -> "([3-9][0-9]|[1-9][0-9]{2,})"
//
// A fragment matches, as a whole, one decimal integer written without leading
// zeros (String(N)) within 0 .. 10^digits - 1 and min <= N <= max. Delimiters
// around it (%, $, a non-digit) are the caller's job.
#pragma once

#include <optional>
#include <string>

namespace RegexNumeric {

// numeric.ts:16 NumRange. Missing bound = std::nullopt (JS undefined).
struct NumRange {
	std::optional<double> min;
	std::optional<double> max;
};

// numeric.ts:35 domainMax
double DomainMax(int digits);

// numeric.ts:40 normalizeRange: integer closed interval; false when empty.
bool NormalizeRange(const NumRange& r, int digits, long long& lo, long long& hi);

// numeric.ts:138 rangeRegex: shortest (heuristic) fragment; "" for an empty set.
// When it contains '|' it is already wrapped in (...).
std::string RangeRegex(const NumRange& r, int digits);

// numeric.ts:198 readableRangeRegex: [0-9] instead of \d, leading [1-9] kept;
// `open` with no max = no upper bound ([1-9][0-9]{n,}).
std::string ReadableRangeRegex(const NumRange& r, int digits, bool open = false);

// numeric.ts:216 naiveRangeRegex: every number listed (tests compare lengths).
std::string NaiveRangeRegex(const NumRange& r, int digits);

} // namespace RegexNumeric
