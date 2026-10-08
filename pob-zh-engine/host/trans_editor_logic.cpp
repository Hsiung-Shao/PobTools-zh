#include "trans_editor_logic.h"

#include <algorithm>
#include <map>

namespace TransEd {

// ---- add-entry notice ---------------------------------------------------------

std::string TrimKey(const std::string& key)
{
	size_t b = 0, e = key.size();
	auto ideoSpaceAt = [&](size_t i) {
		return i + 2 < key.size() && (unsigned char)key[i] == 0xE3 && (unsigned char)key[i + 1] == 0x80 &&
		       (unsigned char)key[i + 2] == 0x80;
	};
	for (;;) {
		if (b < e && (key[b] == ' ' || key[b] == '\t' || key[b] == '\r' || key[b] == '\n')) { b++; continue; }
		if (b + 3 <= e && ideoSpaceAt(b)) { b += 3; continue; }
		break;
	}
	for (;;) {
		if (e > b && (key[e - 1] == ' ' || key[e - 1] == '\t' || key[e - 1] == '\r' || key[e - 1] == '\n')) { e--; continue; }
		if (e >= b + 3 && ideoSpaceAt(e - 3)) { e -= 3; continue; }
		break;
	}
	return key.substr(b, e - b);
}

AddHint ClassifyAdd(const EditorModel& model, const std::string& key, int target)
{
	AddHint h;
	if (key.empty()) return h;
	h.owners = FilesContaining(model, key);
	if (TrimKey(key) != key) {
		h.kind = AddKind::Whitespace;
		return h;
	}
	if (h.owners.empty()) {
		h.kind = AddKind::New;
		return h;
	}
	const int winner = WinnerFileIdx(model, key);
	const bool inTarget = std::find(h.owners.begin(), h.owners.end(), target) != h.owners.end();
	const int tOrder = (target >= 0 && target < (int)model.files.size()) ? model.files[target].order : -1;
	if (winner >= 0 && winner != target && model.files[winner].order > tOrder) {
		h.kind = AddKind::Shadowed;
		h.winner = winner;
		return h;
	}
	if (inTarget) {
		h.kind = AddKind::OverwriteSame;
		const long long at = FindEntry(model, target, key);
		if (at >= 0) h.oldValue = model.entries[(size_t)at].value;
		return h;
	}
	h.kind = AddKind::TargetWins;
	return h;
}

// ---- undo -------------------------------------------------------------------

EntryState StateOf(const EditorModel& model, int fileIdx, const std::string& key)
{
	EntryState s;
	const long long at = FindEntry(model, fileIdx, key);
	if (at >= 0) {
		s.exists = true;
		s.value = model.entries[(size_t)at].value;
	}
	return s;
}

bool ApplyState(EditorModel& model, int fileIdx, const std::string& key, const EntryState& s)
{
	const long long at = FindEntry(model, fileIdx, key);
	if (s.exists) {
		if (at >= 0) {
			model.entries[(size_t)at].value = s.value;
			RefreshEdited(model, (size_t)at);
			return false;
		}
		SetEntry(model, fileIdx, key, s.value);
		return true;
	}
	if (at < 0) return false;
	// Only an entry added in this session can be "not there" in an earlier state:
	// the editor never deletes what is on disk.
	return RemoveAddedEntry(model, (size_t)at);
}

void UndoStack::Push(UndoStep step)
{
	if (step.Empty()) return;
	steps_.push_back(std::move(step));
	if (steps_.size() > kMax) steps_.erase(steps_.begin());
}

bool UndoStack::Undo(EditorModel& model, std::vector<char>* missFilled, bool* reshaped)
{
	if (steps_.empty()) return false;
	UndoStep s = std::move(steps_.back());
	steps_.pop_back();
	bool shape = false;
	// newest change first, so a step that touched one key twice ends at its
	// earliest "before"
	for (auto it = s.changes.rbegin(); it != s.changes.rend(); ++it)
		if (ApplyState(model, it->fileIdx, it->key, it->before)) shape = true;
	if (missFilled)
		for (const MissMark& m : s.marks)
			if (m.missIdx >= 0 && m.missIdx < (int)missFilled->size()) (*missFilled)[(size_t)m.missIdx] = m.before ? 1 : 0;
	if (reshaped) *reshaped = shape;
	return true;
}

bool SetWithUndo(EditorModel& model, UndoStack& undo, int fileIdx, const std::string& key, const std::string& value)
{
	EntryChange c;
	c.fileIdx = fileIdx;
	c.key = key;
	c.before = StateOf(model, fileIdx, key);
	c.after.exists = true;
	c.after.value = value;
	if (c.before == c.after) return false;
	const bool shape = ApplyState(model, fileIdx, key, c.after);
	UndoStep s;
	s.changes.push_back(std::move(c));
	undo.Push(std::move(s));
	return shape;
}

// ---- missing strings ----------------------------------------------------------

namespace {
bool fillable(const std::vector<MissEntry>& misses, const std::vector<MissRow>& rows, int i)
{
	if (i < 0 || i >= (int)misses.size() || i >= (int)rows.size()) return false;
	const MissRow& r = rows[(size_t)i];
	return !misses[(size_t)i].reverse && !r.filled && r.target >= 0 && !r.trans.empty();
}

// One row's fill into an existing step.
bool fillInto(EditorModel& model, const std::vector<MissEntry>& misses, std::vector<MissRow>& rows, int i,
              UndoStep& step)
{
	MissRow& r = rows[(size_t)i];
	EntryChange c;
	c.fileIdx = r.target;
	c.key = misses[(size_t)i].text;
	c.before = StateOf(model, r.target, c.key);
	c.after.exists = true;
	c.after.value = r.trans;
	const bool shape = ApplyState(model, c.fileIdx, c.key, c.after);
	r.prev = c.before;
	r.filled = true;
	r.filledFile = r.target;
	step.changes.push_back(std::move(c));
	step.marks.push_back(MissMark{ i, false, true });
	return shape;
}
} // namespace

bool FillMiss(EditorModel& model, UndoStack& undo, const std::vector<MissEntry>& misses, std::vector<MissRow>& rows,
              int i, bool* reshaped)
{
	if (reshaped) *reshaped = false;
	if (!fillable(misses, rows, i)) return false;
	UndoStep step;
	const bool shape = fillInto(model, misses, rows, i, step);
	undo.Push(std::move(step));
	if (reshaped) *reshaped = shape;
	return true;
}

int FillAllMisses(EditorModel& model, UndoStack& undo, const std::vector<MissEntry>& misses,
                  std::vector<MissRow>& rows, bool* reshaped)
{
	if (reshaped) *reshaped = false;
	UndoStep step;
	int n = 0;
	bool shape = false;
	for (int i = 0; i < (int)misses.size() && i < (int)rows.size(); i++) {
		if (!fillable(misses, rows, i)) continue;
		if (fillInto(model, misses, rows, i, step)) shape = true;
		n++;
	}
	undo.Push(std::move(step));
	if (reshaped) *reshaped = shape;
	return n;
}

bool WithdrawMiss(EditorModel& model, UndoStack& undo, const std::vector<MissEntry>& misses,
                  std::vector<MissRow>& rows, int i, bool* reshaped)
{
	if (reshaped) *reshaped = false;
	if (i < 0 || i >= (int)misses.size() || i >= (int)rows.size() || !rows[(size_t)i].filled) return false;
	MissRow& r = rows[(size_t)i];
	EntryChange c;
	c.fileIdx = r.filledFile;
	c.key = misses[(size_t)i].text;
	c.before = StateOf(model, c.fileIdx, c.key);
	c.after = r.prev;
	const bool shape = ApplyState(model, c.fileIdx, c.key, c.after);
	r.filled = false;
	UndoStep step;
	step.changes.push_back(std::move(c));
	step.marks.push_back(MissMark{ i, true, false });
	undo.Push(std::move(step));
	if (reshaped) *reshaped = shape;
	return true;
}

bool UndoInto(EditorModel& model, UndoStack& undo, std::vector<MissRow>& rows, bool* reshaped)
{
	std::vector<char> marks(rows.size());
	for (size_t i = 0; i < rows.size(); i++) marks[i] = rows[i].filled ? 1 : 0;
	if (!undo.Undo(model, &marks, reshaped)) return false;
	for (size_t i = 0; i < rows.size(); i++) rows[i].filled = marks[i] != 0;
	return true;
}

// ---- multi-line editor checks -------------------------------------------------

namespace {
bool hexDigit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// Same rules as the editor's preview (engine/common/common.cpp IsColorEscape).
int escLen(const std::string& s, size_t i)
{
	if (i + 1 >= s.size() || s[i] != '^') return 0;
	const char d = s[i + 1];
	if (d >= '0' && d <= '9') return 2;
	if ((d == 'x' || d == 'X') && i + 7 < s.size()) {
		for (int k = 0; k < 6; k++)
			if (!hexDigit(s[i + 2 + (size_t)k])) return 0;
		return 8;
	}
	return 0;
}

// {N} and {N:fmt}: the N of each, in order of appearance.
std::vector<int> placeholderIds(const std::string& s)
{
	std::vector<int> ids;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] != '{') continue;
		size_t j = i + 1;
		int n = 0, digits = 0;
		while (j < s.size() && s[j] >= '0' && s[j] <= '9' && digits < 4) { n = n * 10 + (s[j] - '0'); j++; digits++; }
		if (!digits || j >= s.size()) continue;
		if (s[j] == '}') { ids.push_back(n); i = j; continue; }
		if (s[j] == ':') {
			const size_t close = s.find('}', j);
			if (close != std::string::npos && close - j < 16) { ids.push_back(n); i = close; }
		}
	}
	return ids;
}
} // namespace

