#include "warehouse_ninja.h"

#include "http_client.h"
#include "warehouse_pricing.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp>

using nlohmann::ordered_json;

namespace {

constexpr wchar_t kNinjaHost[] = L"poe.ninja";

// type=All is empty on the exchange endpoint; these are fetched one by one.
const char* kExchangeTypes[] = { "Currency", "Fragment", "Scarab", "Oil",
	                             "Essence", "Fossil", "Resonator", "DeliriumOrb",
	                             "Tattoo", "Omen", "Runegraft" };

// Categories the stash-item endpoint prices that stacks in a stash are made of.
const char* kItemTypes[] = { "SkillGem", "DivinationCard", "Map", "UniqueMap",
	                         "UniqueWeapon", "UniqueArmour", "UniqueAccessory",
	                         "UniqueJewel", "UniqueFlask" };

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
	const std::wstring tmp = dst + L".tmp";
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

} // namespace

bool NinjaPriceSource::ParseExchangeOverview(
    const std::string& body, std::vector<std::pair<std::string, NinjaPrice>>* out,
    std::string* err, const char* keyPrefix)
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
			p.chaos = v;
			out->emplace_back(keyPrefix + nameIt->second, p);
		}
		return true;
	} catch (const std::exception&) {
		return fail(u8"exchange 回應格式異常");
	}
}

