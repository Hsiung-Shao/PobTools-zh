#include "filter_doc_editor.h"
#include "filter_parser.h"

#include <algorithm>
#include <cstdlib>

namespace {

inline bool is_space(char c) { return c == ' ' || c == '\t'; }

// A line that carries filter syntax (would confuse the game if left orphaned
// when its header is disabled).
inline bool is_syntax(FilterLineKind k)
{
	return k == FilterLineKind::BlockHeader || k == FilterLineKind::Condition ||
	       k == FilterLineKind::Action || k == FilterLineKind::Unknown;
}

// In-place comment-out shared by CommentOutLine / CommentOutBlock: the line
// becomes "<indent>#! <content>" with raw as its only source of truth.
void comment_out(FilterLine& ln)
{
	std::string body = FilterSerializeLine(ln);
	body.erase(0, ln.indent.size());
	FilterLine repl;
	repl.kind = FilterLineKind::Comment;
	repl.indent = ln.indent;
	repl.raw = ln.indent + "#! " + body;
	repl.uid = ln.uid;   // the same line, disabled: it keeps its identity
	ln = std::move(repl);
}

} // namespace

int FilterDocumentEditor::FindLine(int blockIdx, const std::string& keyword) const
{
	if (!f_ || blockIdx < 0 || blockIdx >= (int)f_->blocks.size()) return -1;
	for (int li : f_->blocks[blockIdx].lineIdx) {
		const FilterLine& ln = f_->lines[li];
		if ((ln.kind == FilterLineKind::Condition || ln.kind == FilterLineKind::Action) &&
		    ln.keyword == keyword)
			return li;
	}
	return -1;
}

bool FilterDocumentEditor::IsDisabledLine(int lineIdx, FilterLine* parsedOut) const
{
	if (!f_ || lineIdx < 0 || lineIdx >= (int)f_->lines.size()) return false;
	const FilterLine& ln = f_->lines[lineIdx];
	if (ln.kind != FilterLineKind::Comment) return false;
	std::string code = ln.raw.substr(ln.indent.size());
	if (code.compare(0, 2, "#!") != 0) return false;
	size_t p = 2;
	while (p < code.size() && is_space(code[p])) p++;
	std::string body = code.substr(p);
	if (body.empty()) return false;
	FilterLine parsed = ParseFilterLine(ln.indent + body);
	if (parsed.kind != FilterLineKind::Condition && parsed.kind != FilterLineKind::Action &&
	    parsed.kind != FilterLineKind::BlockHeader)
		return false;
	if (parsedOut) *parsedOut = std::move(parsed);
	return true;
}

int FilterDocumentEditor::InsertLine(int blockIdx, const std::string& keyword,
                                     const std::string& op, const std::vector<FilterToken>& values)
{
	if (!f_ || blockIdx < 0 || blockIdx >= (int)f_->blocks.size()) return -1;
	const FilterBlock& b = f_->blocks[blockIdx];

	bool isCond = FilterIsKnownCondition(keyword);
	int lastCond = -1, lastAct = -1, lastSyntax = b.headerLineIdx;
	for (int li : b.lineIdx) {
		FilterLineKind k = f_->lines[li].kind;
		if (k == FilterLineKind::Condition) lastCond = li;
		else if (k == FilterLineKind::Action) lastAct = li;
		if (is_syntax(k)) lastSyntax = li;
	}
	// Conditions group after the last condition; actions after the last action,
	// falling back to the end of the block's syntax so they land below conditions.
	int after;
	if (isCond) after = (lastCond >= 0) ? lastCond : b.headerLineIdx;
	else        after = (lastAct >= 0) ? lastAct : lastSyntax;

	// Indent: copy from the neighbour we insert after (or the first body line),
	// defaulting to four spaces beside a bare header.
	std::string indent = "    ";
	if (after != b.headerLineIdx) indent = f_->lines[after].indent;
	else
		for (int li : b.lineIdx)
			if (li != b.headerLineIdx && is_syntax(f_->lines[li].kind)) { indent = f_->lines[li].indent; break; }

	FilterLine ln;
	ln.kind = isCond ? FilterLineKind::Condition : FilterLineKind::Action;
	ln.indent = indent;
	ln.keyword = keyword;
	ln.op = op;
	ln.values = values;
	ln.dirty = true;

	int at = after + 1;
	f_->lines.insert(f_->lines.begin() + at, std::move(ln));
	f_->dirty = true;
	RebuildBlocks();
	return at;
}

