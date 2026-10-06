#include "regex_share.h"

#include "regex_state_json.h"

#include <miniz.h>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <stdexcept>

using nlohmann::ordered_json;
using RegexAlgo::PageRef;
using RegexFrag::AlgoValue;

namespace RegexShare {

namespace {

// ---- small JS-isms ------------------------------------------------------------

// JS String(x) of a JSON value; `v` null = undefined.
std::string JsString(const ordered_json* v)
{
	if (!v) return "undefined";
	switch (v->type()) {
	case ordered_json::value_t::null: return "null";
	case ordered_json::value_t::boolean: return v->get<bool>() ? "true" : "false";
	case ordered_json::value_t::string: return v->get<std::string>();
	case ordered_json::value_t::number_integer: return std::to_string(v->get<long long>());
	case ordered_json::value_t::number_unsigned: return std::to_string(v->get<unsigned long long>());
	case ordered_json::value_t::number_float: {
		const double d = v->get<double>();
		if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 1e21) return RegexStateJson::JsIntegral(d);
		return v->dump();
	}
	case ordered_json::value_t::array: {
		// Array.prototype.join(","): null / undefined -> ""
		std::string out;
		bool first = true;
		for (const ordered_json& e : *v) {
			if (!first) out += ',';
			first = false;
			if (!e.is_null()) out += JsString(&e);
		}
		return out;
	}
	default: return "[object Object]";
	}
}

// One UTF-8 code point at s[i] (length of the sequence; 1 for a stray byte).
size_t CpLen(const std::string& s, size_t i, unsigned* cp)
{
	const unsigned char c = (unsigned char)s[i];
	size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
	if (i + n > s.size()) n = 1;
	unsigned v = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f) : (c & 0x07);
	for (size_t k = 1; k < n; k++) v = (v << 6) | ((unsigned char)s[i + k] & 0x3f);
	if (cp) *cp = v;
	return n;
}

// JS \s (WhiteSpace + LineTerminator).
bool JsSpace(unsigned cp)
{
	return cp == 0x09 || cp == 0x0a || cp == 0x0b || cp == 0x0c || cp == 0x0d || cp == 0x20 || cp == 0xa0 ||
	       cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f ||
	       cp == 0x205f || cp == 0x3000 || cp == 0xfeff;
}

// TextDecoder('utf-8', {fatal: true}): well-formed UTF-8 or nothing.
bool ValidUtf8(const std::string& s)
{
	size_t i = 0;
	const size_t n = s.size();
	while (i < n) {
		const unsigned char c = (unsigned char)s[i];
		if (c < 0x80) { i++; continue; }
		size_t len;
		unsigned cp, min;
		if ((c >> 5) == 6) { len = 2; cp = c & 0x1f; min = 0x80; }
		else if ((c >> 4) == 14) { len = 3; cp = c & 0x0f; min = 0x800; }
		else if ((c >> 3) == 30) { len = 4; cp = c & 0x07; min = 0x10000; }
		else return false;
		if (i + len > n) return false;
		for (size_t k = 1; k < len; k++) {
			const unsigned char d = (unsigned char)s[i + k];
			if ((d >> 6) != 2) return false;
			cp = (cp << 6) | (d & 0x3f);
		}
		if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
		i += len;
	}
	return true;
}

// ---- ordered lists ------------------------------------------------------------

template <class V>
V* Find(std::vector<std::pair<std::string, V>>& m, const std::string& k)
{
	for (auto& kv : m)
		if (kv.first == k) return &kv.second;
	return nullptr;
}
template <class V>
const V* Find(const std::vector<std::pair<std::string, V>>& m, const std::string& k)
{
	for (const auto& kv : m)
		if (kv.first == k) return &kv.second;
	return nullptr;
}
// JS `o[k] = v`: an existing key keeps its place, a new one goes last.
template <class V>
V& Slot(std::vector<std::pair<std::string, V>>& m, const std::string& k)
{
	if (V* v = Find(m, k)) return *v;
	m.emplace_back(k, V{});
	return m.back().second;
}
template <class V>
void Erase(std::vector<std::pair<std::string, V>>& m, const std::string& k)
{
	m.erase(std::remove_if(m.begin(), m.end(), [&](const std::pair<std::string, V>& kv) { return kv.first == k; }),
	        m.end());
}

// ---- normalize ----------------------------------------------------------------