bool NinjaPriceSource::ParseItemOverview(
    const std::string& body, const std::string& type,
    std::vector<std::pair<std::string, NinjaPrice>>* out, std::string* err)
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
		auto jl = doc.find("lines");
		if (jl == doc.end() || !jl->is_array()) return fail(u8"item overview 回應缺 lines");

		for (const auto& l : *jl) {
			if (!l.is_object()) continue;
			std::string name = jstr(l, "name");
			double chaos = jnum(l, "chaosValue", 0.0);
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
				// UniqueWeapon / UniqueArmour / UniqueAccessory / UniqueJewel / UniqueFlask
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

void NinjaPriceSource::Init(const std::wstring& exeDir)
{
	exeDir_ = exeDir;
}

bool NinjaPriceSource::loadCache(const std::string& league)
{
	std::string body;
	if (!ReadAll(exeDir_ + L"PobTools\\cache\\ninja\\" + SanitizeForFile(league) + L".json",
	             body))
		return false;
	try {
		ordered_json doc = ordered_json::parse(body);
		// Exact schema match: bumping the number is how a coverage fix (new
		// sources, new key families) invalidates every stale cache at once --
		// otherwise the 15-minute TTL keeps serving the pre-fix price table.
		if (doc.value("schema", 1) != 2) return false;
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
		league_ = league;
		return !prices_.empty();
	} catch (const std::exception&) {
		return false;
	}
}

void NinjaPriceSource::saveCache(const std::string& league) const
{
	CreateDirectoryW((exeDir_ + L"PobTools").c_str(), nullptr);
	CreateDirectoryW((exeDir_ + L"PobTools\\cache").c_str(), nullptr);
	CreateDirectoryW((exeDir_ + L"PobTools\\cache\\ninja").c_str(), nullptr);
	ordered_json doc;
	doc["schema"] = 2;
	doc["league"] = league;
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
	WriteAtomic(exeDir_ + L"PobTools\\cache\\ninja\\" + SanitizeForFile(league) + L".json",
	            doc.dump());
}

bool NinjaPriceSource::Refresh(const std::string& league, bool force, std::string* err,
                               const std::atomic<bool>* cancel)
{
	if (league_ != league || prices_.empty()) {
		fetchedUtc_ = 0;
		divineRate_ = 0.0;
		prices_.clear();
		loadCache(league);
	}
	if (!force && CacheFresh(fetchedUtc_, NowUtc())) return true;

	HttpsClient http(kNinjaHost);
	std::unordered_map<std::string, NinjaPrice> fresh;
	int fetchedTypes = 0;
	auto polite = [&]() { Sleep(300); }; // it is a community resource

	const std::wstring leagueQ = UrlEncodeQuery(league);
	// (type, key namespace): divination cards trade on the exchange too, under
	// their own price-key family -- the item overview 404s for them.
	struct ExchangeFetch { const char* type; const char* prefix; };
	std::vector<ExchangeFetch> exchangeFetches;
	for (const char* type : kExchangeTypes) exchangeFetches.push_back({ type, "currency|" });
	exchangeFetches.push_back({ "DivinationCard", "card|" });
	for (const ExchangeFetch& ef : exchangeFetches) {
		if (cancel && cancel->load()) break;
		std::string body;
		std::string terr;
		std::wstring path = L"/poe1/api/economy/exchange/current/overview?league=" +
		                    leagueQ + L"&type=" + Widen(ef.type);
		if (http.GetString(path, body, &terr, cancel)) {
			std::vector<std::pair<std::string, NinjaPrice>> lines;
			if (ParseExchangeOverview(body, &lines, nullptr, ef.prefix)) {
				for (auto& kv : lines) fresh[kv.first] = kv.second;
				fetchedTypes++;
			}
		}
		polite();
	}
	// Gap-filler AFTER the exchange pass: stash-listed prices for whatever the
	// bulk market does not trade. Exchange keys win -- they are live trades.
	for (const char* type : { "Currency", "Fragment" }) {
		if (cancel && cancel->load()) break;
		std::string body;
		std::string terr;
		std::wstring path = L"/poe1/api/economy/stash/current/currency/overview?league=" +
		                    leagueQ + L"&type=" + Widen(type);
		if (http.GetString(path, body, &terr, cancel)) {
			std::vector<std::pair<std::string, NinjaPrice>> lines;
			if (ParseCurrencyOverview(body, &lines, nullptr)) {
				for (auto& kv : lines)
					if (fresh.find(kv.first) == fresh.end()) fresh[kv.first] = kv.second;
				fetchedTypes++;
			}
		}
		polite();
	}
	for (const char* type : kItemTypes) {
		if (cancel && cancel->load()) break;
		std::string body;
		std::string terr;
		std::wstring path = L"/poe1/api/economy/stash/current/item/overview?league=" +
		                    leagueQ + L"&type=" + Widen(type);
		if (http.GetString(path, body, &terr, cancel)) {
			std::vector<std::pair<std::string, NinjaPrice>> lines;
			if (ParseItemOverview(body, type, &lines, nullptr)) {
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

	if (fetchedTypes == 0) {
		// Nothing new; the stale cache (if any) stays usable.
		if (err) *err = u8"poe.ninja 無法連線或全部類別都失敗";
		return !prices_.empty();
	}

	prices_ = std::move(fresh);
	league_ = league;
	fetchedUtc_ = NowUtc();

	// Chaos Orb IS the unit every price is quoted in, so no source ever lists
	// it -- without this line a stack of chaos counts as 未估價.
	NinjaPrice chaosOrb;
	chaosOrb.chaos = 1.0;
	chaosOrb.listingCount = 999;
	prices_[NinjaExchangeKey("Chaos Orb")] = chaosOrb;

	NinjaPrice divine;
	divineRate_ = 0.0;
	if (PriceOf(NinjaExchangeKey("Divine Orb"), &divine) && divine.chaos >= 30.0)
		divineRate_ = divine.chaos;

	saveCache(league);
	return true;
}

bool NinjaPriceSource::PriceOf(const std::string& key, NinjaPrice* out) const
{
	auto it = prices_.find(key);
	if (it == prices_.end()) return false;
	*out = it->second;
	return true;
}

bool FetchNinjaLeagues(std::vector<std::string>& out, std::string* err,
                       const std::atomic<bool>* cancel)
{
	HttpsClient http(kNinjaHost);
	std::string body;
	if (!http.GetString(L"/poe1/api/economy/leagues", body, err, cancel)) return false;
	try {
		ordered_json doc = ordered_json::parse(body);
		if (!doc.is_array()) {
			if (err) *err = u8"leagues 回應不是陣列";
			return false;
		}
		out.clear();
		for (const auto& l : doc) {
			if (l.is_object()) {
				std::string id = l.value("id", l.value("name", std::string()));
				if (!id.empty()) out.push_back(std::move(id));
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
