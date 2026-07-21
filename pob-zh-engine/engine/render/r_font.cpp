// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Module: Render Font
//

#include "r_local.h"

#include "translation_manager.h"

#include <fmt/format.h>
#include <iostream>
#include <fstream>
#include <string>
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <algorithm>

#include <ft2build.h>
#include FT_FREETYPE_H

// =======
// Classes
// =======

// Glyph parameters
struct f_glyph_s {
	float	tcLeft = 0.0;
	float	tcRight = 0.0;
	float	tcTop = 0.0;
	float	tcBottom = 0.0;
	int		width = 0;
	int		spLeft = 0;
	int		spRight = 0;
};

// Font height info
struct f_fontHeight_s {
	r_tex_c* tex;
	int		height;
	int		numGlyph;
	f_glyph_s glyphs[128];
	f_glyph_s defGlyph{0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0};

	f_glyph_s const& Glyph(char ch) const {
		if ((unsigned char)ch >= numGlyph) {
			return defGlyph;
		}
		return glyphs[(unsigned char)ch];
	}
};

// =====================
// FreeType CJK Fallback
// =====================

static const int FT_ATLAS_SIZE = 2048;

struct FtCachedGlyph {
	int atlasIndex;
	float tcLeft, tcRight, tcTop, tcBottom;
	int bitmapWidth;
	int bitmapRows;
	int bearingX;
	int bearingY;
	int advance; // in pixels
};

struct FtAtlas {
	r_tex_c* tex = nullptr;
	int curX = 1; // 1-pixel padding from edge
	int curY = 1;
	int rowHeight = 0;
};

struct r_font_c::FtCache {
	FT_Library library = nullptr;
	FT_Face face = nullptr;
	r_renderer_c* renderer = nullptr;
	std::vector<FtAtlas> atlases;
	// Key: (pixelSize << 21) | codepoint (supports up to 2M codepoints and heights up to 2048)
	std::unordered_map<uint64_t, FtCachedGlyph> cache;

	bool Init(r_renderer_c* rend, const std::string& ttfPath) {
		renderer = rend;
		if (FT_Init_FreeType(&library)) {
			rend->sys->con->Warning("FreeType: failed to initialize library");
			return false;
		}
		if (FT_New_Face(library, ttfPath.c_str(), 0, &face)) {
			rend->sys->con->Warning("FreeType: failed to load font '%s'", ttfPath.c_str());
			FT_Done_FreeType(library);
			library = nullptr;
			return false;
		}
		rend->sys->con->Printf("FreeType: loaded CJK fallback font '%s'", ttfPath.c_str());
		return true;
	}

	~FtCache() {
		for (auto& atlas : atlases) {
			delete atlas.tex;
		}
		if (face) FT_Done_Face(face);
		if (library) FT_Done_FreeType(library);
	}

	FtAtlas& EnsureAtlas(int glyphW, int glyphH) {
		if (!atlases.empty()) {
			auto& last = atlases.back();
			// Check if glyph fits in current row
			if (last.curX + glyphW + 1 <= FT_ATLAS_SIZE &&
				last.curY + (std::max)(last.rowHeight, glyphH) + 1 <= FT_ATLAS_SIZE) {
				return last;
			}
			// Try starting a new row
			if (last.curY + last.rowHeight + glyphH + 2 <= FT_ATLAS_SIZE) {
				last.curX = 1;
				last.curY += last.rowHeight + 1;
				last.rowHeight = 0;
				return last;
			}
		}

		// Create new atlas
		FtAtlas atlas;
		auto img = std::make_unique<image_c>();
		std::vector<byte> zeros(FT_ATLAS_SIZE * FT_ATLAS_SIZE * 4, 0);
		img->CopyRaw(IMGTYPE_RGBA, FT_ATLAS_SIZE, FT_ATLAS_SIZE, zeros.data());
		atlas.tex = new r_tex_c(renderer->texMan, std::move(img), TF_NOMIPMAP | TF_CLAMP);
		atlas.tex->fileWidth = FT_ATLAS_SIZE;
		atlas.tex->fileHeight = FT_ATLAS_SIZE;
		atlases.push_back(atlas);

		renderer->sys->con->Printf("FreeType: created atlas #%d (%dx%d)",
			(int)atlases.size(), FT_ATLAS_SIZE, FT_ATLAS_SIZE);
		return atlases.back();
	}