void FilterDocumentEditor::CommentOutLine(int lineIdx)
{
	if (!f_ || lineIdx < 0 || lineIdx >= (int)f_->lines.size()) return;
	FilterLine& ln = f_->lines[lineIdx];
	if (!is_syntax(ln.kind)) return;
	comment_out(ln);
	f_->dirty = true;
	RebuildBlocks();
}

bool FilterDocumentEditor::RestoreLine(int lineIdx)
{
	FilterLine parsed;
	if (!IsDisabledLine(lineIdx, &parsed)) return false;
	parsed.uid = f_->lines[lineIdx].uid;
	f_->lines[lineIdx] = std::move(parsed);
	f_->dirty = true;
	RebuildBlocks();
	return true;
}

void FilterDocumentEditor::RemoveLine(int lineIdx)
{
	if (!f_ || lineIdx < 0 || lineIdx >= (int)f_->lines.size()) return;
	f_->lines.erase(f_->lines.begin() + lineIdx);
	f_->dirty = true;
	RebuildBlocks();
}

int FilterDocumentEditor::CreateBlockAtLine(int atLine, bool hide, const std::string& headerComment)
{
	if (!f_) return -1;
	if (atLine < 0 || atLine > (int)f_->lines.size()) atLine = (int)f_->lines.size();

	FilterLine header;
	header.kind = FilterLineKind::BlockHeader;
	header.keyword = hide ? "Hide" : "Show";
	if (!headerComment.empty()) header.trailingComment = " # " + headerComment;
	header.dirty = true;

	FilterLine blank;
	blank.kind = FilterLineKind::Blank;

	f_->lines.insert(f_->lines.begin() + atLine, { std::move(header), std::move(blank) });
	f_->dirty = true;
	RebuildBlocks();

	for (int i = 0; i < (int)f_->blocks.size(); i++)
		if (f_->blocks[i].headerLineIdx == atLine) return i;
	return -1;
}

int FilterDocumentEditor::CreateBlock(int insertBeforeBlockIdx, bool hide, const std::string& headerComment)
{
	if (!f_) return -1;
	int atLine = (int)f_->lines.size();
	if (insertBeforeBlockIdx >= 0 && insertBeforeBlockIdx < (int)f_->blocks.size())
		atLine = f_->blocks[insertBeforeBlockIdx].headerLineIdx;
	return CreateBlockAtLine(atLine, hide, headerComment);
}

int FilterDocumentEditor::DuplicateBlock(int blockIdx)
{
	if (!f_ || blockIdx < 0 || blockIdx >= (int)f_->blocks.size()) return -1;
	const FilterBlock& b = f_->blocks[blockIdx];
	int first = b.lineIdx.front(), last = b.lineIdx.back();
	std::vector<FilterLine> copy(f_->lines.begin() + first, f_->lines.begin() + last + 1);
	for (FilterLine& ln : copy) ln.uid = 0;   // a copy is a new block (fresh uids on rebuild)
	int at = last + 1;
	f_->lines.insert(f_->lines.begin() + at, copy.begin(), copy.end());
	f_->dirty = true;
	RebuildBlocks();

	for (int i = 0; i < (int)f_->blocks.size(); i++)
		if (f_->blocks[i].headerLineIdx == at) return i;
	return -1;
}

void FilterDocumentEditor::CommentOutBlock(int blockIdx)
{
	if (!f_ || blockIdx < 0 || blockIdx >= (int)f_->blocks.size()) return;
	// Every syntax line is disabled together (a live condition under a disabled
	// header would be an orphan-line error when the game loads the filter).
	for (int li : f_->blocks[blockIdx].lineIdx) {
		FilterLine& ln = f_->lines[li];
		if (is_syntax(ln.kind)) comment_out(ln);
	}
	f_->dirty = true;
	RebuildBlocks();
}

void FilterDocumentEditor::RemoveBlock(int blockIdx)
{
	if (!f_ || blockIdx < 0 || blockIdx >= (int)f_->blocks.size()) return;
	const FilterBlock& b = f_->blocks[blockIdx];
	f_->lines.erase(f_->lines.begin() + b.lineIdx.front(),
	                f_->lines.begin() + b.lineIdx.back() + 1);
	f_->dirty = true;
	RebuildBlocks();
}