std::vector<std::string> ColourEscapes(const std::string& s)
{
	std::vector<std::string> out;
	for (size_t i = 0; i < s.size();) {
		const int n = escLen(s, i);
		if (!n) { i++; continue; }
		std::string e = s.substr(i, (size_t)n);
		for (char& c : e) if (c >= 'a' && c <= 'f') c = (char)(c - 32);   // case is not a difference
		if (e.size() > 1 && e[1] == 'X') e[1] = 'x';
		out.push_back(std::move(e));
		i += (size_t)n;
	}
	return out;
}

int LineCount(const std::string& s)
{
	int n = 1;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '\n') n++;
		else if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { n++; i++; }
	}
	return n;
}

TextChecks CheckTranslation(const std::string& key, const std::string& value)
{
	TextChecks c;
	{
		std::vector<std::string> k = ColourEscapes(key), v = ColourEscapes(value);
		c.colourKey = (int)k.size();
		c.colourValue = (int)v.size();
		if (!k.empty()) {
			std::map<std::string, int> have;
			for (const std::string& e : v) have[e]++;
			for (const std::string& e : k) {
				auto it = have.find(e);
				if (it != have.end() && it->second > 0) it->second--;
				else c.colourMissing.push_back(e);
			}
			c.colourOk = c.colourMissing.empty();
		}
	}
	c.linesKey = LineCount(key);
	c.linesValue = LineCount(value);
	c.linesOk = c.linesKey == c.linesValue;
	{
		std::vector<int> k = placeholderIds(key), v = placeholderIds(value);
		std::map<int, int> kc, vc;
		for (int n : k) kc[n]++;
		for (int n : v) vc[n]++;
		for (const auto& kv : kc) {
			c.placeholdersKey.push_back("{" + std::to_string(kv.first) + "}");
			const int have = vc.count(kv.first) ? vc[kv.first] : 0;
			// a placeholder may legitimately appear twice in one language and once
			// in the other; what breaks POB is one that is not there at all
			if (have == 0) c.placeholdersMissing.push_back("{" + std::to_string(kv.first) + "}");
		}
		for (const auto& kv : vc)
			if (!kc.count(kv.first)) c.placeholdersExtra.push_back("{" + std::to_string(kv.first) + "}");
		c.placeholdersOk = c.placeholdersMissing.empty() && c.placeholdersExtra.empty();
	}
	return c;
}

int CountModified(const EditorModel& model)
{
	return DirtyEntryCount(model);
}

} // namespace TransEd
