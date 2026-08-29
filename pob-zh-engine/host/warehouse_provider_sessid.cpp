// 倉庫收益統計 — the POESESSID (test-channel) stash provider.
//
// Talks to the legacy character-window endpoints on www.pathofexile.com with the
// user's own session cookie. This channel exists to prove the feature out before
// the official OAuth application lands; nothing above IStashProvider knows which
// channel served it.
//
// The session id is an account credential. It goes into exactly one place -- the
// Cookie request header -- and never into an error string, a log line or a
// report file.
#include "warehouse_provider.h"

#include "http_client.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp>

#include <climits>

using nlohmann::ordered_json;

namespace {

constexpr wchar_t kHost[] = L"www.pathofexile.com";

// RFC 3986 unreserved set; everything else (spaces, CJK bytes) is escaped the
// way the site itself does it. Account names and league names both need this.
std::wstring UrlEncode(const std::string& utf8)
{
	static const wchar_t hex[] = L"0123456789ABCDEF";
	std::wstring out;
	out.reserve(utf8.size() * 3);
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

int ToInt(const std::string& s, int fallback = 0)
{
	int v = 0;
	bool any = false, neg = false;
	size_t i = 0;
	if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; i++; }
	for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) {
		v = v * 10 + (s[i] - '0');
		any = true;
	}
	if (!any) return fallback;
	return neg ? -v : v;
}

// Legacy item names arrive wrapped in render markup ("<<set:MS>><<set:M>>Name").
// Only the text after the last ">>" is the name.
std::string StripNameMarkup(const std::string& s)
{
	size_t pos = s.rfind(">>");
	return pos == std::string::npos ? s : s.substr(pos + 2);
}

// properties[] carries display strings; the number is the leading digits of the
// first value ("20 (Max)" -> 20, "+13%" -> 13).
int PropertyNumber(const ordered_json& prop)
{
	auto vals = prop.find("values");
	if (vals == prop.end() || !vals->is_array() || vals->empty()) return 0;
	const ordered_json& first = (*vals)[0];
	if (!first.is_array() || first.empty() || !first[0].is_string()) return 0;
	const std::string text = first[0].get<std::string>();
	size_t i = 0;
	while (i < text.size() && (text[i] == '+' || text[i] == ' ')) i++;
	return ToInt(text.substr(i));
}

} // namespace