BlockAnchor FilterDocumentEditor::CaptureAnchor(int blockIdx) const
{
	BlockAnchor a;
	if (!f_ || blockIdx < 0 || blockIdx >= (int)f_->blocks.size()) return a;
	const FilterBlock& b = f_->blocks[blockIdx];
	a.headerLineIdx = b.headerLineIdx;
	a.headerRaw = FilterSerializeLine(f_->lines[b.headerLineIdx]);
	for (int li : b.lineIdx)
		if (f_->lines[li].kind == FilterLineKind::Condition) {
			a.firstCondRaw = FilterSerializeLine(f_->lines[li]);
			break;
		}
	return a;
}

int FilterDocumentEditor::ResolveAnchor(const BlockAnchor& a) const
{
	if (!f_ || !a.valid()) return -1;

	// Fast path: the header line has not moved.
	if (a.headerLineIdx < (int)f_->lines.size() &&
	    FilterSerializeLine(f_->lines[a.headerLineIdx]) == a.headerRaw)
		for (int i = 0; i < (int)f_->blocks.size(); i++)
			if (f_->blocks[i].headerLineIdx == a.headerLineIdx) return i;

	// Scan: same header text, tie-broken by first condition line, then by
	// proximity to the captured position.
	int best = -1;
	long bestDist = 0;
	for (int i = 0; i < (int)f_->blocks.size(); i++) {
		const FilterBlock& b = f_->blocks[i];
		if (FilterSerializeLine(f_->lines[b.headerLineIdx]) != a.headerRaw) continue;
		if (!a.firstCondRaw.empty()) {
			std::string cond;
			for (int li : b.lineIdx)
				if (f_->lines[li].kind == FilterLineKind::Condition) {
					cond = FilterSerializeLine(f_->lines[li]);
					break;
				}
			if (cond != a.firstCondRaw) continue;
		}
		long dist = std::labs((long)b.headerLineIdx - (long)a.headerLineIdx);
		if (best < 0 || dist < bestDist) { best = i; bestDist = dist; }
	}
	return best;
}

void FilterDocumentEditor::BeginBatch() { batchDepth_++; }

void FilterDocumentEditor::EndBatch()
{
	if (batchDepth_ > 0 && --batchDepth_ == 0 && pendingRebuild_) {
		pendingRebuild_ = false;
		RebuildBlocks();
	}
}

void FilterDocumentEditor::RebuildBlocks()
{
	if (!f_) return;
	if (batchDepth_ > 0) { pendingRebuild_ = true; return; }
	RebuildFilterBlocks(*f_);
	AssignUids();
	version_++;
}

// ---- baseline -----------------------------------------------------------------

namespace {

// A line that belongs to its block: syntax, or a "#!" line this editor disabled.
bool OwnLine(const FilterLine& ln)
{
	if (is_syntax(ln.kind)) return true;
	if (ln.kind != FilterLineKind::Comment) return false;
	return ln.raw.compare(ln.indent.size(), 2, "#!") == 0;
}

bool SameText(const FilterLine& cur, const FilterLine& base)
{
	// base is a clean copy (raw == its serialization); a clean current line
	// serializes to its raw, so only a dirty one needs rebuilding.
	if (!cur.dirty) return cur.raw == base.raw;
	return FilterSerializeLine(cur) == base.raw;
}

} // namespace

void FilterDocumentEditor::AssignUids()
{
	if (!f_) return;
	for (FilterLine& ln : f_->lines)
		if (ln.uid == 0) ln.uid = nextUid_++;
}

int FilterDocumentEditor::BodyEnd(int blockIdx) const
{
	const FilterBlock& b = f_->blocks[blockIdx];
	int end = b.headerLineIdx;
	for (int li : b.lineIdx)
		if (OwnLine(f_->lines[li])) end = (std::max)(end, li);
	return end;
}

void FilterDocumentEditor::ClearBaseline()
{
	settleCheckedAt_ = ~0u;
	hasBaseline_ = false;
	base_.clear();
	baseLine_.clear();
	baseText_.clear();
}