	FtCachedGlyph* GetGlyph(char32_t cp, int pixelSize) {
		if (!face) return nullptr;

		uint64_t key = ((uint64_t)pixelSize << 21) | (uint32_t)cp;
		auto it = cache.find(key);
		if (it != cache.end()) return &it->second;

		// Set pixel size
		FT_Set_Pixel_Sizes(face, 0, pixelSize);

		// Load and render glyph
		if (FT_Load_Char(face, (FT_ULong)cp, FT_LOAD_RENDER)) {
			return nullptr;
		}

		FT_GlyphSlot slot = face->glyph;
		FT_Bitmap& bmp = slot->bitmap;

		int glyphW = (int)bmp.width;
		int glyphH = (int)bmp.rows;

		// Handle zero-size glyphs (e.g., spaces in CJK)
		if (glyphW == 0 || glyphH == 0) {
			FtCachedGlyph glyph{};
			glyph.atlasIndex = -1;
			glyph.bitmapWidth = 0;
			glyph.bitmapRows = 0;
			glyph.bearingX = slot->bitmap_left;
			glyph.bearingY = slot->bitmap_top;
			glyph.advance = (int)(slot->advance.x >> 6);
			auto [inserted, _] = cache.emplace(key, glyph);
			return &inserted->second;
		}

		// Find space in atlas
		FtAtlas& atlas = EnsureAtlas(glyphW, glyphH);
		int atlasIdx = (int)(atlases.size() - 1);
		int x = atlas.curX;
		int y = atlas.curY;

		// Prepare RGBA pixel data for this glyph
		std::vector<byte> glyphPixels(glyphW * glyphH * 4);
		for (int row = 0; row < glyphH; row++) {
			for (int col = 0; col < glyphW; col++) {
				int srcIdx = row * bmp.pitch + col;
				int dstIdx = (row * glyphW + col) * 4;
				byte gray = bmp.buffer[srcIdx];
				glyphPixels[dstIdx + 0] = 255;  // R
				glyphPixels[dstIdx + 1] = 255;  // G
				glyphPixels[dstIdx + 2] = 255;  // B
				glyphPixels[dstIdx + 3] = gray;  // A = grayscale coverage
			}
		}

		// Upload glyph to GPU atlas texture
		glBindTexture(atlas.tex->target, atlas.tex->texId);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		if (atlas.tex->target == GL_TEXTURE_2D_ARRAY) {
			glTexSubImage3D(atlas.tex->target, 0, x, y, 0,
				glyphW, glyphH, 1, GL_RGBA, GL_UNSIGNED_BYTE, glyphPixels.data());
		} else {
			glTexSubImage2D(atlas.tex->target, 0, x, y,
				glyphW, glyphH, GL_RGBA, GL_UNSIGNED_BYTE, glyphPixels.data());
		}

		// Calculate texture coordinates
		FtCachedGlyph glyph{};
		glyph.atlasIndex = atlasIdx;
		glyph.tcLeft = (float)x / FT_ATLAS_SIZE;
		glyph.tcRight = (float)(x + glyphW) / FT_ATLAS_SIZE;
		glyph.tcTop = (float)y / FT_ATLAS_SIZE;
		glyph.tcBottom = (float)(y + glyphH) / FT_ATLAS_SIZE;
		glyph.bitmapWidth = glyphW;
		glyph.bitmapRows = glyphH;
		glyph.bearingX = slot->bitmap_left;
		glyph.bearingY = slot->bitmap_top;
		glyph.advance = (int)(slot->advance.x >> 6);

		auto [inserted, _] = cache.emplace(key, glyph);

		// Advance atlas cursor
		atlas.curX += glyphW + 1;
		atlas.rowHeight = (std::max)(atlas.rowHeight, glyphH);

		return &inserted->second;
	}
};

