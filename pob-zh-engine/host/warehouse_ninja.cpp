#include "warehouse_ninja.h"

#include "http_client.h"
#include "warehouse_pricing.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp>

using nlohmann::ordered_json;

namespace {

constexpr wchar_t kNinjaHost[] = L"poe.ninja";

// One exchange type to fetch, and the price-key namespace its lines file under.
struct TypePlan {
	const char* type;
	const char* prefix;
};

// PoE1 exchange: stackables, plus divination cards (their item overview 404s).
const TypePlan kPoe1Exchange[] = {
	{ "Currency", "currency|" },  { "Fragment", "currency|" },    { "Scarab", "currency|" },
	{ "Oil", "currency|" },       { "Essence", "currency|" },     { "Fossil", "currency|" },
	{ "Resonator", "currency|" }, { "DeliriumOrb", "currency|" }, { "Tattoo", "currency|" },
	{ "Omen", "currency|" },      { "Runegraft", "currency|" },   { "DivinationCard", "card|" },
};

// PoE2: the economy IS the in-game Currency Exchange. Names are the site's own
// request parameters, not its URL slugs (read off its requests, 2026-09-11).
const TypePlan kPoe2Exchange[] = {
	{ "Currency", "currency|" },   { "Fragments", "currency|" },
	{ "Abyss", "currency|" },      { "UncutGems", "currency|" },
	{ "LineageSupportGems", "currency|" },
	{ "Essences", "currency|" },   { "SoulCores", "currency|" },
	{ "Idols", "currency|" },      { "Runes", "currency|" },
	{ "Ritual", "currency|" },     { "Expedition", "currency|" },
	{ "Delirium", "currency|" },   { "Breach", "currency|" },
	{ "Verisium", "currency|" },
};

const char* const kPoe1ItemTypes[] = { "SkillGem", "Map", "UniqueMap", "UniqueWeapon",
	                                   "UniqueArmour", "UniqueAccessory", "UniqueJewel",
	                                   "UniqueFlask" };
// PoE2 spells these in the plural (and UniqueRelics 404s).
const char* const kPoe2ItemTypes[] = { "UniqueWeapons", "UniqueArmours", "UniqueAccessories",
	                                   "UniqueFlasks",  "UniqueCharms",  "UniqueJewels",
	                                   "UniqueTablets" };

constexpr int kLowConfidenceMinCount = 5;

std::wstring Widen(const std::string& utf8)
{
	if (utf8.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], n);
	return w;
}

