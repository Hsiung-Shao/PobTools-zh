#include "regex_folders.h"

#include <algorithm>

namespace RegexFolders {

namespace {

// JS WhiteSpace + LineTerminator (what both String.prototype.trim and /\s/ match).
bool IsJsSpace(char32_t c)
{
	return c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20 || c == 0xA0 ||
	       c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
	       c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

// UTF-8 -> code points (with the byte span of each), invalid bytes as themselves.
struct Cp {
	char32_t c;
	size_t at, len;
};
std::vector<Cp> Decode(const std::string& s)
{
	std::vector<Cp> out;
	size_t i = 0;
	while (i < s.size()) {
		const unsigned char b = (unsigned char)s[i];
		size_t len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
		if (i + len > s.size()) len = 1;
		char32_t c = len == 1 ? b : len == 2 ? (b & 0x1F) : len == 3 ? (b & 0x0F) : (b & 0x07);
		for (size_t k = 1; k < len; k++) c = (c << 6) | ((unsigned char)s[i + k] & 0x3F);
		out.push_back({c, i, len});
		i += len;
	}
	return out;
}

int FolderIndex(const RegexUiState& s, const std::string& game, const std::string& name)
{
	const std::vector<RegexBookmarkFolder>& list = s.Folders(game);
	for (int i = 0; i < (int)list.size(); i++)
		if (list[i].name == name) return i;
	return -1;
}

// folderRank: position in the list; uncategorised / unknown = the list length.
int Rank(const RegexUiState& s, const std::string& game, const std::string& folder)
{
	const int n = (int)s.Folders(game).size();
	if (folder.empty()) return n;
	const int i = FolderIndex(s, game, folder);
	return i < 0 ? n : i;
}

std::vector<int> SlotsOf(const RegexUiState& s, const std::string& game)
{
	std::vector<int> slots;
	for (int i = 0; i < (int)s.bookmarks.size(); i++)
		if (s.bookmarks[i].game == game) slots.push_back(i);
	return slots;
}

} // namespace

std::string NormalizeName(const std::string& name)
{
	const std::vector<Cp> cps = Decode(name);
	size_t a = 0, b = cps.size();
	while (a < b && IsJsSpace(cps[a].c)) a++;
	while (b > a && IsJsSpace(cps[b - 1].c)) b--;
	std::string out;
	int count = 0;
	bool inSpace = false;
	for (size_t i = a; i < b && count < kNameMax; i++) {
		if (IsJsSpace(cps[i].c)) {
			if (inSpace) continue;
			inSpace = true;
			out += ' ';
			count++;
			continue;
		}
		inSpace = false;
		out.append(name, cps[i].at, cps[i].len);
		count++;
	}
	return out;
}

const char* ResultId(Result r)
{
	switch (r) {
	case Result::Ok: return "ok";
	case Result::Empty: return "empty";
	case Result::Duplicate: return "duplicate";
	case Result::Missing: return "missing";
	}
	return "?";
}

bool SortBookmarks(RegexUiState& s, const std::string& game, int* track)
{
	const std::vector<int> slots = SlotsOf(s, game);
	struct Item {
		int orig, order, rank;
	};
	std::vector<Item> items;
	for (int k = 0; k < (int)slots.size(); k++)
		items.push_back({slots[k], k, Rank(s, game, s.bookmarks[slots[k]].folder)});
	std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) {
		return x.rank != y.rank ? x.rank < y.rank : x.order < y.order;
	});
	bool changed = false;
	for (int k = 0; k < (int)items.size(); k++)
		if (items[k].orig != slots[k]) changed = true;
	if (!changed) return false;
	std::vector<RegexBookmark> moved;
	moved.reserve(items.size());
	for (const Item& it : items) moved.push_back(s.bookmarks[it.orig]);
	int newTrack = track ? *track : -1;
	for (int k = 0; k < (int)items.size(); k++) {
		s.bookmarks[slots[k]] = std::move(moved[k]);
		if (track && items[k].orig == *track) newTrack = slots[k];
	}
	if (track) *track = newTrack;
	return true;
}

