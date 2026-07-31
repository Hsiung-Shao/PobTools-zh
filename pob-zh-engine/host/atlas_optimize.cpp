#include "atlas_optimize.h"

#include "atlas_persist.h" // sandbox contract check serializes a project

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX          // std::max/std::min are used throughout the solver
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <random>
#include <utility>

namespace {

const int kInf = 1 << 29;

// Exact-solver ceiling. The DP is O(3^k * n) in the merge step alone, so each
// extra target roughly triples the work; this is the last size that still
// solves inside a hover preview. Measured on the real 3.29 tree (901 nodes /
// 1001 edges) by --atlas-opt-selftest -- do not raise it without re-running it.
const int kExactTargetCap = 8;

// Deterministic restart count for the shortest-path heuristic. Each restart is
// a few BFS sweeps, so this is cheap next to the quality it buys.
const int kSphRestarts = 40;

// Hub candidates tried by the star construction (see star_candidates).
const int kStarHubs = 24;

// Re-insertion improvement passes, and how many of the ranked candidates get
// that (comparatively expensive) treatment. Measured: running it on more than
// the single best candidate cost time and bought no points.
const int kImprovePasses = 2;
const int kImproveCandidates = 1;

// ---- file helper (same convention as atlas_tree_data.cpp) ------------------

bool write_file_utf8(const std::wstring& path, const std::string& content)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD wrote = 0;
	bool ok = content.empty() ||
	          (WriteFile(h, content.data(), (DWORD)content.size(), &wrote, nullptr) && wrote == content.size());
	CloseHandle(h);
	return ok;
}

// ---- small graph utilities --------------------------------------------------

int count_set(const std::vector<char>& in)
{
	int c = 0;
	for (char v : in) c += v ? 1 : 0;
	return c;
}

std::vector<int> to_list(const std::vector<char>& in)
{
	std::vector<int> out;
	for (int i = 0; i < (int)in.size(); i++)
		if (in[i]) out.push_back(i);
	return out;
}

// Is every node of `in` reachable from root walking only through `in`?
// (`in` is always kept connected, so this doubles as "nothing was orphaned".)
bool induced_connected(const AtlasTreeData& d, const std::vector<char>& in)
{
	const int n = (int)d.nodes.size();
	if (d.root < 0 || d.root >= n || !in[d.root]) return false;
	std::vector<char> seen(n, 0);
	std::vector<int> stack{ d.root };
	seen[d.root] = 1;
	int count = 1;
	while (!stack.empty()) {
		int cur = stack.back(); stack.pop_back();
		for (int nb : d.nodes[cur].adj) {
			if (seen[nb] || !in[nb]) continue;
			seen[nb] = 1;
			count++;
			stack.push_back(nb);
		}
	}
	return count == count_set(in);
}

// Blocked nodes are excluded HERE, in the three primitives that walk edges
// (bfs / path_into / dw_solve's relaxation). Every construction in this file
// reaches the graph through one of them, so a node the user ruled out can never
// end up on a path. The only other way into a solution is `warmStart`, which
// AtlasPlanMinimal filters explicitly, and the selftest asserts the resulting
// post-condition rather than trusting this reasoning.
using Mask = std::vector<char>;

// BFS over the whole graph from `sources`; fills dist (-1 = unreachable) and
// prev. Used for terminal-to-everything distances and for path reconstruction.
void bfs(const AtlasTreeData& d, const Mask& blocked, const std::vector<int>& sources,
         std::vector<int>& dist, std::vector<int>& prev)
{
	const int n = (int)d.nodes.size();
	dist.assign(n, -1);
	prev.assign(n, -1);
	std::deque<int> q;
	for (int s : sources)
		if (s >= 0 && s < n && !blocked[s] && dist[s] < 0) { dist[s] = 0; q.push_back(s); }
	while (!q.empty()) {
		int cur = q.front(); q.pop_front();
		for (int nb : d.nodes[cur].adj) {
			if (dist[nb] >= 0 || blocked[nb]) continue;
			dist[nb] = dist[cur] + 1;
			prev[nb] = cur;
			q.push_back(nb);
		}
	}
}

// Shortest path from the `in` set to `target`, as the nodes that would have to
// be added (ordered towards the target, target last). Returns false only when
// the target cannot be reached at all.
bool path_into(const AtlasTreeData& d, const Mask& blocked, const std::vector<char>& in,
               int target, std::vector<int>* add)
{
	add->clear();
	const int n = (int)d.nodes.size();
	if (target < 0 || target >= n || blocked[target]) return false;
	if (in[target]) return true;

	std::vector<int> prev(n, -1);
	std::vector<char> seen(n, 0);
	std::deque<int> q;
	for (int i = 0; i < n; i++)
		if (in[i]) { seen[i] = 1; q.push_back(i); }
	while (!q.empty()) {
		int cur = q.front(); q.pop_front();
		for (int nb : d.nodes[cur].adj) {
			if (seen[nb] || blocked[nb]) continue;
			seen[nb] = 1;
			prev[nb] = cur;
			if (nb == target) {
				for (int at = target; at != -1 && !in[at]; at = prev[at])
					add->push_back(at);
				std::reverse(add->begin(), add->end());
				return true;
			}
			q.push_back(nb);
		}
	}
	return false;
}

// Cheap pass: peel non-pinned nodes that hang off the set by a single edge.
// Linear in the set size, so it runs on every candidate.
void prune_leaves(const AtlasTreeData& d, std::vector<char>& in, const std::vector<char>& pinned)
{
	const int n = (int)d.nodes.size();
	std::vector<int> deg(n, 0);
	std::vector<int> members = to_list(in);
	for (int v : members)
		for (int nb : d.nodes[v].adj)
			if (in[nb]) deg[v]++;

	std::vector<int> q;
	for (int v : members)
		if (!pinned[v] && v != d.root && deg[v] <= 1) q.push_back(v);
	while (!q.empty()) {
		int v = q.back(); q.pop_back();
		if (!in[v]) continue;
		in[v] = 0;
		for (int nb : d.nodes[v].adj) {
			if (!in[nb]) continue;
			if (--deg[nb] <= 1 && !pinned[nb] && nb != d.root) q.push_back(nb);
		}
	}
}

// Full pass: after leaf peeling, anything still removable sits on a cycle.
// Only those survivors are re-tested, which keeps this affordable.
void prune_redundant(const AtlasTreeData& d, std::vector<char>& in, const std::vector<char>& pinned)
{
	prune_leaves(d, in, pinned);
	bool changed = true;
	while (changed) {
		changed = false;
		for (int v : to_list(in)) {
			if (!in[v] || pinned[v] || v == d.root) continue;
			in[v] = 0;
			if (induced_connected(d, in)) { changed = true; prune_leaves(d, in, pinned); }
			else in[v] = 1;
		}
	}
}

// Every terminal's distance/parent map, computed once and reused by the exact
// solver's look-ahead bound and by the MST / star constructions below.
struct TermDists {
	std::vector<std::vector<int>> dist, prev;
};