// ===========
// Font Loader
// ===========

r_font_c::r_font_c(r_renderer_c* renderer, const char* fontName)
	: renderer(renderer)
{
	numFontHeight = 0;
	fontHeightMap = NULL;

	std::string fileNameBase = fmt::format(CFG_DATAPATH "Fonts/{}", fontName);

	// Open info file
	std::string tgfName = fileNameBase + ".tgf";
	std::ifstream tgf(tgfName);
	if (!tgf) {
		renderer->sys->con->Warning("font \"%s\" not found", fontName);
		return;
	}

	maxHeight = 0;
	f_fontHeight_s* fh = NULL;

	// Parse info file
	std::string sub;
	while (std::getline(tgf, sub)) {
		int h, x, y, w, sl, sr;
		if (sscanf(sub.c_str(), "HEIGHT %u;", &h) == 1) {
			// New height
			fh = new f_fontHeight_s;
			fontHeights[numFontHeight++] = fh;
			std::string tgaName = fmt::format("{}.{}.tga", fileNameBase, h);
			fh->tex = new r_tex_c(renderer->texMan, tgaName.c_str(), TF_NOMIPMAP);
			fh->height = h;
			if (h > maxHeight) {
				maxHeight = h;
			}
			fh->numGlyph = 0;
		}
		else if (fh && sscanf(sub.c_str(), "GLYPH %u %u %u %d %d;", &x, &y, &w, &sl, &sr) == 5) {
			// Add glyph
			if (fh->numGlyph >= 128) continue;
			f_glyph_s* glyph = &fh->glyphs[fh->numGlyph++];
			glyph->tcLeft = (float)x / fh->tex->fileWidth;
			glyph->tcRight = (float)(x + w) / fh->tex->fileWidth;
			glyph->tcTop = (float)y / fh->tex->fileHeight;
			glyph->tcBottom = (float)(y + fh->height) / fh->tex->fileHeight;
			glyph->width = w;
			glyph->spLeft = sl;
			glyph->spRight = sr;
		}
	}

	// Generate mapping of text height to font height
	fontHeightMap = new int[maxHeight + 1];
	memset(fontHeightMap, 0, sizeof(int) * (maxHeight + 1));
	for (int i = 0; i < numFontHeight; i++) {
		int gh = fontHeights[i]->height;
		for (int h = gh; h <= maxHeight; h++) {
			fontHeightMap[h] = i;
		}
		if (i > 0) {
			int belowH = fontHeights[i - 1]->height;
			int lim = (gh - belowH - 1) / 2;
			for (int b = 0; b < lim; b++) {
				fontHeightMap[gh - b - 1] = i;
			}
		}
	}

	// Try to initialize FreeType CJK fallback.
	// Search order:
	//   1. POB_ZH_FONTDIR/Fonts/  (set by the pob-zh host = our own dir; keeps the
	//      external POB folder pristine — fonts ship next to pob-zh.exe, not in POB)
	//   2. CFG_DATAPATH "Fonts/"  (CWD-relative = POB folder; legacy / ShadowPOB layout)
	std::vector<std::string> ttfCandidates;
	// Win32 read, not getenv: the /MT host sets this with SetEnvironmentVariableW,
	// which the UCRT's startup snapshot may predate (see translation_manager).
	char fontDirBuf[260];
	if (const char* fontDir = translation_win_env("POB_ZH_FONTDIR", fontDirBuf, sizeof(fontDirBuf))) {
		std::string base(fontDir);
		if (!base.empty() && base.back() != '/' && base.back() != '\\') {
			base += '/';
		}
		// User-selected font (set by the pob-zh host from pob-zh.ini) wins; then
		// the open-source default, then the legacy fonts.
		char fontFileBuf[128];
		if (const char* ff = translation_win_env("POB_ZH_FONTFILE", fontFileBuf, sizeof(fontFileBuf))) {
			if (ff[0]) ttfCandidates.push_back(base + "Fonts/" + ff);
		}
		ttfCandidates.push_back(base + "Fonts/NotoSansTC-Regular.ttf");
		ttfCandidates.push_back(base + "Fonts/FZ_ZY.ttf");
		ttfCandidates.push_back(base + "Fonts/CJKFallback.ttf");
	}
	ttfCandidates.push_back(CFG_DATAPATH "Fonts/NotoSansTC-Regular.ttf");
	ttfCandidates.push_back(CFG_DATAPATH "Fonts/FZ_ZY.ttf");
	ttfCandidates.push_back(CFG_DATAPATH "Fonts/CJKFallback.ttf");

	for (size_t i = 0; i < ttfCandidates.size(); i++) {
		std::ifstream test(ttfCandidates[i]);
		if (test.good()) {
			test.close();
			ftCache = std::make_unique<FtCache>();
			if (ftCache->Init(renderer, ttfCandidates[i].c_str())) {
				break;
			}
			ftCache.reset();
		}
	}
}

