// PobToolsTextureAtlas: headless-only Lua binding that turns one of POB's
// texture files into PNG pages a browser can draw.
//
// PoE2's Path of Building ships its passive tree art as DDS texture arrays
// (TreeData/<ver>/*.dds.zst, BC1/BC7 compressed, one layer per icon; the tree's
// ddsCoords table names a layer index per sprite). A browser cannot decode
// either, so the new UI gets the same pixels as PNG: every layer decoded with
// the engine's own DDS loader and compressonator block decoders, laid out on
// pages of at most 4096 x 4096. Pages land in <exe>\PobTools\cache\tree\ with a
// sidecar recording the source size and write time, so a POB update that ships
// new art is decoded again and an unchanged one is not.
#include "texture_atlas.h"

#include "common.h"
#include "core/core_image.h"
#include "cmp_core.h"
#include "stb_image_write.h"
#include "stb_image_resize.h"

#include <json.hpp>

#include <array>
#include <memory>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <gli/gli.hpp>

#include <sol/sol.hpp> // brings in LuaJIT's lua.h / lauxlib.h the way ui_local.h does

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

const int kPageMax = 4096;
// Layers larger than this are stored downscaled (class and ascendancy plates
// are 1500-4000 px squares the page never shows at full size).
const int kCellMax = 512;

fs::path CacheDir()
{
#ifdef _WIN32
	wchar_t buf[MAX_PATH * 2] = {};
	DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
	fs::path exe(std::wstring(buf, n));
	return exe.parent_path() / L"PobTools" / L"cache" / L"tree";
#else
	return fs::path("PobTools") / "cache" / "tree";
#endif
}

bool SafeKey(const std::string& key)
{
	if (key.empty() || key.size() > 120) return false;
	for (unsigned char c : key) {
		if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
	}
	return key.find("..") == std::string::npos;
}

// BC1 (DXT1) block -> 16 RGBA texels. Written out here rather than linking
// compressonator's BC1 kernel, which drags its SIMD encoder dispatch along.
void DecodeBC1(const uint8_t* b, uint8_t* out)
{
	const unsigned c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
	auto expand = [](unsigned c, uint8_t* p) {
		const unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31;
		p[0] = (uint8_t)((r << 3) | (r >> 2));
		p[1] = (uint8_t)((g << 2) | (g >> 4));
		p[2] = (uint8_t)((bl << 3) | (bl >> 2));
		p[3] = 255;
	};
	uint8_t pal[16];
	expand(c0, pal);
	expand(c1, pal + 4);
	for (int k = 0; k < 3; k++) {
		if (c0 > c1) {
			pal[8 + k] = (uint8_t)((2 * pal[k] + pal[4 + k]) / 3);
			pal[12 + k] = (uint8_t)((pal[k] + 2 * pal[4 + k]) / 3);
		} else {
			pal[8 + k] = (uint8_t)((pal[k] + pal[4 + k]) / 2);
			pal[12 + k] = 0;
		}
	}
	pal[11] = 255;
	pal[15] = c0 > c1 ? 255 : 0;
	const uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
	for (int t = 0; t < 16; t++) memcpy(out + t * 4, pal + ((idx >> (2 * t)) & 3) * 4, 4);
}