TermDists term_dists(const AtlasTreeData& d, const Mask& blocked, const std::vector<int>& terms)
{
	TermDists td;
	td.dist.resize(terms.size());
	td.prev.resize(terms.size());
	for (size_t i = 0; i < terms.size(); i++)
		bfs(d, blocked, { terms[i] }, td.dist[i], td.prev[i]);
	return td;
}

// ---- exact solver: Dreyfus-Wagner on unit edge weights ----------------------

// terms must be distinct, valid and reachable from each other; terms[0] is the
// root. `ub` is a known-achievable edge count: states costing more than that
// cannot belong to a better tree, and pruning them is what keeps the DP
// affordable. `td` (optional) supplies per-terminal distances for the
// look-ahead bound. Fills *out and returns the edge count, or -1 when nothing
// beats (or matches) ub.
int dw_solve(const AtlasTreeData& d, const Mask& blocked, const std::vector<int>& terms, int ub,
             std::vector<char>* out, const TermDists* td = nullptr)
{
	const int n = (int)d.nodes.size();
	const int k = (int)terms.size();
	out->assign(n, 0);
	if (k == 0) return -1;
	if (k == 1) { (*out)[terms[0]] = 1; return 0; }
	if (ub < 0) ub = n;

	const size_t full = (size_t)1 << k;
	std::vector<int> dp(full * (size_t)n, kInf);
	// How each (S, v) got its value: pnode >= 0 means "walked in from that
	// neighbour with the same S"; otherwise pmask != 0 means "merged S from
	// pmask and S^pmask at this same node". Both unset = terminal seed.
	std::vector<int> pmask(full * (size_t)n, 0);
	std::vector<int> pnode(full * (size_t)n, -1);

	for (int i = 0; i < k; i++)
		dp[((size_t)1 << i) * n + terms[i]] = 0;

	// Dial buckets: every edge costs 1 and no useful state exceeds ub, so a
	// bucket queue replaces the binary heap outright. This is the difference
	// between a hover preview and a visible stall.
	std::vector<std::vector<int>> bucket((size_t)ub + 2);

	for (size_t S = 1; S < full; S++) {
		int* row = &dp[S * n];
		int* pm = &pmask[S * n];
		int* pn = &pnode[S * n];

		for (size_t sub = (S - 1) & S; sub; sub = (sub - 1) & S) {
			size_t other = S ^ sub;
			if (sub > other) continue;              // visit each split once
			const int* a = &dp[sub * n];
			const int* b = &dp[other * n];
			for (int v = 0; v < n; v++) {
				if (a[v] > ub || b[v] > ub) continue;
				int s = a[v] + b[v];
				if (s <= ub && s < row[v]) { row[v] = s; pm[v] = (int)sub; pn[v] = -1; }
			}
		}

		for (auto& b : bucket) b.clear();
		for (int v = 0; v < n; v++)
			if (!blocked[v] && row[v] <= ub) bucket[(size_t)row[v]].push_back(v);
		// (the look-ahead filter below runs after the relaxation, so states
		// only enter the buckets once)
		for (int dcur = 0; dcur <= ub; dcur++) {
			std::vector<int>& bk = bucket[(size_t)dcur];
			for (size_t qi = 0; qi < bk.size(); qi++) {   // bk only grows at dcur+1
				int v = bk[qi];
				if (row[v] != dcur) continue;             // stale entry
				if (dcur + 1 > ub) continue;
				for (int nb : d.nodes[v].adj) {
					if (blocked[nb]) continue;
					if (dcur + 1 < row[nb]) {
						row[nb] = dcur + 1;
						pm[nb] = 0;
						pn[nb] = v;
						bucket[(size_t)dcur + 1].push_back(nb);
					}
				}
			}
		}

		// Look-ahead: a state that still has to reach terminal t costs at least
		// dist(v, t) more. Retiring the states that cannot fit under ub is what
		// keeps the O(3^k * n) merge from doing most of its work for nothing.
		if (td && S != full - 1) {
			for (int v = 0; v < n; v++) {
				if (row[v] > ub) { row[v] = kInf; continue; }
				int need = 0;
				for (int i = 0; i < k && need >= 0; i++) {
					if (S & ((size_t)1 << i)) continue;
					int dv = td->dist[(size_t)i][v];
					if (dv < 0) { need = -1; break; }
					if (dv > need) need = dv;
				}
				if (need < 0 || row[v] + need > ub) row[v] = kInf;
			}
		}
	}

	const size_t last = full - 1;
	int best = -1, bestVal = kInf;
	for (int v = 0; v < n; v++) {
		int val = dp[last * n + v];
		if (val < bestVal) { bestVal = val; best = v; }
	}
	if (best < 0 || bestVal > ub) return -1;

	std::vector<std::pair<size_t, int>> stack{ { last, best } };
	while (!stack.empty()) {
		size_t S = stack.back().first;
		int v = stack.back().second;
		stack.pop_back();
		(*out)[v] = 1;
		int mk = pmask[S * n + v], nd = pnode[S * n + v];
		if (nd >= 0) {
			stack.emplace_back(S, nd);
		} else if (mk != 0) {
			stack.emplace_back((size_t)mk, v);
			stack.emplace_back(S ^ (size_t)mk, v);
		}
	}
	return bestVal;
}

// ---- heuristic --------------------------------------------------------------

// Grow a tree one terminal at a time. Empty `order` means "always take the
// nearest still-unconnected terminal".
std::vector<char> sph(const AtlasTreeData& d, const Mask& blocked, const std::vector<int>& terms,
                      const std::vector<int>& order)
{
	const int n = (int)d.nodes.size();
	std::vector<char> in(n, 0);
	in[d.root] = 1;

	std::vector<int> add;
	if (!order.empty()) {
		for (int t : order) {
			if (in[t]) continue;
			if (!path_into(d, blocked, in, t, &add)) continue;   // unreachable: skip
			for (int v : add) in[v] = 1;
		}
		return in;
	}

	std::vector<char> want(n, 0);
	int remaining = 0;
	for (int t : terms)
		if (!in[t] && !want[t]) { want[t] = 1; remaining++; }

	std::vector<int> dist, prev, sources;
	while (remaining > 0) {
		// Full BFS, then pick the closest wanted terminal, ties by lowest node
		// index -- picking during the BFS would make the result depend on
		// adjacency order.
		sources = to_list(in);
		bfs(d, blocked, sources, dist, prev);
		int pick = -1;
		for (int v = 0; v < n; v++) {
			if (!want[v] || dist[v] < 0) continue;
			if (pick == -1 || dist[v] < dist[pick]) pick = v;
		}
		if (pick == -1) break;                          // the rest is unreachable
		for (int at = pick; at != -1 && !in[at]; at = prev[at]) in[at] = 1;
		want[pick] = 0;
		remaining--;
		for (int v = 0; v < n; v++)
			if (want[v] && in[v]) { want[v] = 0; remaining--; }
	}
	return in;
}