// share.ts:39 strList
std::vector<std::string> StrList(const ordered_json* v, const std::string& where, std::vector<std::string>& warnings)
{
	std::vector<std::string> out;
	if (!v) return out;
	if (!v->is_array()) {
		warnings.push_back(where + " 不是陣列,已忽略");
		return out;
	}
	for (const ordered_json& x : *v)
		if (x.is_string()) out.push_back(x.get<std::string>());
	if (out.size() != v->size())
		warnings.push_back(where + " 有 " + std::to_string(v->size() - out.size()) + " 個非字串值,已忽略");
	return out;
}

const ordered_json* Field(const ordered_json& o, const char* k)
{
	auto it = o.find(k);
	return it == o.end() ? nullptr : &*it;
}

struct ShareError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

// share.ts:65 normalizeShareState
Normalized NormalizeValue(const ordered_json& raw, bool requireVersion)
{
	if (!raw.is_object()) throw ShareError("分享碼內容不是物件");
	Normalized out;
	std::vector<std::string>& warnings = out.warnings;
	State& st = out.state;
	const ordered_json* v = Field(raw, "v");
	if (requireVersion) {
		const bool ok = v && v->is_number() && (v->get<double>() == 1.0 || v->get<double>() == 2.0);
		if (!ok) throw ShareError("分享碼版本不符(" + JsString(v) + ",本版只認 1 / 2)");
	}
	const ordered_json* game = Field(raw, "game");
	if (!game || !game->is_string() || (game->get<std::string>() != "poe1" && game->get<std::string>() != "poe2"))
		throw ShareError("分享碼的遊戲不明(" + JsString(game) + ")");
	st.game = game->get<std::string>();
	st.mode = "any";
	if (const ordered_json* m = Field(raw, "mode")) {
		const std::string s = m->is_string() ? m->get<std::string>() : std::string();
		if (m->is_string() && (s == "any" || s == "all" || s == "none")) st.mode = s;
		else warnings.push_back("mode「" + JsString(m) + "」不認得,改用 any");
	}
	auto keyLists = [&](const char* name, KeyLists& dst) {
		const ordered_json* f = Field(raw, name);
		if (!f) return;
		if (!f->is_object()) {
			warnings.push_back(std::string(name) + " 不是物件,已忽略");
			return;
		}
		for (auto it = f->begin(); it != f->end(); ++it)
			Slot(dst, it.key()) = StrList(&it.value(), std::string(name) + "." + it.key(), warnings);
	};
	keyLists("pages", st.pages);
	keyLists("sections", st.sections);
	if (const ordered_json* f = Field(raw, "numeric")) {
		if (!f->is_object()) {
			warnings.push_back("numeric 不是物件,已忽略");
		} else {
			for (auto it = f->begin(); it != f->end(); ++it) {
				if (!it.value().is_object()) {
					warnings.push_back("numeric." + it.key() + " 不是物件,已忽略");
					continue;
				}
				RegexValueList m;
				for (auto e = it.value().begin(); e != it.value().end(); ++e) {
					AlgoValue val;
					if (RegexStateJson::ValueFrom(e.value(), val)) RegexValueSet(m, e.key(), val);
					else warnings.push_back("numeric." + it.key() + "." + e.key() + " 不是物件,已忽略");
				}
				Slot(st.numeric, it.key()) = std::move(m);
			}
		}
	}
	static const std::set<std::string> kKnown = {"v", "game", "mode", "pages", "sections", "numeric", "custom", "excludes",
	                                             "id", "name", "desc"};
	for (auto it = raw.begin(); it != raw.end(); ++it)
		if (!kKnown.count(it.key())) warnings.push_back("未知欄位「" + it.key() + "」已忽略");
	MigrateSections(st);
	st.custom = StrList(Field(raw, "custom"), "custom", warnings);
	st.excludes = StrList(Field(raw, "excludes"), "excludes", warnings);
	return out;
}

ordered_json KeyListsJson(const KeyLists& m)
{
	ordered_json o = ordered_json::object();
	for (const auto& kv : m) o[kv.first] = kv.second;
	return o;
}

// ---- gzip ---------------------------------------------------------------------

struct Sink {
	std::string* out;
	size_t max;
	bool overflow = false;
};

int PutBuf(const void* buf, int len, void* user)
{
	Sink* s = (Sink*)user;
	if (s->out->size() + (size_t)len > s->max) {
		s->overflow = true;
		return 0;
	}
	s->out->append((const char*)buf, (size_t)len);
	return 1;
}