std::wstring UrlEncodeQuery(const std::string& utf8)
{
	static const wchar_t hex[] = L"0123456789ABCDEF";
	std::wstring out;
	for (unsigned char c : utf8) {
		const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		                  (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		                  c == '.' || c == '~';
		if (keep) {
			out += (wchar_t)c;
		} else {
			out += L'%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

// League ids become cache file names; keep only bytes every filesystem takes.
std::wstring SanitizeForFile(const std::string& s)
{
	std::wstring out;
	for (unsigned char c : s) {
		const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		                  (c >= '0' && c <= '9') || c == '-' || c == '_';
		out += keep ? (wchar_t)c : L'_';
	}
	return out.empty() ? L"league" : out;
}

std::wstring CachePath(const std::wstring& exeDir, const std::string& game,
                       const std::string& league)
{
	return exeDir + L"PobTools\\cache\\ninja\\" + Widen(game) + L"_" +
	       SanitizeForFile(league) + L".json";
}

bool ReadAll(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                       OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 26)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() ||
		     (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

bool WriteAtomic(const std::wstring& dst, const std::string& body)
{
	// Unique per writer: two price sources (the snapshot worker and the atlas
	// cost card's feed, possibly in two processes) can save the same cache file
	// at once, and a shared ".tmp" would let one truncate the other's half-write.
	const std::wstring tmp = dst + L".tmp" + std::to_wstring(GetCurrentProcessId()) + L"_" +
	                         std::to_wstring(GetCurrentThreadId());
	HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
	                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	DWORD wrote = 0;
	const bool ok = WriteFile(f, body.data(), (DWORD)body.size(), &wrote, nullptr) &&
	                wrote == body.size();
	CloseHandle(f);
	if (!ok) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	if (!MoveFileExW(tmp.c_str(), dst.c_str(),
	                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	return true;
}

long long NowUtc()
{
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	// FILETIME epoch (1601) -> unix epoch (1970), 100ns -> s
	return (long long)((t - 116444736000000000ull) / 10000000ull);
}

double RateOf(const ordered_json& rates, const char* unit)
{
	auto it = rates.find(unit);
	return it != rates.end() && it->is_number() ? it->get<double>() : 0.0;
}

// How many chaos one unit of this response's quotes is worth. poe.ninja quotes
// in core.primary (chaos on PoE1, divine on PoE2); core.rates says how many of
// each other unit one primary buys. No core at all is the PoE1 shape from
// before core existed: chaos. Returns 0 when the quote cannot be converted.
// chaosPerDivine (optional) receives the rate the core states, if any.
double ChaosFactor(const ordered_json& doc, double* chaosPerDivine)
{
	auto jc = doc.find("core");
	if (jc == doc.end() || !jc->is_object()) return 1.0;
	std::string primary;
	auto jp = jc->find("primary");
	if (jp != jc->end() && jp->is_string()) primary = jp->get<std::string>();
	double rChaos = 0.0, rDivine = 0.0;
	auto jr = jc->find("rates");
	if (jr != jc->end() && jr->is_object()) {
		rChaos = RateOf(*jr, "chaos");
		rDivine = RateOf(*jr, "divine");
	}
	if (primary.empty() || primary == "chaos") {
		if (chaosPerDivine && rDivine > 0) *chaosPerDivine = 1.0 / rDivine;
		return 1.0;
	}
	if (primary == "divine") {
		if (rChaos <= 0) return 0.0;
		if (chaosPerDivine) *chaosPerDivine = rChaos;
		return rChaos;
	}
	// Some other primary: convertible only through its chaos rate.
	if (rChaos <= 0) return 0.0;
	if (chaosPerDivine && rDivine > 0) *chaosPerDivine = rChaos / rDivine;
	return rChaos;
}

} // namespace

bool NinjaPriceSource::ParseExchangeOverview(
    const std::string& body, std::vector<std::pair<std::string, NinjaPrice>>* out,
    std::string* err, const char* keyPrefix, double* chaosPerDivine)
{
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};
	// The id is a NUMBER for some types and a SLUG STRING for others
	// (currency: 1; divination cards: "abandoned-wealth") -- normalised to a
	// string so the items/lines join works for both.
	auto idOf = [](const ordered_json& o) -> std::string {
		auto it = o.find("id");
		if (it == o.end()) return std::string();
		if (it->is_string()) return it->get<std::string>();
		if (it->is_number_integer()) return std::to_string(it->get<long long>());
		return std::string();
	};
	// The WHOLE walk sits inside the try: value(key, default) throws on a
	// present-but-null field (type_error.302), and poe.ninja does emit nulls.
	// An exception escaping here would cross the worker thread and terminate
	// the process -- which is exactly what it did once.
	try {
		ordered_json doc = ordered_json::parse(body);
		if (!doc.is_object()) return fail(u8"exchange 回應不是物件");
		const double factor = ChaosFactor(doc, chaosPerDivine);
		if (factor <= 0) return fail(u8"exchange 回應的計價單位無法換算成混沌石");

		// items[] names the goods, lines[] prices them; joined on id.
		std::unordered_map<std::string, std::string> nameById;
		auto ji = doc.find("items");
		if (ji != doc.end() && ji->is_array()) {
			for (const auto& it : *ji) {
				if (!it.is_object()) continue;
				std::string id = idOf(it);
				auto jn = it.find("name");
				if (id.empty() || jn == it.end() || !jn->is_string()) continue;
				std::string name = jn->get<std::string>();
				if (!name.empty()) nameById[id] = std::move(name);
			}
		}
		auto jl = doc.find("lines");
		if (jl == doc.end() || !jl->is_array()) return fail(u8"exchange 回應缺 lines");
		for (const auto& l : *jl) {
			if (!l.is_object()) continue;
			std::string id = idOf(l);
			auto jv = l.find("primaryValue");
			if (id.empty() || jv == l.end() || !jv->is_number()) continue;
			double v = jv->get<double>();
			auto nameIt = nameById.find(id);
			if (nameIt == nameById.end() || v <= 0) continue;
			NinjaPrice p;
			p.chaos = v * factor;
			out->emplace_back(keyPrefix + nameIt->second, p);
		}
		return true;
	} catch (const std::exception&) {
		return fail(u8"exchange 回應格式異常");
	}
}

bool NinjaPriceSource::ParseItemOverview(
    const std::string& body, const std::string& type,
    std::vector<std::pair<std::string, NinjaPrice>>* out, std::string* err,
    double* chaosPerDivine)
{
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};
	// Null-tolerant field reads: value(key, default) throws on an explicit null,
	// and several overview fields (baseType, gem details, counts) arrive as null.
	auto jstr = [](const ordered_json& o, const char* k) -> std::string {
		auto it = o.find(k);
		return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
	};
	auto jnum = [](const ordered_json& o, const char* k, double def) -> double {
		auto it = o.find(k);
		return it != o.end() && it->is_number() ? it->get<double>() : def;
	};
	auto jbool = [](const ordered_json& o, const char* k) -> bool {
		auto it = o.find(k);
		return it != o.end() && it->is_boolean() && it->get<bool>();
	};

	// Everything inside the try: an exception must degrade to "this type is
	// skipped", never cross the worker thread (that terminates the process).
	try {
		ordered_json doc = ordered_json::parse(body);
		if (!doc.is_object()) return fail(u8"item overview 回應不是物件");
		const double factor = ChaosFactor(doc, chaosPerDivine);
		auto jl = doc.find("lines");
		if (jl == doc.end() || !jl->is_array()) return fail(u8"item overview 回應缺 lines");

		for (const auto& l : *jl) {
			if (!l.is_object()) continue;
			std::string name = jstr(l, "name");
			// PoE1 lines carry chaosValue; PoE2 lines only primaryValue, in the
			// core's primary currency (divine).
			double chaos = jnum(l, "chaosValue", 0.0);
			if (chaos <= 0 && factor > 0) chaos = jnum(l, "primaryValue", 0.0) * factor;
			if (name.empty() || chaos <= 0) continue;
			int count = (int)jnum(l, "count", jnum(l, "listingCount", 0.0));

			std::string key;
			if (type == "SkillGem") {
				key = NinjaGemKey(name, (int)jnum(l, "gemLevel", 1),
				                  (int)jnum(l, "gemQuality", 0), jbool(l, "corrupted"));
			} else if (type == "DivinationCard") {
				key = NinjaCardKey(name);
			} else if (type == "Map") {
				int tier = (int)jnum(l, "mapTier", 0);
				if (tier <= 0) continue;
				key = NinjaMapKey(name, tier);
			} else if (type == "UniqueMap") {
				key = NinjaUniqueKey(name, jstr(l, "baseType"), 0);
			} else {
				// PoE1 UniqueWeapon/... and PoE2 UniqueWeapons/...
				key = NinjaUniqueKey(name, jstr(l, "baseType"), (int)jnum(l, "links", 0));
			}

			NinjaPrice p;
			p.chaos = chaos;
			p.listingCount = count;
			p.lowConfidence = count > 0 && count < kLowConfidenceMinCount;
			out->emplace_back(std::move(key), p);
		}
		return true;
	} catch (const std::exception&) {
		return fail(u8"item overview 回應格式異常");
	}
}

bool NinjaPriceSource::ParseCurrencyOverview(
    const std::string& body, std::vector<std::pair<std::string, NinjaPrice>>* out,
    std::string* err)
{
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};
	try {
		ordered_json doc = ordered_json::parse(body);
		if (!doc.is_object()) return fail(u8"currency overview 回應不是物件");
		auto jl = doc.find("lines");
		if (jl == doc.end() || !jl->is_array()) return fail(u8"currency overview 回應缺 lines");
		for (const auto& l : *jl) {
			if (!l.is_object()) continue;
			auto jn = l.find("currencyTypeName");
			auto jv = l.find("chaosEquivalent");
			if (jn == l.end() || !jn->is_string() || jv == l.end() || !jv->is_number())
				continue;
			double chaos = jv->get<double>();
			if (chaos <= 0) continue;
			int count = 0;
			auto jr = l.find("receive");
			if (jr != l.end() && jr->is_object()) {
				auto jc = jr->find("count");
				if (jc != jr->end() && jc->is_number()) count = (int)jc->get<double>();
			}
			NinjaPrice p;
			p.chaos = chaos;
			p.listingCount = count;
			p.lowConfidence = count > 0 && count < kLowConfidenceMinCount;
			out->emplace_back(NinjaExchangeKey(jn->get<std::string>()), p);
		}
		return true;
	} catch (const std::exception&) {
		return fail(u8"currency overview 回應格式異常");
	}
}

static long long UnixOf(const FILETIME& ft)
{
	ULARGE_INTEGER t;
	t.LowPart = ft.dwLowDateTime;
	t.HighPart = ft.dwHighDateTime;
	const unsigned long long kEpoch = 116444736000000000ull; // 1601 -> 1970, in 100ns
	return t.QuadPart > kEpoch ? (long long)((t.QuadPart - kEpoch) / 10000000ull) : 0;
}

int PruneNinjaCache(const std::wstring& exeDir, long long nowUtc, int maxAgeDays)
{
	const std::wstring dir = exeDir + L"PobTools\\cache\\ninja\\";
	WIN32_FIND_DATAW fd{};
	HANDLE f = FindFirstFileW((dir + L"poe?_*").c_str(), &fd);
	if (f == INVALID_HANDLE_VALUE) return 0;
	std::vector<std::wstring> victims;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		// Only names this cache writes (the wildcard also matches 8.3 aliases).
		const std::wstring name = fd.cFileName;
		if (name.compare(0, 5, L"poe1_") != 0 && name.compare(0, 5, L"poe2_") != 0) continue;
		const bool isJson = name.size() > 5 && name.compare(name.size() - 5, 5, L".json") == 0;
		const bool isTmp = name.find(L".json.tmp") != std::wstring::npos;
		if (!isJson && !isTmp) continue;
		if (nowUtc - UnixOf(fd.ftLastWriteTime) > (long long)maxAgeDays * 86400)
			victims.push_back(dir + name);
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	int removed = 0;
	for (const std::wstring& p : victims)
		if (DeleteFileW(p.c_str())) removed++;
	return removed;
}

void NinjaPriceSource::Init(const std::wstring& exeDir)
{
	exeDir_ = exeDir;
	// A directory listing, no network: allowed in a panel's Init.
	FILETIME now{};
	GetSystemTimeAsFileTime(&now);
	PruneNinjaCache(exeDir, UnixOf(now));
}

bool NinjaPriceSource::loadCache(const std::string& game, const std::string& league)
{
	std::string body;
	if (!ReadAll(CachePath(exeDir_, game, league), body)) return false;
	try {
		ordered_json doc = ordered_json::parse(body);
		// Exact schema match: bumping the number is how a coverage or unit fix
		// invalidates every stale cache at once -- otherwise the 15-minute TTL
		// keeps serving the pre-fix price table.
		if (doc.value("schema", 1) != 3) return false;
		if (doc.value("game", std::string()) != game) return false;
		if (doc.value("league", std::string()) != league) return false;
		fetchedUtc_ = doc.value("fetchedUtc", (long long)0);
		divineRate_ = doc.value("divineRate", 0.0);
		prices_.clear();
		auto jp = doc.find("prices");
		if (jp != doc.end() && jp->is_object()) {
			for (auto it = jp->begin(); it != jp->end(); ++it) {
				if (!it.value().is_object()) continue;
				NinjaPrice p;
				p.chaos = it.value().value("c", 0.0);
				p.listingCount = it.value().value("n", 0);
				p.lowConfidence = it.value().value("lc", false);
				prices_[it.key()] = p;
			}
		}
		return !prices_.empty();
	} catch (const std::exception&) {
		return false;
	}
}

void NinjaPriceSource::saveCache() const
{
	CreateDirectoryW((exeDir_ + L"PobTools").c_str(), nullptr);
	CreateDirectoryW((exeDir_ + L"PobTools\\cache").c_str(), nullptr);
	CreateDirectoryW((exeDir_ + L"PobTools\\cache\\ninja").c_str(), nullptr);
	ordered_json doc;
	doc["schema"] = 3;
	doc["game"] = game_;
	doc["league"] = league_;
	doc["fetchedUtc"] = fetchedUtc_;
	doc["divineRate"] = divineRate_;
	ordered_json jp = ordered_json::object();
	for (const auto& kv : prices_) {
		ordered_json p;
		p["c"] = kv.second.chaos;
		p["n"] = kv.second.listingCount;
		p["lc"] = kv.second.lowConfidence;
		jp[kv.first] = std::move(p);
	}
	doc["prices"] = std::move(jp);
	WriteAtomic(CachePath(exeDir_, game_, league_), doc.dump());
}

bool NinjaPriceSource::Refresh(const std::string& gameIn, const std::string& league,
                               bool force, std::string* err,
                               const std::atomic<bool>* cancel)
{
	const std::string game = gameIn == "poe2" ? "poe2" : "poe1";
	if (game_ != game || league_ != league || prices_.empty()) {
		fetchedUtc_ = 0;
		divineRate_ = 0.0;
		prices_.clear();
		game_ = game;
		league_ = league;
		loadCache(game, league);
	}
	if (!force && CacheFresh(fetchedUtc_, NowUtc())) return true;

	HttpsClient http(kNinjaHost);
	std::unordered_map<std::string, NinjaPrice> fresh;
	double coreDivine = 0.0;
	int fetchedTypes = 0;
	auto polite = [&]() { Sleep(300); }; // it is a community resource

	const std::wstring base = L"/" + Widen(game) + L"/api/economy/";
	const std::wstring leagueQ = UrlEncodeQuery(league);

	auto runExchange = [&](const TypePlan* plans, size_t n) {
		for (size_t i = 0; i < n; i++) {
			if (cancel && cancel->load()) return;
			std::string body, terr;
			if (http.GetString(base + L"exchange/current/overview?league=" + leagueQ +
			                       L"&type=" + Widen(plans[i].type),
			                   body, &terr, cancel)) {
				std::vector<std::pair<std::string, NinjaPrice>> lines;
				double cpd = 0.0;
				if (ParseExchangeOverview(body, &lines, nullptr, plans[i].prefix, &cpd)) {
					for (auto& kv : lines) fresh[kv.first] = kv.second;
					if (coreDivine <= 0 && cpd > 0) coreDivine = cpd;
					fetchedTypes++;
				}
			}
			polite();
		}
	};
	auto runItems = [&](const char* const* types, size_t n) {
		for (size_t i = 0; i < n; i++) {
			if (cancel && cancel->load()) return;
			std::string body, terr;
			if (http.GetString(base + L"stash/current/item/overview?league=" + leagueQ +
			                       L"&type=" + Widen(types[i]),
			                   body, &terr, cancel)) {
				std::vector<std::pair<std::string, NinjaPrice>> lines;
				if (ParseItemOverview(body, types[i], &lines, nullptr)) {
					// Keys can repeat (gem variants collapse into one bucket); the
					// best-evidenced price wins. Exchange prices are never displaced.
					for (auto& kv : lines) {
						auto it = fresh.find(kv.first);
						if (it == fresh.end() || kv.second.listingCount > it->second.listingCount)
							fresh[kv.first] = kv.second;
					}
					fetchedTypes++;
				}
			}
			polite();
		}
	};

	if (game == "poe2") {
		runExchange(kPoe2Exchange, sizeof(kPoe2Exchange) / sizeof(kPoe2Exchange[0]));
		runItems(kPoe2ItemTypes, sizeof(kPoe2ItemTypes) / sizeof(kPoe2ItemTypes[0]));
	} else {
		runExchange(kPoe1Exchange, sizeof(kPoe1Exchange) / sizeof(kPoe1Exchange[0]));
		// Gap-filler AFTER the exchange pass: stash-listed prices for whatever
		// the bulk market does not trade. Exchange keys win -- they are live trades.
		for (const char* type : { "Currency", "Fragment" }) {
			if (cancel && cancel->load()) break;
			std::string body, terr;
			if (http.GetString(base + L"stash/current/currency/overview?league=" + leagueQ +
			                       L"&type=" + Widen(type),
			                   body, &terr, cancel)) {
				std::vector<std::pair<std::string, NinjaPrice>> lines;
				if (ParseCurrencyOverview(body, &lines, nullptr)) {
					for (auto& kv : lines)
						if (fresh.find(kv.first) == fresh.end()) fresh[kv.first] = kv.second;
					fetchedTypes++;
				}
			}
			polite();
		}
		runItems(kPoe1ItemTypes, sizeof(kPoe1ItemTypes) / sizeof(kPoe1ItemTypes[0]));
	}

	if (fetchedTypes == 0) {
		// Nothing new; the stale cache (if any) stays usable.
		if (err) *err = u8"poe.ninja 無法連線或全部類別都失敗";
		return !prices_.empty();
	}

	prices_ = std::move(fresh);
	fetchedUtc_ = NowUtc();

	// Chaos Orb IS the unit every price was converted into: 1 by definition,
	// and no PoE1 source ever lists it.
	NinjaPrice chaosOrb;
	chaosOrb.chaos = 1.0;
	chaosOrb.listingCount = 999;
	prices_[NinjaExchangeKey("Chaos Orb")] = chaosOrb;

	// Chaos per divine: the rate the responses' core states is authoritative.
	// Fallback for a PoE1 response without core: the Divine Orb line, believed
	// only above 30c (a PoE2 divine is ~11 chaos, so that floor is PoE1-only).
	divineRate_ = coreDivine;
	if (divineRate_ <= 0 && game == "poe1") {
		NinjaPrice divine;
		if (PriceOf(NinjaExchangeKey("Divine Orb"), &divine) && divine.chaos >= 30.0)
			divineRate_ = divine.chaos;
	}

	saveCache();
	return true;
}

bool NinjaPriceSource::PriceOf(const std::string& key, NinjaPrice* out) const
{
	auto it = prices_.find(key);
	if (it == prices_.end()) return false;
	*out = it->second;
	return true;
}

bool FetchNinjaLeagues(const std::string& gameIn, std::vector<std::string>& out,
                       std::string* err, const std::atomic<bool>* cancel)
{
	const std::string game = gameIn == "poe2" ? "poe2" : "poe1";
	HttpsClient http(kNinjaHost);
	std::string body;
	if (!http.GetString(L"/" + Widen(game) + L"/api/economy/leagues", body, err, cancel))
		return false;
	try {
		ordered_json doc = ordered_json::parse(body);
		if (!doc.is_array()) {
			if (err) *err = u8"leagues 回應不是陣列";
			return false;
		}
		out.clear();
		for (const auto& l : doc) {
			if (l.is_object()) {
				auto jid = l.find("id");
				auto jn = l.find("name");
				if (jid != l.end() && jid->is_string()) out.push_back(jid->get<std::string>());
				else if (jn != l.end() && jn->is_string()) out.push_back(jn->get<std::string>());
			} else if (l.is_string()) {
				out.push_back(l.get<std::string>());
			}
		}
		return !out.empty();
	} catch (const std::exception&) {
		if (err) *err = u8"leagues 回應不是有效的 JSON";
		return false;
	}
}