// Walk `prev` back from v to terminal i, marking the path.
void mark_path(const TermDists& td, size_t i, int v, std::vector<char>& in)
{
	for (int at = v; at != -1; at = td.prev[i][at]) {
		in[at] = 1;
		if (td.dist[i][at] == 0) break;
	}
}

// Kou-Markowsky: minimum spanning tree over the terminals in the metric
// closure, each MST edge expanded back into a shortest path.
std::vector<char> kmb(const AtlasTreeData& d, const std::vector<int>& terms, const TermDists& td)
{
	const int n = (int)d.nodes.size();
	const int k = (int)terms.size();
	std::vector<char> in(n, 0);
	std::vector<char> used(k, 0);
	std::vector<int> best(k, kInf), from(k, -1);
	used[0] = 1;
	in[terms[0]] = 1;
	for (int j = 1; j < k; j++) {
		int dj = td.dist[0][terms[j]];
		best[j] = dj < 0 ? kInf : dj;
		from[j] = 0;
	}
	for (int step = 1; step < k; step++) {
		int pick = -1;
		for (int j = 0; j < k; j++) {
			if (used[j] || best[j] >= kInf) continue;
			if (pick == -1 || best[j] < best[pick]) pick = j;
		}
		if (pick == -1) break;                          // unreachable remainder
		used[pick] = 1;
		mark_path(td, (size_t)from[pick], terms[pick], in);
		for (int j = 0; j < k; j++) {
			if (used[j]) continue;
			int dj = td.dist[pick][terms[j]];
			if (dj >= 0 && dj < best[j]) { best[j] = dj; from[j] = pick; }
		}
	}
	return in;
}

// Star construction: pick a hub and route every terminal to it. The optimum
// often shares one junction, and neither SPH nor the MST finds that junction on
// its own -- this is what closes most of the remaining gap.
void star_candidates(const AtlasTreeData& d, const std::vector<int>& terms, const TermDists& td,
                     std::vector<std::vector<char>>* out)
{
	const int n = (int)d.nodes.size();
	const int k = (int)terms.size();
	std::vector<std::pair<int, int>> score;              // (sum of distances, node)
	score.reserve(n);
	for (int v = 0; v < n; v++) {
		long long s = 0;
		bool ok = true;
		for (int i = 0; i < k && ok; i++) {
			int dv = td.dist[i][v];
			if (dv < 0) ok = false;
			else s += dv;
		}
		if (ok && s < kInf) score.emplace_back((int)std::min<long long>(s, kInf - 1), v);
	}
	std::sort(score.begin(), score.end());               // ties by node index
	int take = std::min((int)score.size(), kStarHubs);
	for (int i = 0; i < take; i++) {
		std::vector<char> in(n, 0);
		int hub = score[(size_t)i].second;
		in[hub] = 1;
		for (int j = 0; j < k; j++)
			mark_path(td, (size_t)j, hub, in);
		out->push_back(std::move(in));
	}
}

// Re-insertion improvement: for each terminal in turn, strip everything that
// exists only to reach it (un-pin it, then prune) and reconnect it by the
// shortest path. Strict improvements only, so it can never grow or oscillate.
void improve(const AtlasTreeData& d, const Mask& blocked, std::vector<char>& in,
             const std::vector<char>& pinned, const std::vector<int>& terms)
{
	std::vector<int> add;
	int cur = count_set(in);
	for (int pass = 0; pass < kImprovePasses; pass++) {
		bool gained = false;
		for (int t : terms) {
			if (t == d.root) continue;
			std::vector<char> trial = in;
			std::vector<char> pin2 = pinned;
			pin2[t] = 0;
			// Leaf peeling only: t's private branch is a pendant path in every
			// case that matters, and the full cycle-aware pass here would cost
			// two orders of magnitude more for the same result.
			prune_leaves(d, trial, pin2);
			if (!trial[t]) {
				if (!path_into(d, blocked, trial, t, &add)) continue;
				for (int v : add) trial[v] = 1;
			}
			int c = count_set(trial);
			if (c < cur) { in = trial; cur = c; gained = true; }
		}
		if (!gained) break;
	}
}

// Best of every construction, pruned. `warm` (may be empty) is included so an
// allocation already on screen is never thrown away for an equal-cost one.
std::vector<char> heuristic(const AtlasTreeData& d, const Mask& blocked, const std::vector<int>& terms,
                            const std::vector<char>& pinned, const std::vector<int>& warm,
                            const TermDists& td)
{
	const int n = (int)d.nodes.size();
	std::vector<std::vector<char>> cands;
	if (!warm.empty()) {
		std::vector<char> w(n, 0);
		// Blocked nodes MUST be dropped here. The warm start is the tree already
		// on screen, and the user may have just ruled out a node that is on it --
		// keeping it would smuggle an excluded node into the winning candidate,
		// which is exactly the bug this filter was reported for.
		for (int v : warm)
			if (v >= 0 && v < n && !blocked[v]) w[v] = 1;
		w[d.root] = 1;
		bool covers = induced_connected(d, w);
		for (int t : terms) covers = covers && w[t];
		if (covers) cands.push_back(std::move(w));
	}
	cands.push_back(sph(d, blocked, terms, {}));
	cands.push_back(kmb(d, terms, td));

	std::vector<int> order(terms.begin(), terms.end());
	order.erase(std::remove(order.begin(), order.end(), d.root), order.end());
	{
		std::vector<int> byNear = order;
		std::sort(byNear.begin(), byNear.end(), [&](int a, int b) {
			int da = td.dist[0][a], db = td.dist[0][b];
			return da != db ? da < db : a < b;
		});
		cands.push_back(sph(d, blocked, terms, byNear));
		std::reverse(byNear.begin(), byNear.end());
		cands.push_back(sph(d, blocked, terms, byNear));
	}
	// Fixed seed: restarts must be reproducible or the tree would reshuffle
	// between two identical solves.
	std::mt19937 rng(0x5EED2026u);
	for (int r = 0; r < kSphRestarts; r++) {
		std::vector<int> perm = order;
		std::shuffle(perm.begin(), perm.end(), rng);
		cands.push_back(sph(d, blocked, terms, perm));
	}
	star_candidates(d, terms, td, &cands);

	// Cheap peel first so the ranking is meaningful, then spend the expensive
	// re-insertion pass on the few best -- the cheapest construction is often
	// not the one that improves furthest.
	std::vector<std::pair<int, int>> rank;
	for (int i = 0; i < (int)cands.size(); i++) {
		prune_leaves(d, cands[i], pinned);
		rank.emplace_back(count_set(cands[i]), i);
	}
	std::sort(rank.begin(), rank.end());
	std::vector<char> best;
	int bestCount = 0;
	int take = std::min((int)rank.size(), kImproveCandidates);
	for (int r = 0; r < take; r++) {
		std::vector<char>& c = cands[(size_t)rank[(size_t)r].second];
		prune_redundant(d, c, pinned);
		improve(d, blocked, c, pinned, terms);
		int cnt = count_set(c);
		if (best.empty() || cnt < bestCount) { best = c; bestCount = cnt; }
	}
	prune_redundant(d, best, pinned);   // improve() peels leaves only
	return best;
}

} // namespace

