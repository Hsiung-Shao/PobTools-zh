// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Render Font Header
//

// =======
// Classes
// =======

#include <string_view>
#include <memory>

// Font
class r_font_c {
public:
	r_font_c(class r_renderer_c* renderer, const char* fontName);
	~r_font_c();

	int		StringWidth(int height, std::u32string_view str);
	int		StringCursorIndex(int height, std::u32string_view str, int curX, int curY);
	void	Draw(scp_t pos, int align, int height, col4_t col, std::u32string_view str);
	void	FDraw(scp_t pos, int align, int height, col4_t col, const char* fmt, ...);
	void	VDraw(scp_t pos, int align, int height, col4_t col, const char* fmt, va_list va);

private:
	int		StringWidthInternal(struct f_fontHeight_s* fh, std::u32string_view str, int height, float scale);
	size_t	StringCursorInternal(struct f_fontHeight_s* fh, std::u32string_view str, int height, float scale, int curX);
	void	DrawTextLine(scp_t pos, int align, int height, col4_t col, std::u32string_view str);

	struct EmbeddedFontSpec {
		f_fontHeight_s* fh;
		int yPad;
	};
	EmbeddedFontSpec FindSmallerFontHeight(int height, int heightIdx, int sizeReduction);

	struct FontHeightEntry {
		f_fontHeight_s* fh;
		int heightIdx;
	};
	FontHeightEntry FindFontHeight(int height);

	class r_renderer_c* renderer = nullptr;
	int		numFontHeight = 0;
	struct f_fontHeight_s *fontHeights[32] = {};
	int		maxHeight = 0;
	int*	fontHeightMap = nullptr;

	// FreeType CJK fallback
	struct FtCache;
	std::unique_ptr<FtCache> ftCache;
};