bool ParseStashTabJson(const std::string& body, int tabIndex,
                       std::vector<StashItemRaw>* items,
                       std::vector<StashTabInfo>* tabs, std::string* err)
{
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};
	// The whole walk lives inside one try: value(key, default) throws on a
	// present-but-null field, and an exception escaping a worker thread kills
	// the process. Anything unexpected degrades to "parse failed".
	try {
	ordered_json doc = ordered_json::parse(body);
	if (!doc.is_object()) return fail(u8"回應不是 JSON 物件");
	// GGG's own refusals come back as 200 with {"error":{...}} -- surface the
	// shape without copying the message (it is not ours and not localised).
	if (doc.find("error") != doc.end()) return fail(u8"API 回應帶有錯誤");

	if (tabs) {
		tabs->clear();
		auto jt = doc.find("tabs");
		if (jt != doc.end() && jt->is_array()) {
			for (const auto& t : *jt) {
				if (!t.is_object()) continue;
				StashTabInfo info;
				info.index = t.value("i", 0);
				info.id = t.value("id", std::string());
				info.name = t.value("n", std::string());
				info.type = t.value("type", std::string());
				auto col = t.find("colour");
				if (col != t.end() && col->is_object()) {
					info.colour = ((unsigned)col->value("r", 0) << 16) |
					              ((unsigned)col->value("g", 0) << 8) |
					              (unsigned)col->value("b", 0);
				}
				tabs->push_back(std::move(info));
			}
		}
	}

	if (items) {
		items->clear();
		auto ji = doc.find("items");
		if (ji != doc.end() && ji->is_array()) {
			for (const auto& it : *ji) {
				if (!it.is_object()) continue;
				// One malformed item (a null where a value should be) skips that
				// item, not the whole tab.
				try {
				StashItemRaw raw;
				raw.name = StripNameMarkup(it.value("name", std::string()));
				raw.typeLine = StripNameMarkup(it.value("typeLine", std::string()));
				raw.baseType = it.value("baseType", std::string());
				if (raw.baseType.empty()) raw.baseType = raw.typeLine;
				raw.icon = it.value("icon", std::string());
				raw.frameType = it.value("frameType", 0);
				raw.stackSize = it.value("stackSize", (long long)1);
				if (raw.stackSize < 1) raw.stackSize = 1;
				raw.ilvl = it.value("ilvl", 0);
				raw.corrupted = it.value("corrupted", false);
				raw.tabIndex = tabIndex;

				auto props = it.find("properties");
				if (props != it.end() && props->is_array()) {
					for (const auto& p : *props) {
						if (!p.is_object()) continue;
						const std::string pn = p.value("name", std::string());
						if (pn == "Level") raw.gemLevel = PropertyNumber(p);
						else if (pn == "Quality") raw.gemQuality = PropertyNumber(p);
						else if (pn == "Map Tier") raw.mapTier = PropertyNumber(p);
					}
				}

				auto socks = it.find("sockets");
				if (socks != it.end() && socks->is_array()) {
					int perGroup[8] = {};
					for (const auto& s : *socks) {
						if (!s.is_object()) continue;
						int g = s.value("group", 0);
						if (g >= 0 && g < 8 && ++perGroup[g] > raw.links)
							raw.links = perGroup[g];
					}
				}
				items->push_back(std::move(raw));
				} catch (const std::exception&) {
					continue;
				}
			}
		}
	}
	return true;
	} catch (const std::exception&) {
		return fail(u8"回應格式異常");
	}
}

void SimpleThrottle::OnResponse(long long nowMs, int status,
                                const std::unordered_map<std::string, std::string>& headers)
{
	auto get = [&](const std::string& k) -> std::string {
		auto it = headers.find(k);
		return it == headers.end() ? std::string() : it->second;
	};

	if (status == 429) {
		// Retry-After is seconds; absent means the server did not say, and 60 is
		// the conservative reading. +1s covers clock skew between us and them.
		int ra = ToInt(get("retry-after"), 60);
		long long until = nowMs + ((long long)ra + 1) * 1000;
		if (until > blockedUntilMs_) blockedUntilMs_ = until;
		return;
	}

	// "account,ip" -> for each rule, "x-rate-limit-account" is a comma list of
	// max:window:penalty buckets and "-state" the matching current:window:penalty.
	std::string rules = get("x-rate-limit-rules");
	size_t pos = 0;
	while (pos <= rules.size() && !rules.empty()) {
		size_t comma = rules.find(',', pos);
		std::string rule = rules.substr(pos, comma == std::string::npos ? std::string::npos
		                                                                : comma - pos);
		pos = comma == std::string::npos ? rules.size() + 1 : comma + 1;
		for (char& c : rule) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
		if (rule.empty()) continue;

		std::string limits = get("x-rate-limit-" + rule);
		std::string state = get("x-rate-limit-" + rule + "-state");
		size_t lp = 0, sp = 0;
		while (lp < limits.size() && sp < state.size()) {
			size_t lc = limits.find(',', lp), sc = state.find(',', sp);
			std::string lim = limits.substr(lp, lc == std::string::npos ? std::string::npos : lc - lp);
			std::string cur = state.substr(sp, sc == std::string::npos ? std::string::npos : sc - sp);
			lp = lc == std::string::npos ? limits.size() : lc + 1;
			sp = sc == std::string::npos ? state.size() : sc + 1;

			// max:window:penalty / current:window:penalty
			int lv[3] = {}, sv[3] = {};
			for (int part = 0, at = 0; part < 3; part++) {
				size_t colon = lim.find(':', at);
				lv[part] = ToInt(lim.substr(at, colon == std::string::npos ? std::string::npos : colon - at));
				at = colon == std::string::npos ? (int)lim.size() : (int)colon + 1;
			}
			for (int part = 0, at = 0; part < 3; part++) {
				size_t colon = cur.find(':', at);
				sv[part] = ToInt(cur.substr(at, colon == std::string::npos ? std::string::npos : colon - at));
				at = colon == std::string::npos ? (int)cur.size() : (int)colon + 1;
			}
			const int maxHits = lv[0], windowS = lv[1], penaltyS = lv[2], curHits = sv[0];
			if (maxHits <= 0) continue;
			if (curHits >= maxHits) {
				// Bucket exhausted: back off for the penalty (or the window when the
				// server reports no penalty).
				long long until = nowMs + (long long)(penaltyS > 0 ? penaltyS : windowS) * 1000;
				if (until > blockedUntilMs_) blockedUntilMs_ = until;
			} else if (curHits * 2 >= maxHits) {
				// Past the halfway mark: stretch the spacing so the bucket cannot be
				// reached inside its window. Twice the even-spacing rate keeps a
				// safety margin over requests other tools may be making.
				long long spacing = maxHits > 0 ? ((long long)windowS * 1000 / maxHits) * 2 : 0;
				long long until = lastReqMs_ + spacing;
				if (until > blockedUntilMs_) blockedUntilMs_ = until;
			}
		}
	}
}

