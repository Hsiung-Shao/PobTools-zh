#include "boss_profit.h"

#include <json.hpp>

#include <set>

using nlohmann::json;

bool LoadBossDb(const std::string& text, BossDb* db, std::string* err)
{
	*db = BossDb();
	try {
		// .at()/.get<>() throw on a missing or mistyped field: one catch reports them all.
		const json j = json::parse(text);
		BossDb out;
		out.source = j.at("source").get<std::string>();
		std::set<std::string> seen;
		for (const json& jb : j.at("bosses")) {
			Boss b;
			b.key = jb.at("key").get<std::string>();
			b.en = jb.at("en").get<std::string>();
			b.zh = jb.value("zh", std::string());
			if (!seen.insert(b.key).second) throw std::runtime_error("duplicate boss key: " + b.key);
			for (const json& jv : jb.at("variants")) {
				BossVariant v;
				v.name = jv.at("name").get<std::string>();
				v.uber = jv.value("uber", false);
				for (const json& jt : jv.at("tickets")) {
					BossTicket t;
					t.priceKey = jt.at("priceKey").get<std::string>();
					t.en = jt.at("en").get<std::string>();
					t.qty = jt.at("qty").get<double>();
					if (!(t.qty > 0.0)) throw std::runtime_error("ticket qty <= 0: " + t.en);
					v.tickets.push_back(std::move(t));
				}
				for (const json& jd : jv.at("drops")) {
					BossDrop d;
					d.priceKey = jd.at("priceKey").get<std::string>();
					d.en = jd.at("en").get<std::string>();
					d.ratePct = jd.at("ratePct").get<double>();
					if (!(d.ratePct >= 0.0 && d.ratePct <= 100.0)) // 0 = rate unknown
						throw std::runtime_error("drop rate outside [0,100]: " + d.en);
					d.rateText = jd.value("rateText", std::string());
					d.note = jd.value("note", std::string());
					v.drops.push_back(std::move(d));
				}
				b.variants.push_back(std::move(v));
			}
			out.bosses.push_back(std::move(b));
		}
		*db = std::move(out);
		return true;
	} catch (const std::exception& e) {
		if (err) *err = e.what();
		return false;
	}
}

std::string BossVariantId(const Boss& b, const BossVariant& v) { return b.key + "|" + v.name; }

std::string SerializeBossOverrides(const BossOverrides& o)
{
	json j;
	j["price"] = o.price.empty() ? json::object() : json(o.price);
	j["rate"] = o.rate.empty() ? json::object() : json(o.rate);
	j["killSeconds"] = o.killSeconds.empty() ? json::object() : json(o.killSeconds);
	j["sort"] = o.sort;
	return j.dump(1);
}

bool ParseBossOverrides(const std::string& text, BossOverrides* o, std::string* err)
{
	try {
		const json j = json::parse(text);
		BossOverrides out;
		// Entry by entry: one bad value (a null left by an inf, a hand edit) must not void the rest.
		// (named locals: items() on the temporary value() returns would dangle)
		const json price = j.value("price", json::object()), rate = j.value("rate", json::object()),
		           kill = j.value("killSeconds", json::object());
		for (const auto& kv : price.items())
			if (kv.value().is_number()) out.price[kv.key()] = kv.value().get<double>();
		for (const auto& kv : rate.items())
			if (kv.value().is_number()) out.rate[kv.key()] = kv.value().get<double>();
		for (const auto& kv : kill.items())
			if (kv.value().is_number_integer()) out.killSeconds[kv.key()] = kv.value().get<int>();
		const json sort = j.value("sort", json::array());
		for (const json& e : sort) {
			if (!e.is_number_integer()) continue;
			const int c = e.get<int>();
			if (c != 0 && c >= -kBossSortCols && c <= kBossSortCols) out.sort.push_back(c);
		}
		*o = std::move(out);
		return true;
	} catch (const std::exception& e) {
		if (err) *err = e.what();
		return false;
	}
}

void ResetBossVariantOverrides(BossOverrides* o, const Boss& b, const BossVariant& v)
{
	const std::string vid = BossVariantId(b, v);
	o->killSeconds.erase(vid);
	for (const BossDrop& d : v.drops) {
		o->rate.erase(vid + "|" + d.en); // exact keys: "Boss|Regular2|..." must survive
		o->price.erase(d.priceKey);
	}
	for (const BossTicket& t : v.tickets) o->price.erase(t.priceKey);
}

namespace {
// Manual price first, then a confident market price, else unpriced.
BossPriceSrc UnitPrice(const std::string& key,
                       const std::function<bool(const std::string&, NinjaPrice*)>& lookup,
                       const BossOverrides& o, double* unit)
{
	*unit = 0.0;
	auto m = o.price.find(key);
	if (m != o.price.end()) {
		*unit = m->second;
		return BossPriceSrc::Manual;
	}
	NinjaPrice p;
	if (lookup && lookup(key, &p) && !p.lowConfidence) {
		*unit = p.chaos;
		return BossPriceSrc::Market;
	}
	return BossPriceSrc::None;
}
} // namespace

BossResult ComputeBossVariant(const Boss& b, const BossVariant& v,
                              const std::function<bool(const std::string&, NinjaPrice*)>& lookup,
                              const BossOverrides& o)
{
	BossResult r;
	const std::string vid = BossVariantId(b, v);
	for (const BossDrop& d : v.drops) {
		BossLine l;
		l.priceKey = d.priceKey;
		l.en = d.en;
		l.ratePct = d.ratePct;
		auto ro = o.rate.find(vid + "|" + d.en);
		if (ro != o.rate.end()) {
			l.ratePct = ro->second;
			l.rateManual = true;
		}
		l.src = UnitPrice(d.priceKey, lookup, o, &l.unit);
		// a rate nobody knows adds nothing either way, so its missing price is not a gap in the sum
		if (l.src == BossPriceSrc::None && l.ratePct > 0.0) r.unpricedDrops++;
		l.value = l.ratePct / 100.0 * l.unit;
		r.revenue += l.value;
		r.drops.push_back(std::move(l));
	}
	for (const BossTicket& t : v.tickets) {
		BossLine l;
		l.priceKey = t.priceKey;
		l.en = t.en;
		l.qty = t.qty;
		l.src = UnitPrice(t.priceKey, lookup, o, &l.unit);
		if (l.src == BossPriceSrc::None) r.ticketIncomplete = true;
		l.value = l.qty * l.unit;
		r.ticketCost += l.value;
		r.tickets.push_back(std::move(l));
	}
	r.hasTicket = !v.tickets.empty();
	r.profit = r.revenue - r.ticketCost;
	r.hasRoi = r.hasTicket && !r.ticketIncomplete && r.ticketCost > 0.0;
	if (r.hasRoi) r.roiPct = r.profit / r.ticketCost * 100.0;
	auto ks = o.killSeconds.find(vid);
	r.hasPerHour = ks != o.killSeconds.end() && ks->second > 0;
	if (r.hasPerHour) r.profitPerHour = r.profit * 3600.0 / ks->second;
	return r;
}