int AtlasOptExactCap() { return kExactTargetCap; }

bool AtlasPlan::has(int idx) const
{
	return std::binary_search(nodes.begin(), nodes.end(), idx);
}

AtlasPlan AtlasPlanMinimal(const AtlasTreeData& d,
                           const std::vector<int>& targets,
                           const std::vector<int>& blockedIdx,
                           const std::vector<int>& warmStart)
{
	AtlasPlan plan;
	const int n = (int)d.nodes.size();
	if (n == 0 || d.root < 0 || d.root >= n) return plan;

	Mask blocked(n, 0);
	for (int b : blockedIdx)
		if (b >= 0 && b < n && b != d.root) blocked[b] = 1;   // the start is never blockable

	// Reachability filter first, ON THE BLOCKED-OUT GRAPH: a target the tree
	// cannot connect -- because the data is broken, or because the user walled
	// it off -- must not sink the whole solve. Report it and carry on. This is
	// also what keeps a walled-off target from being left allocated but
	// disconnected from the start, which is the failure mode this whole model
	// has to avoid.
	std::vector<int> reachDist, reachPrev;
	bfs(d, blocked, { d.root }, reachDist, reachPrev);

	std::vector<char> pinned(n, 0);
	std::vector<int> terms{ d.root };
	pinned[d.root] = 1;
	for (int t : targets) {
		if (t < 0 || t >= n || pinned[t]) continue;
		if (blocked[t] || reachDist[t] < 0) { plan.unreachable.push_back(t); continue; }
		pinned[t] = 1;
		terms.push_back(t);
	}
	std::sort(plan.unreachable.begin(), plan.unreachable.end());
	// Deterministic terminal order (root first): the DP's bit assignment and
	// every tie-break below key off it.
	std::sort(terms.begin() + 1, terms.end());

	const int targetCount = (int)terms.size() - 1;
	std::vector<char> in;
	if (targetCount == 0) {
		in.assign(n, 0);
		in[d.root] = 1;
		plan.exact = true;
	} else {
		// The heuristic always runs: below the cap it supplies the upper bound
		// and the look-ahead distances that make the exact DP affordable, above
		// it it IS the answer.
		TermDists td = term_dists(d, blocked, terms);
		in = heuristic(d, blocked, terms, pinned, warmStart, td);
		if (targetCount <= kExactTargetCap) {
			std::vector<char> exact;
			int edges = dw_solve(d, blocked, terms, count_set(in) - 1, &exact, &td);
			if (edges >= 0 && count_set(exact) < count_set(in)) in = exact;
			plan.exact = true;
		}
	}

	// Stability: when the tree already on screen is a valid solution of the
	// same size, keep it. Wiring must never shuffle unless it buys a point.
	// A warm start can legitimately contain a node the user has just blocked
	// (they marked something that was already allocated), so it is filtered
	// before use -- otherwise the tie-break would smuggle it back in.
	if (!warmStart.empty()) {
		std::vector<char> w(n, 0);
		for (int v : warmStart)
			if (v >= 0 && v < n && !blocked[v]) w[v] = 1;
		w[d.root] = 1;
		bool covers = induced_connected(d, w);
		for (int t : terms) covers = covers && w[t];
		if (covers && count_set(w) <= count_set(in)) in = w;
	}

	plan.nodes = to_list(in);
	plan.points = plan.nodes.empty() ? 0 : (int)plan.nodes.size() - 1;
	return plan;
}

std::vector<int> AtlasTargetsAfterClick(const AtlasTreeData& d, int idx)
{
	std::vector<int> out = d.TargetIdx();
	if (idx < 0 || idx >= (int)d.nodes.size() || d.nodes[idx].kind == kAtlasStart) return out;

	if (!d.nodes[idx].alloc) {                       // a new deliberate pick
		out.push_back(idx);
		std::sort(out.begin(), out.end());
		return out;
	}
	if (d.nodes[idx].target) {                       // un-pick it
		out.erase(std::remove(out.begin(), out.end(), idx), out.end());
		return out;
	}
	std::vector<int> lost = d.FindRemoveSet(idx);
	for (int l : lost)
		out.erase(std::remove(out.begin(), out.end(), l), out.end());
	return out;
}

std::vector<int> AtlasInferTargets(const AtlasTreeData& d)
{
	const int n = (int)d.nodes.size();
	std::vector<int> out;
	for (int i = 0; i < n; i++) {
		const AtlasNode& node = d.nodes[i];
		if (!node.alloc || node.kind == kAtlasStart) continue;
		if (node.kind == kAtlasNotable || node.kind == kAtlasKeystone || node.kind == kAtlasWormhole) {
			out.push_back(i);
			continue;
		}
		int deg = 0;
		for (int nb : node.adj)
			if (d.nodes[nb].alloc) deg++;
		if (deg <= 1) out.push_back(i);   // leaf of the allocated subgraph
	}
	return out;
}

// ---- selftest ---------------------------------------------------------------

namespace {

// Selftest shorthand: most cases have nothing blocked. Named rather than
// inlined so a case that DOES block reads as deliberate.
Mask no_block(const AtlasTreeData& g) { return Mask(g.nodes.size(), 0); }

struct TestReport {
	std::string text;
	int failures = 0;

