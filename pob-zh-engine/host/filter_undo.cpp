#include "filter_undo.h"

bool FilterLineSame(const FilterLine& a, const FilterLine& b)
{
	if (a.uid != b.uid || a.kind != b.kind || a.dirty != b.dirty) return false;
	if (!a.dirty) return a.raw == b.raw;
	if (a.keyword != b.keyword || a.op != b.op || a.indent != b.indent || a.trailingComment != b.trailingComment ||
	    a.values.size() != b.values.size())
		return false;
	for (size_t i = 0; i < a.values.size(); i++)
		if (a.values[i].text != b.values[i].text || a.values[i].quoted != b.values[i].quoted) return false;
	return true;
}

namespace {

// Replace lines [start, start + oldLen) with `with`.
void Splice(std::vector<FilterLine>& lines, size_t start, size_t oldLen, const std::vector<FilterLine>& with)
{
	lines.erase(lines.begin() + start, lines.begin() + start + oldLen);
	lines.insert(lines.begin() + start, with.begin(), with.end());
}

} // namespace

void FilterUndoHistory::Reset(const std::vector<FilterLine>& lines)
{
	shadow_ = lines;
	steps_.clear();
	pos_ = 0;
}

bool FilterUndoHistory::Capture(const std::vector<FilterLine>& lines)
{
	const size_t n0 = shadow_.size(), n1 = lines.size();
	size_t pre = 0;
	while (pre < n0 && pre < n1 && FilterLineSame(shadow_[pre], lines[pre])) pre++;
	if (pre == n0 && pre == n1) return false;
	size_t suf = 0;
	while (suf < n0 - pre && suf < n1 - pre && FilterLineSame(shadow_[n0 - 1 - suf], lines[n1 - 1 - suf])) suf++;

	Step st;
	st.start = pre;
	st.before.assign(shadow_.begin() + pre, shadow_.begin() + (n0 - suf));
	st.after.assign(lines.begin() + pre, lines.begin() + (n1 - suf));
	Splice(shadow_, pre, st.before.size(), st.after);

	steps_.resize(pos_);   // a new edit drops the redo side
	steps_.push_back(std::move(st));
	if (steps_.size() > kMaxSteps) steps_.erase(steps_.begin());
	pos_ = steps_.size();
	return true;
}

int FilterUndoHistory::Undo(std::vector<FilterLine>& lines)
{
	Capture(lines);   // an edit not captured yet is a step of its own
	if (pos_ == 0) return -1;
	const Step& st = steps_[--pos_];
	Splice(lines, st.start, st.after.size(), st.before);
	Splice(shadow_, st.start, st.after.size(), st.before);
	return (int)st.start;
}

int FilterUndoHistory::Redo(std::vector<FilterLine>& lines)
{
	if (Capture(lines)) return -1;   // a fresh edit: nothing to redo any more
	if (pos_ >= steps_.size()) return -1;
	const Step& st = steps_[pos_++];
	Splice(lines, st.start, st.before.size(), st.after);
	Splice(shadow_, st.start, st.before.size(), st.after);
	return (int)st.start;
}