// One mip-0 layer as RGBA8, or empty when the format is not one we decode.
std::vector<uint8_t> DecodeLayer(const gli::texture2d_array& tex, size_t layer)
{
	const gli::format fmt = tex.format();
	const auto ext = tex.extent(0);
	const size_t w = ext.x, h = ext.y;
	std::vector<uint8_t> out(w * h * 4, 0);
	const auto* src = (const uint8_t*)tex.data(layer, 0, 0);

	int bc = 0;
	switch (fmt) {
	case gli::FORMAT_RGB_DXT1_UNORM_BLOCK8:
	case gli::FORMAT_RGB_DXT1_SRGB_BLOCK8:
	case gli::FORMAT_RGBA_DXT1_UNORM_BLOCK8:
	case gli::FORMAT_RGBA_DXT1_SRGB_BLOCK8:
		bc = 1; break;
	case gli::FORMAT_RGBA_DXT5_UNORM_BLOCK16:
	case gli::FORMAT_RGBA_DXT5_SRGB_BLOCK16:
		bc = 3; break;
	case gli::FORMAT_RGBA_BP_UNORM_BLOCK16:
	case gli::FORMAT_RGBA_BP_SRGB_BLOCK16:
		bc = 7; break;
	case gli::FORMAT_RGBA8_UNORM_PACK8:
	case gli::FORMAT_RGBA8_SRGB_PACK8:
		memcpy(out.data(), src, out.size());
		return out;
	case gli::FORMAT_BGRA8_UNORM_PACK8:
	case gli::FORMAT_BGRA8_SRGB_PACK8:
		for (size_t i = 0; i < w * h; i++) {
			out[i * 4 + 0] = src[i * 4 + 2];
			out[i * 4 + 1] = src[i * 4 + 1];
			out[i * 4 + 2] = src[i * 4 + 0];
			out[i * 4 + 3] = src[i * 4 + 3];
		}
		return out;
	default:
		return {};
	}

	const size_t blockBytes = gli::block_size(fmt);
	const size_t bw = (w + 3) / 4, bh = (h + 3) / 4;
	std::array<uint8_t, 64> rgba{};
	for (size_t by = 0; by < bh; by++) {
		for (size_t bx = 0; bx < bw; bx++) {
			switch (bc) {
			case 1: DecodeBC1(src, rgba.data()); break;
			case 3: DecompressBlockBC3(src, rgba.data()); break;
			default: DecompressBlockBC7(src, rgba.data()); break;
			}
			src += blockBytes;
			// Blocks are always 4x4; clip the ones hanging over the right/bottom edge.
			const size_t rows = (std::min)(size_t(4), h - by * 4);
			const size_t cols = (std::min)(size_t(4), w - bx * 4);
			for (size_t r = 0; r < rows; r++) {
				memcpy(&out[((by * 4 + r) * w + bx * 4) * 4], rgba.data() + r * 16, cols * 4);
			}
		}
	}
	return out;
}

json Layout(int srcW, int srcH, int layers, const std::string& key)
{
	const int f = (std::max)(1, ((std::max)(srcW, srcH) + kCellMax - 1) / kCellMax);
	const int layerW = (std::max)(1, srcW / f), layerH = (std::max)(1, srcH / f);
	const int perRow = (std::max)(1, kPageMax / (std::max)(1, layerW));
	const int rowsPerPage = (std::max)(1, kPageMax / (std::max)(1, layerH));
	const int perPage = perRow * rowsPerPage;
	json pages = json::array();
	for (int first = 0, p = 1; first < layers; first += perPage, p++) {
		const int count = (std::min)(perPage, layers - first);
		const int cols = (std::min)(perRow, count);
		const int rows = (count + perRow - 1) / perRow;
		pages.push_back({ { "file", "tree/" + key + "-" + std::to_string(p) + ".png" },
		                  { "w", cols * layerW }, { "h", rows * layerH } });
	}
	return { { "layerW", layerW }, { "layerH", layerH }, { "srcW", srcW }, { "srcH", srcH }, { "layers", layers },
	         { "perRow", perRow }, { "perPage", perPage }, { "pages", pages } };
}

bool WritePng(const fs::path& path, int w, int h, const uint8_t* rgba)
{
#ifdef _WIN32
	FILE* f = _wfopen(path.wstring().c_str(), L"wb");
#else
	FILE* f = fopen(path.string().c_str(), "wb");
#endif
	if (!f) return false;
	int ok = stbi_write_png_to_func([](void* ctx, void* data, int size) {
		fwrite(data, 1, size, (FILE*)ctx);
	}, f, w, h, 4, rgba, w * 4);
	fclose(f);
	return ok != 0;
}

void PushLayout(lua_State* L, const json& j)
{
	lua_newtable(L);
	for (const char* k : { "layerW", "layerH", "srcW", "srcH", "layers", "perRow", "perPage" }) {
		lua_pushinteger(L, j.value(k, 0));
		lua_setfield(L, -2, k);
	}
	lua_newtable(L);
	int i = 1;
	for (const auto& p : j["pages"]) {
		lua_newtable(L);
		lua_pushstring(L, p.value("file", "").c_str());
		lua_setfield(L, -2, "file");
		lua_pushinteger(L, p.value("w", 0));
		lua_setfield(L, -2, "w");
		lua_pushinteger(L, p.value("h", 0));
		lua_setfield(L, -2, "h");
		lua_rawseti(L, -2, i++);
	}
	lua_setfield(L, -2, "pages");
}

