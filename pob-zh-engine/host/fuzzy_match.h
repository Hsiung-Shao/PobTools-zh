// PobTools shared fuzzy matcher for the atlas planner's pickers.
//
// Extracted from atlas_scarabs when the astrolabe and map pickers needed the
// same behaviour. Plain substring is too strict for these names: Chinese puts
// the family word last (「聖甲蟲：窩巢裂痕」) while English puts it first
// ("Breach Scarab of the Hive"), so someone who knows one order cannot find the
// other, and the fullwidth colon defeats a query typed without it.
//
// Pure string logic, no ImGui / GL.
#pragma once

#include <string>
#include <vector>

// A query parsed once per frame rather than once per row.
struct FuzzyQuery {
	std::string lower;               // as typed, ASCII-lowercased and trimmed
	std::string compact;             // spaces and punctuation removed
	std::vector<std::string> tokens; // whitespace-split
	bool empty() const { return lower.empty(); }
};

FuzzyQuery MakeFuzzyQuery(const std::string& raw);

// Lowercase input with whitespace and the punctuation these names use removed.
// Build a row's compact key with this so both sides are folded the same way.
std::string FuzzyCompactKey(const std::string& s);

// How well one NAME field matches, best tier first:
//
//   name exact / prefix / substring        100 / 90 / 80 - position
//   substring ignoring punctuation         70
//   every whitespace token present         60
//   characters in order (fzf-like)         40
//
// `key` must already be lowercased and `compact` must be FuzzyCompactKey(key).
// Returns 0 for no match; an empty query is the caller's business, not this
// function's (the pickers give everything score 1 to keep the natural order).
int FuzzyNameScore(const std::string& key, const std::string& compact, const FuzzyQuery& q);

// How well one DESCRIPTION field matches: substring 30, all tokens present 20.
// Deliberately below every name tier so "what is this called" outranks "what
// does it do". `key` must already be lowercased.
int FuzzyTextScore(const std::string& key, const FuzzyQuery& q);