	void check(bool ok, const char* what, const std::string& detail = std::string())
	{
		text += ok ? "PASS  " : "FAIL  ";
		text += what;
		if (!detail.empty()) { text += "  ("; text += detail; text += ")"; }
		text += "\n";
		if (!ok) failures++;
	}
	void note(const std::string& s) { text += "      " + s + "\n"; }
};

// Independent minimum over every node subset: the ground truth the DP is
// checked against. Only usable for tiny graphs.
int brute_force(const AtlasTreeData& d, const std::vector<int>& terms)
{
	const int n = (int)d.nodes.size();
	int best = -1;
	for (unsigned mask = 0; mask < (1u << n); mask++) {
		bool okTerms = true;
		for (int t : terms) okTerms = okTerms && (mask & (1u << t));
		if (!okTerms) continue;
		std::vector<char> in(n, 0);
		for (int i = 0; i < n; i++) in[i] = (char)((mask >> i) & 1);
		if (!induced_connected(d, in)) continue;
		int c = count_set(in);
		if (best == -1 || c < best) best = c;
	}
	return best;
}

// Deterministic random connected graph for the brute-force comparison.
AtlasTreeData make_small_graph(unsigned seed, int n)
{
	std::mt19937 rng(seed);
	AtlasTreeData d;
	d.nodes.resize(n);
	d.root = 0;
	for (int i = 0; i < n; i++) {
		d.nodes[i].id = 1000 + i;
		d.nodes[i].kind = kAtlasNormal;
	}
	d.nodes[0].kind = kAtlasStart;
	auto link = [&](int a, int b) {
		if (a == b) return;
		for (int x : d.nodes[a].adj)
			if (x == b) return;
		d.nodes[a].adj.push_back(b);
		d.nodes[b].adj.push_back(a);
		AtlasEdge e;
		e.a = a; e.b = b;
		d.edges.push_back(e);
	};
	for (int i = 1; i < n; i++)                       // random spanning tree
		link(i, (int)(rng() % (unsigned)i));
	int extra = (int)(rng() % (unsigned)std::max(1, n / 2));
	for (int i = 0; i < extra; i++)
		link((int)(rng() % (unsigned)n), (int)(rng() % (unsigned)n));
	return d;
}

// The planner's previous click behaviour, replayed headlessly: each target in
// turn gets the multi-source BFS shortest path from whatever is allocated.
int greedy_points(const AtlasTreeData& d, const std::vector<int>& order)
{
	const Mask nb = no_block(d);
	std::vector<char> in((size_t)d.nodes.size(), 0);
	in[d.root] = 1;
	std::vector<int> add;
	for (int t : order) {
		if (!path_into(d, nb, in, t, &add)) return -1;
		for (int v : add) in[v] = 1;
	}
	return count_set(in) - 1;
}

double ms_since(std::chrono::steady_clock::time_point t0)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

int RunAtlasOptSelfTest(const std::wstring& exeDir)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}

	TestReport rep;

	// --- O1: exactness against brute force on small random graphs ------------
	{
		int cases = 0, bad = 0;
		for (unsigned seed = 1; seed <= 40; seed++) {
			AtlasTreeData g = make_small_graph(seed, 12);
			std::mt19937 rng(seed * 7919u);
			int k = 2 + (int)(rng() % 3);
			std::vector<int> terms{ 0 };
			while ((int)terms.size() < k + 1) {
				int v = 1 + (int)(rng() % 11);
				if (std::find(terms.begin(), terms.end(), v) == terms.end()) terms.push_back(v);
			}
			std::sort(terms.begin() + 1, terms.end());
			int truth = brute_force(g, terms);
			TermDists td = term_dists(g, no_block(g), terms);
			// Both with and without the look-ahead filter: an unsound bound
			// would silently prune the optimum, and only this catches it.
			for (int variant = 0; variant < 2; variant++) {
				std::vector<char> got;
				int edges = dw_solve(g, no_block(g), terms, (int)g.nodes.size(), &got, variant ? &td : nullptr);
				int nodesUsed = count_set(got);
				cases++;
				if (truth < 0 || nodesUsed != truth || edges != nodesUsed - 1) {
					bad++;
					if (bad <= 3)
						rep.note("seed " + std::to_string(seed) + " variant " + std::to_string(variant) +
						         ": dw=" + std::to_string(nodesUsed) + " edges=" + std::to_string(edges) +
						         " brute=" + std::to_string(truth));
				}
			}
		}
		rep.check(bad == 0, "exact solver matches brute force, with and without look-ahead pruning",
		          std::to_string(cases) + " cases, " + std::to_string(bad) + " mismatches");
	}

	// --- O1b: the upper-bound pruning must not change the answer -------------
	{
		int bad = 0, cases = 0;
		for (unsigned seed = 41; seed <= 70; seed++) {
			AtlasTreeData g = make_small_graph(seed, 11);
			std::mt19937 rng(seed * 104729u);
			std::vector<int> terms{ 0 };
			while (terms.size() < 4) {
				int v = 1 + (int)(rng() % 10);
				if (std::find(terms.begin(), terms.end(), v) == terms.end()) terms.push_back(v);
			}
			std::sort(terms.begin() + 1, terms.end());
			TermDists td = term_dists(g, no_block(g), terms);
			std::vector<char> loose, tight;
			int a = dw_solve(g, no_block(g), terms, (int)g.nodes.size(), &loose);
			int b = dw_solve(g, no_block(g), terms, a, &tight, &td);
			cases++;
			if (a != b || count_set(loose) != count_set(tight)) bad++;
		}
		rep.check(bad == 0, "upper-bound pruning does not change the optimum",
		          std::to_string(cases) + " cases, " + std::to_string(bad) + " differ");
	}

	// --- O2: disconnected graph degrades instead of failing -------------------
	{
		AtlasTreeData g = make_small_graph(3, 10);
		AtlasNode iso;                                  // island with no edges
		iso.id = 9999;
		iso.kind = kAtlasNotable;
		g.nodes.push_back(iso);
		AtlasPlan p = AtlasPlanMinimal(g, { 4, (int)g.nodes.size() - 1 }, {}, {});
		rep.check(p.unreachable.size() == 1 && p.unreachable[0] == (int)g.nodes.size() - 1 &&
		          p.has(4) && !p.ok(),
		          "unreachable target is reported, the rest still solves");
	}

	// --- the real tree --------------------------------------------------------
	AtlasTreeData d;
	std::string err;
	if (!d.Load(exeDir, &err)) {
		rep.check(false, "load atlas_tree_poe1.json", err);
		printf("%s", rep.text.c_str());
		write_file_utf8(exeDir + L"atlas_opt_selftest.txt", rep.text);
		return 1;
	}
	rep.note("nodes=" + std::to_string(d.nodes.size()) + " edges=" + std::to_string(d.edges.size()) +
	         " totalPoints=" + std::to_string(d.TotalPoints()) +
	         " exactCap=" + std::to_string(kExactTargetCap));

	std::vector<int> notables;
	for (int i = 0; i < (int)d.nodes.size(); i++)
		if ((d.nodes[i].kind == kAtlasNotable || d.nodes[i].kind == kAtlasKeystone) && i != d.root)
			notables.push_back(i);
	rep.check(notables.size() > 100, "notable pool available", std::to_string(notables.size()));

	auto pick = [&](std::mt19937& rng, int k) {
		std::vector<int> tg;
		while ((int)tg.size() < k) {
			int v = notables[rng() % notables.size()];
			if (std::find(tg.begin(), tg.end(), v) == tg.end()) tg.push_back(v);
		}
		std::sort(tg.begin(), tg.end());
		return tg;
	};

	// --- O3: one target == the existing shortest-path answer ------------------
	{
		std::mt19937 rng(11);
		int bad = 0, checked = 0;
		for (int i = 0; i < 40; i++) {
			int t = notables[rng() % notables.size()];
			d.Reset();
			std::vector<int> viaModel = d.FindPathTo(t);
			AtlasPlan p = AtlasPlanMinimal(d, { t }, {}, {});
			checked++;
			if (p.points != (int)viaModel.size() || !p.has(t)) bad++;
		}
		rep.check(bad == 0, "single target equals FindPathTo length",
		          std::to_string(checked) + " sampled, " + std::to_string(bad) + " differ");
	}

	// --- O4: never worse than the old greedy, over every click order ----------
	{
		std::mt19937 rng(23);
		int worse = 0, orders = 0, sets = 0, better = 0, bestGap = 0;
		for (int i = 0; i < 25; i++) {
			std::vector<int> tg = pick(rng, 2 + (int)(rng() % 3));
			AtlasPlan p = AtlasPlanMinimal(d, tg, {}, {});
			sets++;
			std::vector<int> perm = tg;
			do {
				int g = greedy_points(d, perm);
				orders++;
				if (g < p.points) worse++;
				if (g > p.points) { better++; bestGap = std::max(bestGap, g - p.points); }
			} while (std::next_permutation(perm.begin(), perm.end()));
		}
		rep.check(worse == 0, "minimum solution is never beaten by the greedy click order",
		          std::to_string(sets) + " sets / " + std::to_string(orders) + " orders, " +
		          std::to_string(worse) + " violations");
		rep.note("greedy was strictly worse in " + std::to_string(better) + " of " +
		         std::to_string(orders) + " orders, by up to " + std::to_string(bestGap) + " points");
	}

	// --- O5: order independence (the headline promise) ------------------------
	{
		std::mt19937 rng(31);
		int bad = 0, sets = 0;
		for (int i = 0; i < 15; i++) {
			std::vector<int> tg = pick(rng, 2 + (int)(rng() % 3));
			std::vector<int> ref = AtlasPlanMinimal(d, tg, {}, {}).nodes;
			sets++;
			std::vector<int> perm = tg;
			do {
				// replay a full click sequence in this order, each click
				// re-solving from the previous allocation
				std::vector<int> acc, alloc;
				for (int t : perm) {
					acc.push_back(t);
					alloc = AtlasPlanMinimal(d, acc, {}, alloc).nodes;
				}
				if (alloc != ref) bad++;
			} while (std::next_permutation(perm.begin(), perm.end()));
		}
		rep.check(bad == 0, "clicking the same targets in any order gives the same allocation",
		          std::to_string(sets) + " sets, " + std::to_string(bad) + " order-dependent");
	}

	// --- O6: determinism + stability -----------------------------------------
	{
		std::mt19937 rng(41);
		std::vector<int> tg = pick(rng, 5);
		AtlasPlan a = AtlasPlanMinimal(d, tg, {}, {});
		AtlasPlan b = AtlasPlanMinimal(d, tg, {}, {});
		rep.check(a.nodes == b.nodes && a.points == b.points, "same input gives the same plan twice");

		AtlasPlan c = AtlasPlanMinimal(d, tg, {}, a.nodes);
		rep.check(c.nodes == a.nodes, "re-solving with the current tree as warm start changes nothing");

		std::vector<int> shuffled = tg;
		std::reverse(shuffled.begin(), shuffled.end());
		AtlasPlan e = AtlasPlanMinimal(d, shuffled, {}, a.nodes);
		rep.check(e.nodes == a.nodes, "target list order does not affect the result");
	}

	// --- O7: targets always survive, removal never costs more -----------------
	{
		std::mt19937 rng(53);
		int missing = 0, grew = 0, rounds = 0;
		for (int i = 0; i < 20; i++) {
			std::vector<int> tg = pick(rng, 3 + (int)(rng() % 4));
			AtlasPlan p = AtlasPlanMinimal(d, tg, {}, {});
			for (int t : tg)
				if (!p.has(t)) missing++;
			std::vector<int> less(tg.begin(), tg.end() - 1);
			AtlasPlan q = AtlasPlanMinimal(d, less, {}, p.nodes);
			rounds++;
			if (q.points > p.points) grew++;
		}
		rep.check(missing == 0, "every target is present in the plan", std::to_string(missing) + " missing");
		rep.check(grew == 0, "dropping a target never costs more points",
		          std::to_string(rounds) + " rounds, " + std::to_string(grew) + " grew");
	}

	// --- O8: heuristic quality, measured against the exact solver -------------
	{
		std::mt19937 rng(67);
		int samples = 0, equal = 0, worseBy = 0, maxGap = 0;
		for (int i = 0; i < 15; i++) {
			std::vector<int> tg = pick(rng, kExactTargetCap);
			std::vector<int> terms{ d.root };
			terms.insert(terms.end(), tg.begin(), tg.end());
			std::sort(terms.begin() + 1, terms.end());
			std::vector<char> pinned(d.nodes.size(), 0);
			for (int t : terms) pinned[t] = 1;

			TermDists td = term_dists(d, no_block(d), terms);
			std::vector<char> h = heuristic(d, no_block(d), terms, pinned, {}, td);
			std::vector<char> exactSet;
			int exactEdges = dw_solve(d, no_block(d), terms, count_set(h) - 1, &exactSet, &td);
			int hEdges = count_set(h) - 1;
			int truth = exactEdges >= 0 ? exactEdges : hEdges;   // -1 == nothing beat the bound
			int gap = hEdges - truth;
			samples++;
			if (gap == 0) equal++;
			else { worseBy += gap; maxGap = std::max(maxGap, gap); }
		}
		// The product guarantee is "exact at or below the cap, approximate and
		// labelled above it" -- so this bounds the approximation rather than
		// pretending it is always optimal, and prints the real numbers.
		rep.check(maxGap <= 2, "heuristic stays within the stated bound of the optimum",
		          std::to_string(equal) + "/" + std::to_string(samples) + " exact, worst gap " +
		          std::to_string(maxGap) + ", total overshoot " + std::to_string(worseBy));
	}

	// --- O9: large target sets stay sane --------------------------------------
	{
		std::mt19937 rng(71);
		std::vector<int> tg = pick(rng, 30);
		AtlasPlan p = AtlasPlanMinimal(d, tg, {}, {});
		rep.note("30 scattered notables need " + std::to_string(p.points) + " points (budget " +
		         std::to_string(d.TotalPoints()) + ")");
		rep.check(p.points > 0 && (int)p.nodes.size() == p.points + 1,
		          "plan point count matches its node count");
		bool allThere = true;
		for (int t : tg) allThere = allThere && p.has(t);
		rep.check(allThere, "heuristic keeps every target too");
	}

	// --- O10: timings ---------------------------------------------------------
	{
		std::mt19937 rng(83);
		double capMs = 0, worstHeur = 0;
		for (int k : { 4, 6, 8, 9, 10, 11 }) {
			std::vector<int> tg = pick(rng, k);
			std::vector<int> terms{ d.root };
			terms.insert(terms.end(), tg.begin(), tg.end());
			std::sort(terms.begin() + 1, terms.end());
			std::vector<char> pinned(d.nodes.size(), 0);
			for (int t : terms) pinned[t] = 1;
			TermDists td = term_dists(d, no_block(d), terms);
			std::vector<char> h = heuristic(d, no_block(d), terms, pinned, {}, td);
			std::vector<char> got;
			auto t0 = std::chrono::steady_clock::now();
			dw_solve(d, no_block(d), terms, count_set(h) - 1, &got, &td);
			double msec = ms_since(t0);
			rep.note("exact k=" + std::to_string(k) + ": " + std::to_string((int)msec) + " ms");
			if (k == kExactTargetCap) capMs = msec;
		}
		for (int k : { 5, 15, 30, 45 }) {
			std::vector<int> tg = pick(rng, k);
			auto t0 = std::chrono::steady_clock::now();
			AtlasPlan p = AtlasPlanMinimal(d, tg, {}, {});
			double msec = ms_since(t0);
			worstHeur = std::max(worstHeur, msec);
			rep.note("full solve k=" + std::to_string(k) + ": " + std::to_string((int)msec) +
			         " ms -> " + std::to_string(p.points) + " points" + (p.exact ? " (exact)" : ""));
		}
		// Budget rationale: a solve only ever runs after the hover has settled
		// (see AtlasView's debounce) or on a click, so ~150 ms is the point
		// where it would start to feel like a stall.
		rep.check(capMs < 150.0, "exact solve at the cap stays interactive",
		          "k=" + std::to_string(kExactTargetCap) + " took " + std::to_string((int)capMs) + " ms");
		rep.check(worstHeur < 150.0, "full solve stays interactive at every size",
		          "worst " + std::to_string((int)worstHeur) + " ms");
	}

	// --- O10b: blocked nodes ---------------------------------------------------
	// This is where PR #2's planning mode went wrong: walling a target off left
	// it allocated but disconnected from the start, and the counter still
	// reported "N/N reached". Both properties are asserted here directly.
	{
		std::mt19937 rng(131);
		auto connected_from_root = [&](const std::vector<int>& nodes) {
			std::vector<char> in(d.nodes.size(), 0);
			for (int v : nodes) in[v] = 1;
			if (nodes.empty()) return true;
			return induced_connected(d, in);
		};

		int orphaned = 0, usedBlocked = 0, wrongReport = 0, walled = 0, rounds = 0;
		for (int i = 0; i < 20; i++) {
			std::vector<int> tg = pick(rng, 3);
			// Wall the first target off completely: every neighbour blocked.
			std::vector<int> blk;
			for (int nb : d.nodes[tg[0]].adj) blk.push_back(nb);
			AtlasPlan p = AtlasPlanMinimal(d, tg, blk, {});
			rounds++;

			if (!connected_from_root(p.nodes)) orphaned++;
			for (int b : blk)
				if (p.has(b)) usedBlocked++;
			// The walled-off target must be REPORTED, not silently included.
			bool reported = std::find(p.unreachable.begin(), p.unreachable.end(), tg[0]) != p.unreachable.end();
			if (reported) walled++;
			if (p.has(tg[0]) != !reported) wrongReport++;
		}
		rep.check(orphaned == 0, "a walled-off target never leaves a disconnected allocation",
		          std::to_string(rounds) + " rounds, " + std::to_string(orphaned) + " orphaned");
		rep.check(usedBlocked == 0, "no plan ever routes through a blocked node",
		          std::to_string(usedBlocked) + " violations");
		rep.check(walled > 0, "the wall-off case actually occurred", std::to_string(walled) + "/" +
		          std::to_string(rounds));
		rep.check(wrongReport == 0, "unreachable targets are reported, not silently claimed",
		          std::to_string(wrongReport) + " mismatches");

		// Field-reported case: block a node that is ALREADY on the allocated
		// path. The warm start still contains it, so this is the one route by
		// which a blocked node could survive into the result.
		{
			std::mt19937 r3(211);
			int stillThere = 0, tried = 0;
			for (int i = 0; i < 15; i++) {
				std::vector<int> tg = pick(r3, 4);
				AtlasPlan p = AtlasPlanMinimal(d, tg, {}, {});
				// pick an allocated node that is neither root nor a target
				int victim = -1;
				for (int v : p.nodes) {
					if (v == d.root) continue;
					if (std::find(tg.begin(), tg.end(), v) != tg.end()) continue;
					victim = v;
					break;
				}
				if (victim < 0) continue;
				tried++;
				AtlasPlan q = AtlasPlanMinimal(d, tg, { victim }, p.nodes);
				if (q.has(victim)) stillThere++;
			}
			rep.check(tried > 0, "found allocated wiring to block", std::to_string(tried) + " cases");
			rep.check(stillThere == 0, "blocking an already-allocated node drops it from the plan",
			          std::to_string(tried) + " tried, " + std::to_string(stillThere) + " survived");
		}

		// Blocking must not degrade optimality: the answer has to equal the exact
		// solve on the graph with those nodes deleted.
		int mismatch = 0, cases = 0;
		for (unsigned seed = 71; seed <= 95; seed++) {
			AtlasTreeData g = make_small_graph(seed, 12);
			std::mt19937 r2(seed * 7717u);
			std::vector<int> terms{ 0 };
			while (terms.size() < 4) {
				int v = 1 + (int)(r2() % 11);
				if (std::find(terms.begin(), terms.end(), v) == terms.end()) terms.push_back(v);
			}
			std::vector<int> blk;
			for (int v = 1; v < 12; v++)
				if (std::find(terms.begin(), terms.end(), v) == terms.end() && (r2() % 4) == 0)
					blk.push_back(v);
			if (blk.empty()) continue;

			std::vector<int> tgs(terms.begin() + 1, terms.end());
			AtlasPlan p = AtlasPlanMinimal(g, tgs, blk, {});
			if (!p.ok()) continue;                  // walled off: covered above
			// Ground truth: brute force over the graph with blocked nodes removed.
			Mask bm(g.nodes.size(), 0);
			for (int b : blk) bm[b] = 1;
			int best = -1;
			for (unsigned mask = 0; mask < (1u << g.nodes.size()); mask++) {
				bool ok = true;
				for (int t : terms) ok = ok && (mask & (1u << t));
				for (int b : blk) ok = ok && !(mask & (1u << b));
				if (!ok) continue;
				std::vector<char> in(g.nodes.size(), 0);
				for (size_t v = 0; v < g.nodes.size(); v++) in[v] = (char)((mask >> v) & 1);
				if (!induced_connected(g, in)) continue;
				int c = count_set(in);
				if (best == -1 || c < best) best = c;
			}
			cases++;
			if (best >= 0 && (int)p.nodes.size() != best) mismatch++;
		}
		rep.check(cases > 0 && mismatch == 0, "blocking keeps the solution exactly optimal",
		          std::to_string(cases) + " cases, " + std::to_string(mismatch) + " mismatches");
	}

	// --- O10c: the sandbox contract ------------------------------------------
	// The planner keeps the sandbox, but the property that matters is pure
	// state: entering (snapshot + clear), marking, then discarding must restore
	// the three fields element for element. Replayed here because the ImGui side
	// cannot be driven headlessly (see error_win_gui_test_capture), and because
	// "abandoning a plan changes nothing" is the guarantee the mode is sold on.
	{
		std::mt19937 rng(151);
		std::vector<int> tg = pick(rng, 4);
		AtlasPlan base = AtlasPlanMinimal(d, tg, {}, {});
		d.Reset();
		d.SetAllocSet(base.nodes);
		for (int t : tg) d.nodes[t].target = true;
		std::vector<int> blk0 = { pick(rng, 1).front() };
		d.nodes[blk0.front()].blocked = true;

		const std::vector<int> beforeAlloc = d.AllocIds();
		const std::vector<int> beforeTargets = d.TargetIds();
		const std::vector<int> beforeBlocked = d.BlockedIds();

		// enter: snapshot then blank slate
		std::vector<int> snapA = beforeAlloc, snapT = beforeTargets, snapB = beforeBlocked;
		d.Reset();
		rep.check(d.UsedPoints() == 0 && d.TargetIdx().empty() && d.BlockedIdx().empty(),
		          "entering the sandbox starts from a blank slate");

		// mark a completely different set inside the sandbox
		std::vector<int> other = pick(rng, 3);
		for (int t : other) d.nodes[t].target = true;
		AtlasPlan p2 = AtlasPlanMinimal(d, d.TargetIdx(), d.BlockedIdx(), d.AllocIdx());
		d.SetAllocSet(p2.nodes);
		rep.check(d.UsedPoints() > 0 && d.AllocIds() != beforeAlloc,
		          "the sandbox really diverged from the saved state");

		// discard: restore
		d.ApplyAllocIds(snapA);
		d.ApplyTargetIds(snapT);
		d.ApplyBlockedIds(snapB);
		rep.check(d.AllocIds() == beforeAlloc && d.TargetIds() == beforeTargets &&
		          d.BlockedIds() == beforeBlocked,
		          "discarding restores allocation, targets and blocked exactly",
		          std::to_string(beforeAlloc.size()) + " nodes / " +
		          std::to_string(beforeTargets.size()) + " targets / " +
		          std::to_string(beforeBlocked.size()) + " blocked");

		// and the serialized project is byte-identical, which is the form the
		// guarantee actually takes on disk
		AtlasBuildFile f1;
		f1.ParseDoc(u8"{\"builds\":[{\"name\":\"p\",\"alloc\":[]}]}");
		f1.Active().alloc = beforeAlloc;
		f1.Active().targets = beforeTargets;
		f1.Active().blocked = beforeBlocked;
		AtlasBuildFile f2;
		f2.ParseDoc(u8"{\"builds\":[{\"name\":\"p\",\"alloc\":[]}]}");
		f2.Active().alloc = d.AllocIds();
		f2.Active().targets = d.TargetIds();
		f2.Active().blocked = d.BlockedIds();
		rep.check(f1.SerializeDoc() == f2.SerializeDoc(),
		          "the project serializes byte-identically after a discarded plan");
		d.Reset();
	}

	// --- O11: the click model itself, driven through the real entry points ----
	// The planner's click handler is ImGui code that cannot be exercised
	// headlessly, but the decision it makes is all here, so this replays it.
	{
		std::mt19937 rng(101);
		auto click = [&](int idx) {
			std::vector<int> tg = AtlasTargetsAfterClick(d, idx);
			AtlasPlan p = AtlasPlanMinimal(d, tg, {}, d.AllocIdx());
			d.SetAllocSet(p.nodes);
			for (AtlasNode& nd : d.nodes) nd.target = false;
			for (int t : tg) d.nodes[t].target = true;
			return p;
		};

		d.Reset();
		std::vector<int> tg = pick(rng, 4);
		for (int t : tg) click(t);
		int afterAdds = d.UsedPoints();
		bool allAlloc = true, allPinned = true;
		for (int t : tg) { allAlloc = allAlloc && d.nodes[t].alloc; allPinned = allPinned && d.nodes[t].target; }
		rep.check(allAlloc && allPinned, "clicked nodes end up allocated and pinned");
		rep.check((int)d.TargetIdx().size() == (int)tg.size(),
		          "no extra node was pinned", std::to_string(d.TargetIdx().size()));

		// Clicking a target again un-picks it and the tree shrinks back.
		click(tg[0]);
		rep.check(!d.nodes[tg[0]].target, "clicking a target un-picks it");
		rep.check(d.UsedPoints() <= afterAdds, "un-picking never grows the tree",
		          std::to_string(afterAdds) + " -> " + std::to_string(d.UsedPoints()));
		click(tg[0]);
		rep.check(d.UsedPoints() == afterAdds && d.nodes[tg[0]].target,
		          "re-picking it restores exactly the same point count",
		          std::to_string(d.UsedPoints()) + " vs " + std::to_string(afterAdds));

		// Clicking wiring must always cost at least one target. This is not a
		// sampled observation but a consequence of prune_redundant: it deletes
		// every non-target node whose removal keeps the set connected, so in any
		// plan a surviving non-target node is a cut vertex by construction.
		//
		// Asserting the invariant (rather than hunting for one example) also
		// settles what AtlasView's planNoop_ branch is for: it can only fire on
		// an allocation that is NOT minimal, so it is a guard, not a normal path.
		{
			std::vector<int> wiringNodes;
			for (int i = 0; i < (int)d.nodes.size(); i++)
				if (d.nodes[i].alloc && !d.nodes[i].target && d.nodes[i].kind != kAtlasStart)
					wiringNodes.push_back(i);
			rep.check(!wiringNodes.empty(), "the sampled plan actually has wiring to test",
			          std::to_string(wiringNodes.size()) + " wiring nodes");

			size_t cur = d.TargetIdx().size();
			int inert = 0;
			for (int w : wiringNodes)
				if (AtlasTargetsAfterClick(d, w).size() >= cur) inert++;
			rep.check(inert == 0, "every wiring node is a cut vertex (clicking it costs a target)",
			          std::to_string(wiringNodes.size()) + " checked, " + std::to_string(inert) + " inert");

			if (!wiringNodes.empty()) {
				size_t before = d.TargetIdx().size();
				click(wiringNodes.front());
				rep.check(d.TargetIdx().size() < before, "clicking wiring gives up the targets behind it",
				          std::to_string(before) + " -> " + std::to_string(d.TargetIdx().size()));
			}
		}

		d.Reset();
		rep.check(d.UsedPoints() == 0 && d.TargetIdx().empty(), "reset clears both");
	}

	// --- O12: inferred targets reproduce a legacy build ----------------------
	{
		std::mt19937 rng(97);
		std::vector<int> tg = pick(rng, 6);
		AtlasPlan p = AtlasPlanMinimal(d, tg, {}, {});
		d.Reset();
		d.Alloc(p.nodes);
		std::vector<int> inferred = AtlasInferTargets(d);
		bool coversAll = true;
		for (int t : tg) coversAll = coversAll && std::find(inferred.begin(), inferred.end(), t) != inferred.end();
		rep.check(coversAll, "inferred targets keep every notable of the old build",
		          std::to_string(inferred.size()) + " inferred from " + std::to_string(p.points) + " points");
		AtlasPlan again = AtlasPlanMinimal(d, inferred, {}, p.nodes);
		rep.check(again.points <= p.points, "re-solving an inferred build never costs more",
		          std::to_string(p.points) + " -> " + std::to_string(again.points));
	}
	d.Reset();

	rep.text += rep.failures == 0 ? "\nALL PASS\n"
	                              : "\n" + std::to_string(rep.failures) + " FAILURE(S)\n";
	printf("%s", rep.text.c_str());
	write_file_utf8(exeDir + L"atlas_opt_selftest.txt", rep.text);
	return rep.failures == 0 ? 0 : 1;
}