uint32_t Le32(const std::string& s, size_t i)
{
	return (uint32_t)(unsigned char)s[i] | ((uint32_t)(unsigned char)s[i + 1] << 8) |
	       ((uint32_t)(unsigned char)s[i + 2] << 16) | ((uint32_t)(unsigned char)s[i + 3] << 24);
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

} // namespace

// ---- base64url (share.ts:140 / :151) -------------------------------------------

std::string Base64Url(const std::string& bytes)
{
	std::string s;
	s.reserve(bytes.size() * 4 / 3 + 4);
	const size_t n = bytes.size();
	for (size_t i = 0; i < n; i += 3) {
		const unsigned b0 = (unsigned char)bytes[i];
		const unsigned b1 = i + 1 < n ? (unsigned char)bytes[i + 1] : 0;
		const unsigned b2 = i + 2 < n ? (unsigned char)bytes[i + 2] : 0;
		const unsigned v = (b0 << 16) | (b1 << 8) | b2;
		s += kB64[(v >> 18) & 63];
		s += kB64[(v >> 12) & 63];
		if (i + 1 < n) s += kB64[(v >> 6) & 63];
		if (i + 2 < n) s += kB64[v & 63];
	}
	return s;
}

bool FromBase64Url(const std::string& text, std::string& out, std::string* err)
{
	out.clear();
	out.reserve(text.size() * 3 / 4 + 3);
	unsigned buf = 0;
	int bits = 0;
	for (size_t i = 0; i < text.size();) {
		unsigned cp = 0;
		const size_t len = CpLen(text, i, &cp);
		const std::string ch = text.substr(i, len);
		i += len;
		if (cp == '=' || JsSpace(cp)) continue;
		if (cp == '+') cp = '-';
		else if (cp == '/') cp = '_';
		const char* hit = cp < 128 && cp ? std::strchr(kB64, (int)cp) : nullptr;
		if (!hit) {
			if (err) *err = "分享碼含有不合法字元「" + ch + "」";
			return false;
		}
		buf = ((buf << 6) | (unsigned)(hit - kB64)) & 0xffffff;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += (char)((buf >> bits) & 255);
		}
	}
	return true;
}

// ---- gzip ---------------------------------------------------------------------

std::string Gzip(const std::string& data)
{
	// Node's zlib header: no flags, mtime 0, xfl 0, OS 10 (what CompressionStream
	// wrote on the machine the fixtures came from).
	std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x0a", 10);
	size_t len = 0;
	const int flags = (int)tdefl_create_comp_flags_from_zip_params(MZ_DEFAULT_LEVEL, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
	void* p = tdefl_compress_mem_to_heap(data.data(), data.size(), &len, flags);
	if (!p && !data.empty()) return std::string();
	if (p) {
		out.append((const char*)p, len);
		mz_free(p);
	} else {
		// An empty input still has to be a valid deflate stream: one final empty fixed block.
		out.append("\x03\x00", 2);
	}
	const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char*)data.data(), data.size());
	const uint32_t size = (uint32_t)data.size();
	for (int k = 0; k < 4; k++) out += (char)((crc >> (8 * k)) & 255);
	for (int k = 0; k < 4; k++) out += (char)((size >> (8 * k)) & 255);
	return out;
}