r_font_c::~r_font_c()
{
	// Delete textures
	for (int i = 0; i < numFontHeight; i++) {
		delete fontHeights[i]->tex;
		delete fontHeights[i];
	}
	delete fontHeightMap;
	// ftCache cleaned up by unique_ptr
}

// =============
// Font Renderer
// =============

std::u32string BuildTofuString(char32_t cp) {
	// Format unhandled Unicode codepoints like U+0123, higher planes have wider numbers.
	fmt::memory_buffer buf;
	fmt::format_to(fmt::appender(buf), "[U+{:04X}]", (uint32_t)cp);
	std::u32string ret;
	ret.reserve(buf.size());
	std::copy(buf.begin(), buf.end(), std::back_inserter(ret));
	return ret;
}

int const tofuSizeReduction = 3;

int r_font_c::StringWidthInternal(f_fontHeight_s* fh, std::u32string_view str, int height, float scale)
{
	int heightIdx = (int)(std::find(fontHeights, fontHeights + numFontHeight, fh) - fontHeights);
	auto tofuFont = FindSmallerFontHeight(height, heightIdx, tofuSizeReduction);

	auto measureCodepoint = [](f_fontHeight_s* glyphFh, char32_t cp) {
		auto& glyph = glyphFh->Glyph((char)(unsigned char)cp);
		return glyph.width + glyph.spLeft + glyph.spRight;
	};

	float width = 0.0f;
	for (size_t idx = 0; idx < str.size();) {
		auto ch = str[idx];
		int escLen = IsColorEscape(str.substr(idx));
		if (escLen) {
			idx += escLen;
		}
		else if (ch >= (unsigned)fh->numGlyph) {
			// Try FreeType fallback for CJK characters
			if (ftCache) {
				auto* ftGlyph = ftCache->GetGlyph(ch, fh->height);
				if (ftGlyph) {
					width += ftGlyph->advance * scale;
					width = std::ceil(width);
					++idx;
					continue;
				}
			}
			// Fallback to tofu
			auto tofu = BuildTofuString(ch);
			for (auto cp : tofu) {
				width += measureCodepoint(tofuFont.fh, cp);
				width = std::ceil(width);
			}
			++idx;
		}
		else if (ch == U'\t') {
			auto& glyph = fh->Glyph(' ');
			int spWidth = glyph.width + glyph.spLeft + glyph.spRight;
			width += spWidth * 4 * scale;
			width = std::ceil(width);
			++idx;
		}
		else {
			width += measureCodepoint(fh, ch) * scale;
			width = std::ceil(width);
			++idx;
		}
	}
	return static_cast<int>(width);
}