bool Normalize(RegexUiState& s)
{
	bool changed = false;
	for (const char* game : {"poe1", "poe2"}) {
		std::vector<std::string> seen;
		std::vector<RegexBookmarkFolder> list;
		for (const RegexBookmarkFolder& f : s.Folders(game)) {
			const std::string name = NormalizeName(f.name);
			if (name.empty() || std::find(seen.begin(), seen.end(), name) != seen.end()) {
				changed = true;
				continue;
			}
			if (name != f.name) changed = true;
			seen.push_back(name);
			list.push_back({name, f.collapsed});
		}
		for (RegexBookmark& b : s.bookmarks) {
			if (b.game != game) continue;
			const std::string name = NormalizeName(b.folder);
			if (name != b.folder) {
				changed = true;
				b.folder = name;
			}
			if (!name.empty() && std::find(seen.begin(), seen.end(), name) == seen.end()) {
				seen.push_back(name);
				list.push_back({name, false});
				changed = true;
			}
		}
		s.Folders(game) = std::move(list);
		if (SortBookmarks(s, game)) changed = true;
	}
	return changed;
}

Result Add(RegexUiState& s, const std::string& game, const std::string& name)
{
	const std::string n = NormalizeName(name);
	if (n.empty()) return Result::Empty;
	if (FolderIndex(s, game, n) >= 0) return Result::Duplicate;
	s.Folders(game).push_back({n, false});
	return Result::Ok;
}

Result Rename(RegexUiState& s, const std::string& game, const std::string& from, const std::string& to)
{
	const std::string n = NormalizeName(to);
	if (n.empty()) return Result::Empty;
	const int i = FolderIndex(s, game, from);
	if (i < 0) return Result::Missing;
	if (n == from) return Result::Ok;
	if (FolderIndex(s, game, n) >= 0) return Result::Duplicate;
	s.Folders(game)[i].name = n;
	for (RegexBookmark& b : s.bookmarks)
		if (b.game == game && b.folder == from) b.folder = n;
	return Result::Ok;
}

int Delete(RegexUiState& s, const std::string& game, const std::string& name)
{
	const int i = FolderIndex(s, game, name);
	if (i < 0) return -1;
	std::vector<RegexBookmarkFolder>& list = s.Folders(game);
	list.erase(std::remove_if(list.begin(), list.end(),
	                          [&](const RegexBookmarkFolder& f) { return f.name == name; }),
	           list.end());
	int n = 0;
	for (RegexBookmark& b : s.bookmarks)
		if (b.game == game && b.folder == name) {
			b.folder.clear();
			n++;
		}
	SortBookmarks(s, game);
	return n;
}

bool MoveTo(RegexUiState& s, const std::string& game, const std::string& name, int to)
{
	std::vector<RegexBookmarkFolder>& list = s.Folders(game);
	const int i = FolderIndex(s, game, name);
	if (i < 0) return false;
	const int j = std::max(0, std::min((int)list.size() - 1, to));
	if (i == j) return false;
	const RegexBookmarkFolder f = list[i];
	list.erase(list.begin() + i);
	list.insert(list.begin() + j, f);
	SortBookmarks(s, game);
	return true;
}

bool MoveBy(RegexUiState& s, const std::string& game, const std::string& name, int delta)
{
	const int i = FolderIndex(s, game, name);
	if (i < 0) return false;
	return MoveTo(s, game, name, i + delta);
}

bool IsCollapsed(const RegexUiState& s, const std::string& game, const std::string& folder)
{
	if (folder.empty())
		return std::find(s.uncatCollapsed.begin(), s.uncatCollapsed.end(), game) != s.uncatCollapsed.end();
	const int i = FolderIndex(s, game, folder);
	return i >= 0 && s.Folders(game)[i].collapsed;
}