bool Gunzip(const std::string& gz, std::string& out, size_t maxOut, std::string* err)
{
	auto fail = [&](const char* m) {
		if (err) *err = m;
		out.clear();
		return false;
	};
	out.clear();
	if (gz.size() < 18) return fail("資料太短,不是 gzip");
	if ((unsigned char)gz[0] != 0x1f || (unsigned char)gz[1] != 0x8b) return fail("不是 gzip 資料");
	if ((unsigned char)gz[2] != 8) return fail("gzip 壓縮方式不支援");
	const unsigned flg = (unsigned char)gz[3];
	if (flg & 0xe0) return fail("gzip 標頭的保留位元不是 0");
	size_t pos = 10;
	if (flg & 4) {   // FEXTRA
		if (pos + 2 > gz.size()) return fail("gzip 標頭不完整");
		const size_t xlen = (unsigned char)gz[pos] | ((size_t)(unsigned char)gz[pos + 1] << 8);
		pos += 2 + xlen;
	}
	for (unsigned bit : {8u, 16u}) {   // FNAME, FCOMMENT: zero-terminated
		if (!(flg & bit)) continue;
		while (pos < gz.size() && gz[pos] != '\0') pos++;
		pos++;
	}
	if (flg & 2) {   // FHCRC: low 16 bits of the header's CRC32
		if (pos + 2 > gz.size()) return fail("gzip 標頭不完整");
		const uint32_t want = (unsigned char)gz[pos] | ((uint32_t)(unsigned char)gz[pos + 1] << 8);
		const uint32_t got = (uint32_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char*)gz.data(), pos) & 0xffff;
		if (want != got) return fail("gzip 標頭檢查碼不符");
		pos += 2;
	}
	if (pos + 8 > gz.size()) return fail("gzip 標頭不完整");
	Sink sink{&out, maxOut};
	size_t inSize = gz.size() - pos;
	const int ok = tinfl_decompress_mem_to_callback(gz.data() + pos, &inSize, PutBuf, &sink, 0);
	if (sink.overflow) return fail("解壓縮後超過大小上限");
	if (ok != 1) return fail("壓縮資料損壞或被截斷");
	pos += inSize;
	if (pos + 8 > gz.size()) return fail("資料被截斷(缺少 gzip 結尾)");
	const uint32_t crc = Le32(gz, pos), isize = Le32(gz, pos + 4);
	if (crc != (uint32_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char*)out.data(), out.size()))
		return fail("CRC32 檢查碼不符");
	if (isize != (uint32_t)out.size()) return fail("解壓縮長度與 gzip 結尾不符");
	if (pos + 8 != gz.size()) return fail("gzip 結尾後面還有多餘的資料");
	return true;
}

// ---- state --------------------------------------------------------------------

void MigrateSections(State& s)
{
	// sections.ts SECTION_HOSTS, in its order
	for (const char* sec : {"map_numeric", "waystone_numeric"}) {
		const std::string host = RegexAlgo::SectionHostOf(sec);
		if (const std::vector<std::string>* keys = Find(s.pages, sec)) {
			if (!keys->empty()) {
				const std::vector<std::string> add = *keys;
				std::vector<std::string>& dst = Slot(s.sections, host);
				dst = RegexStateJson::UnionKeys(dst, add);
			}
			Erase(s.pages, sec);
		}
		if (const RegexValueList* m = Find(s.numeric, sec)) {
			const RegexValueList add = *m;
			RegexValueList& dst = Slot(s.numeric, host);
			for (const auto& kv : add) RegexValueSet(dst, kv.first, kv.second);
			Erase(s.numeric, sec);
		}
	}
}

bool NormalizeJson(const std::string& jsonText, bool requireVersion, Normalized& out, std::string* err)
{
	ordered_json raw;
	try {
		raw = ordered_json::parse(jsonText);
	} catch (const std::exception&) {
		if (err) *err = "分享碼內容不是 JSON";
		return false;
	}
	try {
		out = NormalizeValue(raw, requireVersion);
	} catch (const std::exception& e) {
		if (err) *err = e.what();
		return false;
	}
	return true;
}

std::string ToJson(const State& s)
{
	ordered_json doc;
	doc["v"] = kVersion;
	doc["game"] = s.game;
	doc["mode"] = s.mode;
	doc["pages"] = KeyListsJson(s.pages);
	doc["sections"] = KeyListsJson(s.sections);
	ordered_json nu = ordered_json::object();
	for (const auto& kv : s.numeric) {
		ordered_json m = ordered_json::object();
		for (const auto& e : kv.second) m[e.first] = RegexStateJson::ValueTo(e.second);
		nu[kv.first] = std::move(m);
	}
	doc["numeric"] = std::move(nu);
	doc["custom"] = s.custom;
	doc["excludes"] = s.excludes;
	// JSON.stringify: compact, UTF-8 as is, control characters as \u00xx.
	return doc.dump(-1, ' ', false, ordered_json::error_handler_t::replace);
}

std::string Encode(const State& s)
{
	return Base64Url(Gzip(ToJson(s)));
}

bool Decode(const std::string& code, Normalized& out, std::string* err)
{
	const std::string text = RegexAlgo::JsTrim(code);
	if (text.empty()) {
		if (err) *err = "分享碼是空的";
		return false;
	}
	if (text.size() > kMaxCodeChars) {
		if (err) *err = "分享碼太長(" + std::to_string(text.size()) + " 字元)";
		return false;
	}
	std::string bytes, json, why;
	bool ok = FromBase64Url(text, bytes, &why) && Gunzip(bytes, json, kMaxJsonBytes, &why);
	if (ok && !ValidUtf8(json)) {
		ok = false;
		why = "內容不是合法的 UTF-8";
	}
	if (!ok) {
		if (err) *err = "分享碼無法解壓縮(" + why + ")";
		return false;
	}
	return NormalizeJson(json, true, out, err);
}