void FilterDocumentEditor::CaptureBaseline()
{
	ClearBaseline();
	if (!f_) return;
	AssignUids();
	for (int bi = 0; bi < (int)f_->blocks.size(); bi++) {
		const FilterBlock& b = f_->blocks[bi];
		BaseBlock& bb = base_[f_->lines[b.headerLineIdx].uid];
		const int end = BodyEnd(bi);
		for (int li = b.headerLineIdx; li <= end; li++) {
			FilterLine c = f_->lines[li];
			c.raw = FilterSerializeLine(c);
			c.dirty = false;
			bb.body.push_back(std::move(c));
		}
	}
	// pointers into the vectors, which no longer change
	for (auto& kv : base_)
		for (const FilterLine& ln : kv.second.body) baseLine_[ln.uid] = &ln;
	baseText_ = SerializeFilter(*f_);
	hasBaseline_ = true;
}

BlockChange FilterDocumentEditor::BlockState(int blockIdx) const
{
	if (!hasBaseline_ || !f_ || blockIdx < 0 || blockIdx >= (int)f_->blocks.size()) return BlockChange::Same;
	const FilterBlock& b = f_->blocks[blockIdx];
	auto it = base_.find(f_->lines[b.headerLineIdx].uid);
	if (it == base_.end()) return BlockChange::Added;
	const std::vector<FilterLine>& body = it->second.body;
	const int end = BodyEnd(blockIdx);
	if (end - b.headerLineIdx + 1 != (int)body.size()) return BlockChange::Modified;
	for (int i = 0; i < (int)body.size(); i++)
		if (!SameText(f_->lines[b.headerLineIdx + i], body[i])) return BlockChange::Modified;
	return BlockChange::Same;
}

const FilterLine* FilterDocumentEditor::BaselineLine(int lineIdx) const
{
	if (!hasBaseline_ || !f_ || lineIdx < 0 || lineIdx >= (int)f_->lines.size()) return nullptr;
	auto it = baseLine_.find(f_->lines[lineIdx].uid);
	return it == baseLine_.end() ? nullptr : it->second;
}

bool FilterDocumentEditor::LineChanged(int lineIdx) const
{
	if (!hasBaseline_) return false;
	const FilterLine* b = BaselineLine(lineIdx);
	if (!b) return true;
	return !SameText(f_->lines[lineIdx], *b);
}

int FilterDocumentEditor::RestoreBlock(int blockIdx)
{
	if (!hasBaseline_ || !f_ || blockIdx < 0 || blockIdx >= (int)f_->blocks.size()) return -1;
	const int h = f_->blocks[blockIdx].headerLineIdx;
	auto it = base_.find(f_->lines[h].uid);
	if (it == base_.end()) return -1;
	const int end = BodyEnd(blockIdx);
	f_->lines.erase(f_->lines.begin() + h, f_->lines.begin() + end + 1);
	f_->lines.insert(f_->lines.begin() + h, it->second.body.begin(), it->second.body.end());
	f_->dirty = true;
	RebuildBlocks();
	if (MatchesBaseline()) f_->dirty = false;
	for (int i = 0; i < (int)f_->blocks.size(); i++)
		if (f_->blocks[i].headerLineIdx == h) return i;
	return -1;
}

int FilterDocumentEditor::RemovedBaselineBlocks() const
{
	if (!hasBaseline_ || !f_) return 0;
	int present = 0;
	for (const FilterBlock& b : f_->blocks)
		if (base_.count(f_->lines[b.headerLineIdx].uid)) present++;
	return (int)base_.size() - present;
}

bool FilterDocumentEditor::MatchesBaseline() const
{
	return hasBaseline_ && f_ && SerializeFilter(*f_) == baseText_;
}

int FilterDocumentEditor::UnsavedBlockCount(bool settle)
{
	if (!f_ || !f_->dirty) return 0;
	if (!hasBaseline_) return 1;
	int n = 0;
	for (int i = 0; i < (int)f_->blocks.size(); i++)
		if (BlockState(i) != BlockChange::Same) n++;
	n += RemovedBaselineBlocks();
	if (n == 0) {
		// every block is back to what the file says; something outside the
		// blocks may still differ (the custom zone's markers)
		// Only lines between blocks can differ now, and those change only through
		// structural edits: one full comparison per structure version is enough.
		if (settle) {
			if (settleCheckedAt_ != version_) {
				settleCheckedAt_ = version_;
				settleMatched_ = MatchesBaseline();
			}
			if (settleMatched_) { f_->dirty = false; return 0; }
		}
		return 1;
	}
	return n;
}