bool SetCollapsed(RegexUiState& s, const std::string& game, const std::string& folder, bool on)
{
	if (IsCollapsed(s, game, folder) == on) return false;
	if (folder.empty()) {
		if (on) s.uncatCollapsed.push_back(game);
		else s.uncatCollapsed.erase(std::remove(s.uncatCollapsed.begin(), s.uncatCollapsed.end(), game),
		                            s.uncatCollapsed.end());
		return true;
	}
	const int i = FolderIndex(s, game, folder);
	if (i < 0) return false;
	s.Folders(game)[i].collapsed = on;
	return true;
}

int MoveBookmark(RegexUiState& s, int index, const std::string& folder, int before)
{
	if (index < 0 || index >= (int)s.bookmarks.size()) return -1;
	const std::string game = s.bookmarks[index].game;
	if (game.empty()) return -1;
	std::string target = folder;
	const bool hasBefore = before >= 0;
	if (hasBefore) {
		if (before >= (int)s.bookmarks.size() || s.bookmarks[before].game != game) return -1;
		target = s.bookmarks[before].folder;
	}
	if (!target.empty() && FolderIndex(s, game, target) < 0) return -1;
	if (hasBefore && before == index) return index;
	// Only the slots this game occupies are reordered: the other game's and the
	// orphans' indices do not change.
	const std::vector<int> slots = SlotsOf(s, game);
	std::vector<int> seq;
	for (int i : slots)
		if (i != index) seq.push_back(i);
	s.bookmarks[index].folder = target;
	if (hasBefore) seq.insert(std::find(seq.begin(), seq.end(), before), index);
	else seq.push_back(index);
	std::vector<RegexBookmark> vals;
	vals.reserve(seq.size());
	for (int i : seq) vals.push_back(s.bookmarks[i]);
	int pos = -1;
	for (int k = 0; k < (int)seq.size(); k++) {
		s.bookmarks[slots[k]] = std::move(vals[k]);
		if (seq[k] == index) pos = slots[k];
	}
	SortBookmarks(s, game, &pos);
	return pos;
}

int MoveBookmarkBy(RegexUiState& s, int index, int delta)
{
	if (index < 0 || index >= (int)s.bookmarks.size()) return index;
	const RegexBookmark& b = s.bookmarks[index];
	if (b.game.empty() || delta == 0) return index;
	std::vector<int> peers;
	for (int i = 0; i < (int)s.bookmarks.size(); i++)
		if (s.bookmarks[i].game == b.game && s.bookmarks[i].folder == b.folder) peers.push_back(i);
	const int k = (int)(std::find(peers.begin(), peers.end(), index) - peers.begin());
	const int j = k + (delta < 0 ? -1 : 1);
	if (j < 0 || j >= (int)peers.size()) return index;
	const int other = peers[j];
	std::swap(s.bookmarks[index], s.bookmarks[other]);
	return other;
}

Grouped GroupBookmarks(const RegexUiState& s, const std::string& game, bool skipEmpty)
{
	const std::vector<RegexBookmarkFolder>& list = s.Folders(game);
	std::vector<Group> groups;
	for (const RegexBookmarkFolder& f : list) groups.push_back({f.name, f.collapsed, {}});
	Group uncat{std::string(), IsCollapsed(s, game, std::string()), {}};
	for (int i = 0; i < (int)s.bookmarks.size(); i++) {
		const RegexBookmark& b = s.bookmarks[i];
		if (b.game != game) continue;
		Group* g = &uncat;
		if (!b.folder.empty())
			for (Group& x : groups)
				if (x.folder == b.folder) {
					g = &x;
					break;
				}
		g->items.push_back(i);
	}
	Grouped out;
	for (Group& g : groups)
		if (!skipEmpty || !g.items.empty()) out.groups.push_back(std::move(g));
	if (!uncat.items.empty() || !skipEmpty) out.groups.push_back(std::move(uncat));
	for (const Group& g : out.groups)
		if (!g.folder.empty()) out.headers = true;
	if (!out.headers)
		for (Group& g : out.groups) g.collapsed = false;
	return out;
}

std::map<std::string, int> Counts(const RegexUiState& s, const std::string& game)
{
	std::map<std::string, int> m;
	for (const RegexBookmark& b : s.bookmarks)
		if (b.game == game) m[b.folder]++;
	return m;
}

} // namespace RegexFolders