int SimpleThrottle::NextDelayMs(long long nowMs) const
{
	long long earliest = lastReqMs_ + minMs_;
	if (blockedUntilMs_ > earliest) earliest = blockedUntilMs_;
	if (earliest <= nowMs) return 0;
	long long d = earliest - nowMs;
	return d > INT_MAX ? INT_MAX : (int)d;
}

namespace {

class SessidStashProvider : public IStashProvider {
public:
	explicit SessidStashProvider(const StashAuth& auth)
	    : auth_(auth), http_(kHost)
	{
		throttle_.SetMinIntervalMs(1000);
	}

	bool Verify(std::string* err, StashError* kind,
	            const std::atomic<bool>* cancel) override
	{
		HttpResult res;
		if (!request(L"/character-window/get-characters", res, err, kind, cancel))
			return false;
		// A dead session is a redirect to the login page: WinHTTP follows it and
		// hands back 200 with HTML, so "is it a JSON array" IS the check.
		try {
			ordered_json doc = ordered_json::parse(res.body);
			if (doc.is_array()) return true;
		} catch (const std::exception&) {
		}
		if (kind) *kind = StashError::Auth;
		if (err) *err = u8"session 無效或已過期";
		return false;
	}

	bool ListTabs(const std::string& league, std::vector<StashTabInfo>& out,
	              std::string* err, StashError* kind,
	              const std::atomic<bool>* cancel) override
	{
		HttpResult res;
		if (!request(stashPath(league, 0, /*wantTabs=*/true), res, err, kind, cancel))
			return false;
		if (!ParseStashTabJson(res.body, 0, nullptr, &out, err)) {
			if (kind) *kind = looksLikeLoginPage(res.body) ? StashError::Auth : StashError::Parse;
			return false;
		}
		if (out.empty()) {
			if (kind) *kind = StashError::Parse;
			if (err) *err = u8"帳號在此聯盟沒有任何倉庫分頁（帳號名或聯盟名錯誤？）";
			return false;
		}
		return true;
	}