// PobToolsTextureAtlas(path, key) -> layout | nil, message
//   path: a texture file, relative to POB's script folder (the working
//         directory Lua runs in) or absolute.
//   key:  cache file stem, [A-Za-z0-9_.-] only.
int l_PobToolsTextureAtlas(lua_State* L)
{
	const char* pathArg = luaL_checkstring(L, 1);
	const std::string key = luaL_checkstring(L, 2);
	if (!SafeKey(key)) {
		lua_pushnil(L);
		lua_pushstring(L, "bad cache key");
		return 2;
	}
	const fs::path src = fs::u8path(pathArg);
	std::error_code ec;
	const auto srcSize = fs::file_size(src, ec);
	if (ec) {
		lua_pushnil(L);
		lua_pushstring(L, ("cannot stat " + std::string(pathArg)).c_str());
		return 2;
	}
	const auto srcTime = (long long)fs::last_write_time(src, ec).time_since_epoch().count();

	const fs::path dir = CacheDir();
	const fs::path meta = dir / fs::u8path(key + ".json");
	{
		FILE* f = nullptr;
#ifdef _WIN32
		f = _wfopen(meta.wstring().c_str(), L"rb");
#else
		f = fopen(meta.string().c_str(), "rb");
#endif
		if (f) {
			std::string body;
			char buf[4096];
			size_t got;
			while ((got = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, got);
			fclose(f);
			json m = json::parse(body, nullptr, false);
			bool pagesThere = m.is_object() && m.contains("layout");
			if (pagesThere) {
				for (const auto& p : m["layout"]["pages"]) {
					if (!fs::exists(dir.parent_path() / fs::u8path(p.value("file", "")), ec)) pagesThere = false;
				}
			}
			if (pagesThere && m.value("srcSize", 0ull) == (unsigned long long)srcSize && m.value("srcTime", 0ll) == srcTime) {
				PushLayout(L, m["layout"]);
				return 1;
			}
		}
	}

	std::unique_ptr<image_c> img(image_c::LoaderForFile(nullptr, src));
	if (!img || img->Load(src) || img->tex.empty()) {
		lua_pushnil(L);
		lua_pushstring(L, ("cannot load " + std::string(pathArg)).c_str());
		return 2;
	}
	const auto& tex = img->tex;
	const int srcW = (int)tex.extent(0).x, srcH = (int)tex.extent(0).y;
	const int layers = (int)tex.layers();
	json layout = Layout(srcW, srcH, layers, key);
	const int layerW = layout["layerW"], layerH = layout["layerH"];

	fs::create_directories(dir, ec);
	int perRow = layout["perRow"], perPage = layout["perPage"];
	int pageNo = 0;
	for (const auto& page : layout["pages"]) {
		const int pw = page["w"], ph = page["h"];
		std::vector<uint8_t> canvas((size_t)pw * ph * 4, 0);
		const int first = pageNo * perPage;
		const int count = (std::min)(perPage, layers - first);
		for (int i = 0; i < count; i++) {
			std::vector<uint8_t> px = DecodeLayer(tex, first + i);
			if (px.empty()) {
				lua_pushnil(L);
				lua_pushstring(L, ("unsupported texture format in " + std::string(pathArg)).c_str());
				return 2;
			}
			if (layerW != srcW || layerH != srcH) {
				std::vector<uint8_t> small((size_t)layerW * layerH * 4);
				stbir_resize_uint8(px.data(), srcW, srcH, 0, small.data(), layerW, layerH, 0, 4);
				px.swap(small);
			}
			const int ox = (i % perRow) * layerW, oy = (i / perRow) * layerH;
			for (int r = 0; r < layerH; r++) {
				memcpy(&canvas[((size_t)(oy + r) * pw + ox) * 4], &px[(size_t)r * layerW * 4], (size_t)layerW * 4);
			}
		}
		if (!WritePng(dir.parent_path() / fs::u8path(page.value("file", "")), pw, ph, canvas.data())) {
			lua_pushnil(L);
			lua_pushstring(L, "cannot write the atlas page");
			return 2;
		}
		pageNo++;
	}

	json m = { { "srcSize", (unsigned long long)srcSize }, { "srcTime", srcTime }, { "layout", layout } };
#ifdef _WIN32
	FILE* f = _wfopen(meta.wstring().c_str(), L"wb");
#else
	FILE* f = fopen(meta.string().c_str(), "wb");
#endif
	if (f) {
		const std::string body = m.dump();
		fwrite(body.data(), 1, body.size(), f);
		fclose(f);
	}
	PushLayout(L, layout);
	return 1;
}

} // namespace

namespace TextureAtlas {

void Register(lua_State* L)
{
	lua_pushcfunction(L, l_PobToolsTextureAtlas);
	lua_setglobal(L, "PobToolsTextureAtlas");
}

} // namespace TextureAtlas