int r_font_c::StringWidth(int height, std::u32string_view str)
{
	auto mainFont = FindFontHeight(height);
	f_fontHeight_s* fh = mainFont.fh;
	int max = 0;
	float const scale = (float)height / fh->height;
	for (auto I = str.begin(); I != str.end(); ++I) {
		auto lineEnd = std::find(I, str.end(), U'\n');
		if (I != lineEnd) {
			std::u32string_view line(&*I, std::distance(I, lineEnd));
			int lw = StringWidthInternal(fh, line, height, scale);
			max = (std::max)(max, lw);
		}
		if (lineEnd == str.end()) {
			break;
		}
		I = lineEnd;
	}
	return max;
}

size_t r_font_c::StringCursorInternal(f_fontHeight_s* fh, std::u32string_view str, int height, float scale, int curX)
{
	int heightIdx = (int)(std::find(fontHeights, fontHeights + numFontHeight, fh) - fontHeights);
	auto tofuFont = FindSmallerFontHeight(height, heightIdx, tofuSizeReduction);

	auto measureCodepoint = [](f_fontHeight_s* glyphFh, char32_t cp) {
		auto& glyph = glyphFh->Glyph((char)(unsigned char)cp);
		return glyph.width + glyph.spLeft + glyph.spRight;
	};

	float x = 0.0f;
	auto I = str.begin();
	auto lineEnd = std::find(I, str.end(), U'\n');
	while (I != lineEnd) {
		auto tail = str.substr(std::distance(str.begin(), I));
		int escLen = IsColorEscape(tail);
		if (escLen) {
			I += escLen;
		}
		else if (*I >= (unsigned)fh->numGlyph) {
			// Try FreeType fallback
			if (ftCache) {
				auto* ftGlyph = ftCache->GetGlyph(*I, fh->height);
				if (ftGlyph) {
					x += ftGlyph->advance * scale;
					x = std::ceil(x);
					if (curX <= x) {
						return std::distance(str.begin(), I);
					}
					++I;
					continue;
				}
			}
			// Fallback to tofu
			auto tofu = BuildTofuString(*I);
			for (auto cp : tofu) {
				x += measureCodepoint(tofuFont.fh, cp);
				x = std::ceil(x);
				if (curX <= x) {
					return std::distance(str.begin(), I);
				}
			}
			++I;
		}
		else if (*I == U'\t') {
			auto& glyph = fh->Glyph(' ');
			float fullWidth = (glyph.width + glyph.spLeft + glyph.spRight) * 4.0f * scale;
			float halfWidth = std::ceil(fullWidth / 2.0f);
			x += halfWidth;
			x = std::ceil(x);
			if (curX <= x) {
				break;
			}
			x += fullWidth - halfWidth;
			x = std::ceil(x);
			if (curX <= x) {
				break;
			}
			++I;
		}
		else {
			x += measureCodepoint(fh, *I) * scale;
			x = std::ceil(x);
			if (curX <= x) {
				break;
			}
			++I;
		}
	}
	return std::distance(str.begin(), I);
}

int	r_font_c::StringCursorIndex(int height, std::u32string_view str, int curX, int curY)
{
	auto mainFont = FindFontHeight(height);
	f_fontHeight_s* fh = mainFont.fh;
	int lastIndex = 0;
	int lineY = height;
	float scale = (float)height / fh->height;

	auto I = str.begin();
	while (I != str.end()) {
		auto lineEnd = std::find(I, str.end(), U'\n');
		auto line = str.substr(std::distance(str.begin(), I), std::distance(I, lineEnd));
		lastIndex = (int)(StringCursorInternal(fh, line, height, scale, curX));
		if (curY <= lineY) {
			break;
		}
		if (lineEnd == str.end()) {
			break;
		}
		I = lineEnd + 1;
		lineY += height;
	}
	return (int)std::distance(str.begin(), I) + lastIndex;
}

r_font_c::EmbeddedFontSpec r_font_c::FindSmallerFontHeight(int height, int heightIdx, int sizeReduction) {
	EmbeddedFontSpec ret{};
	ret.fh = fontHeights[heightIdx];
	ret.yPad = 0;
	for (int tofuIdx = heightIdx - 1; tofuIdx >= 0; --tofuIdx) {
		auto candFh = fontHeights[tofuIdx];
		int heightDiff = height - candFh->height;
		if (heightDiff >= sizeReduction) {
			ret.fh = candFh;
			ret.yPad = (int)std::ceil(heightDiff / 2.0f);
			break;
		}
	}
	return ret;
}