	bool FetchTab(const std::string& league, int tabIndex,
	              std::vector<StashItemRaw>& out, std::string* err,
	              StashError* kind, const std::atomic<bool>* cancel) override
	{
		HttpResult res;
		if (!request(stashPath(league, tabIndex, /*wantTabs=*/false), res, err, kind, cancel))
			return false;
		if (!ParseStashTabJson(res.body, tabIndex, &out, nullptr, err)) {
			if (kind) *kind = looksLikeLoginPage(res.body) ? StashError::Auth : StashError::Parse;
			return false;
		}
		return true;
	}

private:
	std::wstring stashPath(const std::string& league, int tabIndex, bool wantTabs) const
	{
		std::wstring p = L"/character-window/get-stash-items?accountName=" +
		                 UrlEncode(auth_.accountName) + L"&realm=pc&league=" +
		                 UrlEncode(league) + L"&tabs=" + (wantTabs ? L"1" : L"0") +
		                 L"&tabIndex=" + std::to_wstring(tabIndex);
		return p;
	}

	static bool looksLikeLoginPage(const std::string& body)
	{
		for (char c : body) {
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
			return c == '<';
		}
		return false;
	}

	// Shared request path: throttle, send with the cookie, classify the status.
	// True only for a 200 -- every other outcome fills *kind and *err.
	bool request(const std::wstring& path, HttpResult& res, std::string* err,
	             StashError* kind, const std::atomic<bool>* cancel)
	{
		if (kind) *kind = StashError::None;

		// Wait out the throttle in slices so Shutdown's cancel is honoured.
		for (;;) {
			int wait = throttle_.NextDelayMs((long long)GetTickCount64());
			if (wait <= 0) break;
			if (cancel && cancel->load()) {
				if (kind) *kind = StashError::Network;
				if (err) *err = u8"已取消";
				return false;
			}
			Sleep(wait > 200 ? 200 : wait);
		}

		HttpExtra extra;
		// The one and only place the secret exists on the wire.
		std::wstring cookie = L"Cookie: POESESSID=";
		cookie += std::wstring(auth_.secret.begin(), auth_.secret.end()); // 32-hex ascii
		extra.headers.push_back(cookie);
		extra.wantHeaders = { L"X-Rate-Limit-Rules",
		                      L"X-Rate-Limit-Account", L"X-Rate-Limit-Account-State",
		                      L"X-Rate-Limit-Ip", L"X-Rate-Limit-Ip-State",
		                      L"Retry-After" };

		throttle_.OnRequest((long long)GetTickCount64());
		std::string terr;
		if (!http_.GetEx(path, extra, res, &terr, cancel)) {
			if (kind) *kind = StashError::Network;
			if (err) *err = terr; // transport messages carry no headers
			return false;
		}
		throttle_.OnResponse((long long)GetTickCount64(), res.status, res.headers);

		switch (res.status) {
		case 200:
			return true;
		case 401:
			if (kind) *kind = StashError::Auth;
			if (err) *err = u8"session 無效或已過期 (HTTP 401)";
			return false;
		case 403:
			// GGG's own 403 is JSON; an HTML body is the edge protection speaking,
			// and telling the user to re-enter their session id would be wrong.
			if (looksLikeLoginPage(res.body)) {
				if (kind) *kind = StashError::Blocked;
				if (err) *err = u8"請求被網站防護阻擋 (HTTP 403)，稍後再試";
			} else {
				if (kind) *kind = StashError::Auth;
				if (err) *err = u8"session 無效或已過期 (HTTP 403)";
			}
			return false;
		case 429:
			if (kind) *kind = StashError::RateLimited;
			if (err) *err = u8"請求過於頻繁 (HTTP 429)，已自動退避";
			return false;
		default:
			if (kind) *kind = StashError::Network;
			if (err) *err = "HTTP " + std::to_string(res.status);
			return false;
		}
	}

	StashAuth auth_;
	HttpsClient http_;
	SimpleThrottle throttle_;
};

} // namespace

std::unique_ptr<IStashProvider> CreateSessidStashProvider(const StashAuth& auth)
{
	return std::make_unique<SessidStashProvider>(auth);
}
