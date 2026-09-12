#include "atlas_cost.h"

#include "warehouse_pricing.h" // NinjaExchangeKey

#include <algorithm>
#include <cmath> // std::round

MapCostSummary ComputeMapCost(const std::vector<MapCostInput>& slots, double manualMapChaos,
                              const std::function<bool(const std::string&, NinjaPrice*)>& lookup,
                              const std::map<std::string, double>* basis)
{
	MapCostSummary s;

	// The device list repeats an entry once per copy placed; fold to one line
	// per item, keeping the order the player socketed them in.
	for (const MapCostInput& in : slots) {
		auto it = std::find_if(s.lines.begin(), s.lines.end(),
		                       [&](const MapCostLine& l) { return l.id == in.id; });
		if (it != s.lines.end()) {
			it->qty++;
			continue;
		}
		MapCostLine l;
		l.id = in.id;
		l.en = in.en;
		l.zh = in.zh;
		l.art = in.art;
		l.qty = 1;
		l.tradable = in.tradable;
		s.lines.push_back(std::move(l));
	}

	for (MapCostLine& l : s.lines) {
		// The market price is looked up either way: it is the reference shown
		// beside a recorded price, and the fallback without one.
		NinjaPrice p;
		if (l.tradable && lookup && !l.en.empty() && lookup(NinjaExchangeKey(l.en), &p) &&
		    !p.lowConfidence && p.chaos > 0)
			l.marketEach = p.chaos;

		double paid = 0.0;
		if (basis) {
			auto it = basis->find(l.id);
			if (it != basis->end() && it->second > 0.0) paid = it->second;
		}
		if (paid > 0.0) {
			l.chaosEach = paid;
			l.fromBasis = true;
		} else {
			l.chaosEach = l.marketEach;
		}

		if (l.chaosEach > 0.0) {
			l.chaosTotal = l.chaosEach * (double)l.qty;
			l.priced = true;
			s.totalChaos += l.chaosTotal;
		} else if (!l.tradable) {
			s.untradableKinds++;
		} else {
			s.unpricedKinds++;
		}
	}

	if (manualMapChaos > 0) {
		s.mapChaos = manualMapChaos;
		s.mapIncluded = true;
		s.totalChaos += manualMapChaos;
	}
	return s;
}

AtlasProfitRecord MakeProfitRecord(const Snapshot& from, const Snapshot& to,
                                   const std::function<std::string(const std::string&)>& zhOf,
                                   double costPerMap)
{
	AtlasProfitRecord r;
	if (from.summary || to.summary || to.utc <= from.utc) return r;
	const SnapshotDiff d = DiffSnapshots(from, to);
	auto cents = [](double v) { return std::round(v * 100.0) / 100.0; };
	r.fromUtc = from.utc;
	r.toUtc = to.utc;
	r.league = to.league;
	r.hours = cents(d.hours);
	r.qtyGain = cents(d.qtyGain);
	r.qtyLoss = cents(d.qtyLoss);
	r.priceMove = cents(d.priceMove);
	r.net = cents(d.dTotalChaos);
	r.divineRate = cents(to.divineRate);
	r.costPerMap = costPerMap > 0.0 ? cents(costPerMap) : 0.0;

	// What the stretch PRODUCED: gains by the value of the count change. A line
	// can sit in `lost` and still have produced (its price fell by more).
	std::vector<const SnapshotDiffLine*> gains;
	for (const SnapshotDiffLine& l : d.gained)
		if (l.dQtyChaos > 0) gains.push_back(&l);
	for (const SnapshotDiffLine& l : d.lost)
		if (l.dQtyChaos > 0) gains.push_back(&l);
	std::stable_sort(gains.begin(), gains.end(),
	                 [](const SnapshotDiffLine* x, const SnapshotDiffLine* y) {
		                 if (x->dQtyChaos != y->dQtyChaos) return x->dQtyChaos > y->dQtyChaos;
		                 return x->key < y->key;
	                 });
	for (size_t i = 0; i < gains.size() && i < 10; i++) {
		AtlasProfitItem it;
		it.en = gains[i]->dispEn.empty() ? gains[i]->key : gains[i]->dispEn;
		it.zh = zhOf ? zhOf(it.en) : std::string();
		it.dCount = gains[i]->dCount;
		it.chaos = cents(gains[i]->dQtyChaos);
		r.top.push_back(std::move(it));
	}
	return r;
}