r_font_c::FontHeightEntry r_font_c::FindFontHeight(int height) {
	FontHeightEntry ret{};
	if (height > maxHeight) {
		// Too large heights get the largest font size.
		ret.heightIdx = numFontHeight - 1;
	}
	else if (height < 0) {
		// Negative heights get the smallest font size.
		ret.heightIdx = 0;
	}
	else {
		ret.heightIdx = fontHeightMap[height];
	}
	ret.fh = fontHeights[ret.heightIdx];
	return ret;
}

void r_font_c::DrawTextLine(scp_t pos, int align, int height, col4_t col, std::u32string_view str)
{
	// Check if the line is visible
	if (pos[Y] >= renderer->sys->video->vid.size[1] || pos[Y] <= -height) {
		// Just process the colour codes
		while (!str.empty()) {
			// Check for escape character
			int escLen = IsColorEscape(str);
			if (escLen) {
				str = ReadColorEscape(str, col);
				col[3] = 1.0f;
				renderer->curLayer->Color(col);
				continue;
			}
			str = str.substr(1);
		}
		return;
	}

	// Find best height to use
	auto mainFont = FindFontHeight(height);
	f_fontHeight_s* fh = mainFont.fh;
	float scale = (float)height / fh->height;
	auto tofuFont = FindSmallerFontHeight(height, mainFont.heightIdx, tofuSizeReduction);

	// Calculate the string position
	float x = pos[X];
	float y = std::floor(pos[Y]);
	if (align != F_LEFT) {
		// Calculate the real width of the string
		float width = StringWidthInternal(fh, str, height, scale);
		switch (align) {
		case F_CENTRE:
			x = floor((renderer->VirtualScreenWidth() - width) / 2.0f + pos[X]);
			break;
		case F_RIGHT:
			x = floor(renderer->VirtualScreenWidth() - width - pos[X]);
			break;
		case F_CENTRE_X:
			x = floor(pos[X] - width / 2.0f);
			break;
		case F_RIGHT_X:
			x = floor(pos[X] - width);
			break;
		}
	}

	// Snap the starting x position to the pixel grid so the leading glyph isn't blurred.
	x = std::round(x);

	r_tex_c* curTex{};

	auto drawCodepoint = [this, &curTex, &x, y](f_fontHeight_s* fh, int height, float scale, int yShift, char32_t cp) {
		float cpY = y + yShift;
		if (curTex != fh->tex) {
			curTex = fh->tex;
			renderer->curLayer->Bind(fh->tex);
		}
		auto& glyph = fh->Glyph((char)(unsigned char)cp);
		x += glyph.spLeft * scale;
		if (glyph.width) {
			float w = glyph.width * scale;
			if (x + w >= 0 && x < renderer->VirtualScreenWidth()) {
				renderer->curLayer->Quad(
					glyph.tcLeft, glyph.tcTop, x, cpY,
					glyph.tcRight, glyph.tcTop, x + w, cpY,
					glyph.tcRight, glyph.tcBottom, x + w, cpY + height,
					glyph.tcLeft, glyph.tcBottom, x, cpY + height
				);
			}
			x += w;
		}
		x += glyph.spRight * scale;
		x = std::ceil(x);
	};

	// Lambda to draw a FreeType glyph
	auto drawFtGlyph = [this, &curTex, &x, y](FtCachedGlyph* ftg, int height, float scale) {
		if (ftg->atlasIndex < 0 || ftg->bitmapWidth == 0) {
			// Zero-width glyph (space-like)
			x += ftg->advance * scale;
			x = std::ceil(x);
			return;
		}

		auto* atlasTex = ftCache->atlases[ftg->atlasIndex].tex;
		if (curTex != atlasTex) {
			curTex = atlasTex;
			renderer->curLayer->Bind(atlasTex);
		}

		// Calculate position with proper baseline alignment
		// bearingY is distance from baseline to glyph top (positive = above baseline)
		// For TGA fonts, the glyph fills the full height with no bearing concept.
		// We need to align the FreeType glyph so its baseline matches the TGA font baseline.
		// Approximate: baseline is at about 80% of the font height from top.
		float baselineY = y + height * 0.82f;
		float glyphX = x + ftg->bearingX * scale;
		float glyphY = baselineY - ftg->bearingY * scale;
		float glyphW = ftg->bitmapWidth * scale;
		float glyphH = ftg->bitmapRows * scale;

		if (glyphX + glyphW >= 0 && glyphX < renderer->VirtualScreenWidth()) {
			renderer->curLayer->Quad(
				ftg->tcLeft, ftg->tcTop, glyphX, glyphY,
				ftg->tcRight, ftg->tcTop, glyphX + glyphW, glyphY,
				ftg->tcRight, ftg->tcBottom, glyphX + glyphW, glyphY + glyphH,
				ftg->tcLeft, ftg->tcBottom, glyphX, glyphY + glyphH
			);
		}

		x += ftg->advance * scale;
		x = std::ceil(x);
	};

	// Render the string
	for (auto tail = str; !tail.empty();) {
		auto ch = tail[0];

		// Check for escape character first
		int escLen = IsColorEscape(tail);
		if (escLen) {
			tail = ReadColorEscape(tail, col);
			col[3] = 1.0f;
			renderer->curLayer->Color(col);
			continue;
		}

		// Handle tabs
		if (ch == U'\t') {
			auto& glyph = fh->Glyph(' ');
			int spWidth = glyph.width + glyph.spLeft + glyph.spRight;
			x+= (spWidth << 2) * scale;
			tail = tail.substr(1);
			continue;
		}

		// Draw characters beyond the TGA glyph range
		if (ch >= (unsigned)fh->numGlyph) {
			// Try FreeType fallback
			if (ftCache) {
				auto* ftGlyph = ftCache->GetGlyph(ch, fh->height);
				if (ftGlyph) {
					drawFtGlyph(ftGlyph, height, scale);
					tail = tail.substr(1);
					continue;
				}
			}
			// Fallback to tofu
			auto tofu = BuildTofuString(ch);
			for (auto tch : tofu) {
				drawCodepoint(tofuFont.fh, tofuFont.fh->height, 1.0f, tofuFont.yPad, tch);
			}
			tail = tail.substr(1);
			continue;
		}

		// Draw normal ASCII glyph
		drawCodepoint(fh, height, scale, 0, ch);
		tail = tail.substr(1);
	}
}

void r_font_c::Draw(scp_t pos, int align, int height, col4_t col, std::u32string_view str)
{
	if (str.empty()) {
		pos[Y]+= height;
		return;
	}

	// Prepare for rendering
	renderer->curLayer->Color(col);

	// Separate into lines and render them
	for (auto I = str.begin(); I != str.end(); ++I) {
		auto lineEnd = std::find(I, str.end(), U'\n');
		if (I != lineEnd) {
			std::u32string_view line(&*I, std::distance(I, lineEnd));
			DrawTextLine(pos, align, height, col, line);
		}
		pos[Y] += height;
		if (lineEnd == str.end()) {
			break;
		}
		I = lineEnd;
	}
}

void r_font_c::FDraw(scp_t pos, int align, int height, col4_t col, const char* fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	VDraw(pos, align, height, col, fmt, va);
	va_end(va);
}

void r_font_c::VDraw(scp_t pos, int align, int height, col4_t col, const char* fmt, va_list va)
{
	char str[65536];
	vsnprintf(str, 65535, fmt, va);
	str[65535] = 0;
	auto idxStr = IndexUTF8ToUTF32(str);
	Draw(pos, align, height, col, idxStr.text);
}