// ---- resolve ------------------------------------------------------------------

Resolved Resolve(const State& s, const std::vector<PageRef>& pages)
{
	Resolved out;
	std::vector<PageRef> mine;
	for (const PageRef& p : pages)
		if (p.Game() == s.game) mine.push_back(p);
	auto findPage = [&](const std::string& id) -> const PageRef* {
		for (const PageRef& p : mine)
			if (p.Id() == id) return &p;
		return nullptr;
	};
	auto unknown = [&](const std::string& id) {
		if (std::find(out.unknownPages.begin(), out.unknownPages.end(), id) == out.unknownPages.end())
			out.unknownPages.push_back(id);
	};
	auto miss = [&](const std::string& id, int n) {
		if (n <= 0) return;
		out.missed += n;
		Slot(out.missedByPage, id) += n;
	};
	for (const auto& kv : s.pages) {
		const PageRef* page = findPage(kv.first);
		if (!page) {
			out.unknownPages.push_back(kv.first);
			miss(kv.first, (int)kv.second.size());
			continue;
		}
		const RegexEmbed::Applied r = RegexEmbed::ApplyPageKeys(*page, kv.second);
		out.picks[kv.first] = r.picked;
		miss(kv.first, r.missed);
	}
	for (const auto& kv : s.sections) {
		const RegexAlgo::AlgoPage* sec = RegexAlgo::SectionPageOf(mine, kv.first, s.game);
		if (!sec) {
			unknown(kv.first);
			miss(kv.first, (int)kv.second.size());
			continue;
		}
		PageRef ref;
		ref.algo = sec;
		const RegexEmbed::Applied r = RegexEmbed::ApplyPageKeys(ref, kv.second);
		out.picks[sec->id] = r.picked;
		miss(kv.first, r.missed);
	}
	for (const auto& kv : s.numeric) {
		// share.ts:256-266 (step 40, B d5ccb47): a host page
		// id -> its section; anything else (the vendor page) -> itself. A host that is
		// itself algorithmic (the item-mod values page + its rarity | corruption
		// section) shares the key with its section: each gets the entry ids it has.
		const RegexAlgo::AlgoPage* sec = RegexAlgo::SectionPageOf(mine, kv.first, s.game);
		const PageRef* own = findPage(kv.first);
		std::vector<PageRef> targets;
		if (sec) {
			PageRef r;
			r.algo = sec;
			targets.push_back(r);
			if (own && own->algo) targets.push_back(*own);
		} else if (own) {
			targets.push_back(*own);
		}
		if (targets.empty()) {
			unknown(kv.first);
			continue;
		}
		for (const PageRef& ref : targets) {
			RegexValueList m;
			for (const auto& e : kv.second) {
				bool exists = false;
				if (ref.algo) {
					for (const RegexAlgo::AlgoEntry& a : ref.algo->entries) exists = exists || a.def.id == e.first;
				} else {
					for (const RegexEntryDef& d : ref.corpus->entries) exists = exists || d.id == e.first;
				}
				if (exists) RegexValueSet(m, e.first, e.second);
			}
			Slot(out.values, ref.Id()) = std::move(m);
		}
	}
	return out;
}

ValueLists ResolvedValues(const ValueLists& values)
{
	ValueLists out;
	for (const auto& kv : values) {
		RegexValueList& dst = Slot(out, RegexAlgo::NumericKeyOf(kv.first));
		for (const auto& e : kv.second) RegexValueSet(dst, e.first, e.second);
	}
	return out;
}

State StateOf(const std::string& game, const std::vector<PageRef>& pages, const RegexEmbed::PicksMap& picks,
              const RegexEmbed::ValuesMap& values, const std::string& mode, const std::vector<std::string>& custom,
              const std::vector<std::string>& excludes)
{
	State s;
	s.game = game;
	s.mode = mode;
	s.custom = custom;
	s.excludes = excludes;
	for (const PageRef& p : pages) {
		if (p.Game() != game) continue;
		auto it = picks.find(p.Id());
		if (it == picks.end() || it->second.empty()) continue;
		const std::vector<int>& pk = it->second;
		const std::vector<std::string> keys = RegexEmbed::PageKeysOf(p, pk).keys;
		if (p.IsSection()) Slot(s.sections, p.algo->sectionOf) = keys;
		else Slot(s.pages, p.Id()) = keys;
		if (p.algo) {
			auto vit = values.find(RegexAlgo::NumericKeyOf(p.Id()));
			RegexValueList m;
			for (int i : pk) {
				if (i < 0 || i >= (int)p.algo->entries.size()) continue;
				const RegexAlgo::AlgoEntry& e = p.algo->entries[i];
				const AlgoValue* cur = nullptr;
				if (vit != values.end()) {
					auto v = vit->second.find(e.def.id);
					if (v != vit->second.end()) cur = &v->second;
				}
				RegexValueSet(m, e.def.id, cur ? *cur : e.input.def);
			}
			// embed.ts:131 (step 40, B d5ccb47): a host algorithmic page and
			// its section share the host id (entry ids never overlap) -> merged
			RegexValueList& dst = Slot(s.numeric, RegexAlgo::NumericKeyOf(p.Id()));
			for (const auto& kv : m) RegexValueSet(dst, kv.first, kv.second);
		}
	}
	return s;
}

// ---- templates ------------------------------------------------------------------

bool ParseTemplates(const std::string& text, std::vector<Template>& out, std::vector<std::string>& errors,
                    std::string* err)
{
	out.clear();
	errors.clear();
	ordered_json doc;
	try {
		doc = ordered_json::parse(text);
	} catch (const std::exception& e) {
		if (err) *err = std::string("templates.json 不是 JSON:") + e.what();
		return false;
	}
	const ordered_json* list = doc.is_object() ? Field(doc, "templates") : nullptr;
	if (!list || !list->is_array()) {
		if (err) *err = "templates.json 缺少 templates 陣列";
		return false;
	}
	for (const ordered_json& t : *list) {
		try {
			const ordered_json* id = t.is_object() ? Field(t, "id") : nullptr;
			if (!id || !id->is_string() || id->get<std::string>().empty()) throw ShareError("缺 id");
			const ordered_json empty = ordered_json::object();
			const ordered_json* nf = Field(t, "name");
			const ordered_json* df = Field(t, "desc");
			const ordered_json& name = nf && nf->is_object() ? *nf : empty;
			const ordered_json& desc = df && df->is_object() ? *df : empty;
			Normalized n = NormalizeValue(t, false);
			if (!n.warnings.empty()) {
				std::string j;
				for (size_t i = 0; i < n.warnings.size(); i++) j += (i ? ";" : "") + n.warnings[i];
				throw ShareError(j);
			}
			// JS `a ?? b`: only null / undefined fall through.
			auto pick = [](std::initializer_list<const ordered_json*> xs, const std::string& def) {
				for (const ordered_json* x : xs)
					if (x && !x->is_null()) return JsString(x);
				return def;
			};
			Template tp;
			tp.id = id->get<std::string>();
			tp.game = n.state.game;
			tp.nameZh = pick({Field(name, "zh")}, tp.id);
			tp.nameEn = pick({Field(name, "en"), Field(name, "zh")}, tp.id);
			tp.descZh = pick({Field(desc, "zh")}, "");
			tp.descEn = pick({Field(desc, "en"), Field(desc, "zh")}, "");
			tp.state = std::move(n.state);
			out.push_back(std::move(tp));
		} catch (const std::exception& e) {
			const ordered_json* id = t.is_object() ? Field(t, "id") : nullptr;
			errors.push_back("範本 " + (t.is_object() ? JsString(id) : std::string("?")) + ":" + e.what());
		}
	}
	return true;
}

bool LoadTemplates(const std::wstring& exeDir, std::vector<Template>& out, std::vector<std::string>& errors,
                   std::string* err)
{
	out.clear();
	errors.clear();
	const std::wstring path = exeDir + L"Data\\regex_templates.json";
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		if (err) *err = u8"找不到 Data\\regex_templates.json";
		return false;
	}
	std::string body;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 24)) {
		body.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = ReadFile(h, &body[0], (DWORD)body.size(), &read, nullptr) && read == body.size();
	}
	CloseHandle(h);
	if (!ok) {
		if (err) *err = u8"讀不到 Data\\regex_templates.json";
		return false;
	}
	return ParseTemplates(body, out, errors, err);
}

} // namespace RegexShare
