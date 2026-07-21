// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Render Global Header
//

#include <glm/vec2.hpp>

// =======
// Classes
// =======

// Renderer feature flags
enum r_featureFlag_e {
	F_DPI_AWARE = 0x1, // App understands DPI, do not virtualize screen size/positions
};

// Font alignment
enum r_fontAlign_e {
	F_LEFT,
	F_CENTRE,
	F_RIGHT,
	F_CENTRE_X,
	F_RIGHT_X
};

// Fonts
enum r_fonts_e {
	F_FIXED,			// Monospaced: Bitsteam Vera Sans Mono
	F_VAR,				// Normal: Liberation Sans
	F_VAR_BOLD,			// Normal: Liberation Sans Bold
	F_FONTIN_SC,		// Normal: Fontin Small Caps
	F_FONTIN_SC_ITALIC,	// Normal: Fontin Small Caps Italic
	F_FONTIN,			// Normal: Fontin
	F_FONTIN_ITALIC,	// Normal: Fontin Italic
	F_NUMFONTS
};

// Texture flags
enum r_texFlag_e {	
	TF_CLAMP	= 0x01,	// Clamp texture
	TF_NOMIPMAP	= 0x02,	// No mipmaps
	TF_NEAREST	= 0x04,	// Use nearest-pixel magnification instead of linear
	TF_ASYNC	= 0x08, // Load asynchronously
};

// Blend modes
enum r_blendMode_e {
	RB_ALPHA,
	RB_PRE_ALPHA,
	RB_ADDITIVE
};

// Shader handle
class r_shaderHnd_c {
	friend class r_renderer_c;
public:
	~r_shaderHnd_c();

	std::optional<int> StackCount() const;
private:
	r_shaderHnd_c(class r_shader_c* sh);
	r_shader_c* sh;
};

// ==========
// Interfaces
// ==========

class image_c;

// Renderer: r_main.cpp
class r_IRenderer {
public:
	static r_IRenderer* GetHandle(sys_IMain* sysHnd);
	static void FreeHandle(r_IRenderer* hnd);

	virtual void	Init(r_featureFlag_e features) = 0;
	virtual void	Shutdown() = 0;

	virtual void	BeginFrame() = 0;
	virtual void	EndFrame() = 0;
	
	virtual r_shaderHnd_c* RegisterShader(std::string_view name, int flags) = 0;
	virtual r_shaderHnd_c* RegisterShaderFromImage(std::unique_ptr<image_c> img, int flags) = 0;
	virtual void	GetShaderImageSize(r_shaderHnd_c* hnd, int &width, int &height) = 0;
	virtual void	SetShaderLoadingPriority(r_shaderHnd_c* hnd, int pri) = 0;
	virtual void	PurgeShaders() = 0;
	virtual int		GetTexAsyncCount() = 0;

	virtual void	SetClearColor(const col4_t col) = 0;
	virtual void	SetDrawLayer(int layer, int subLayer = 0) = 0;
	virtual void	SetDrawSubLayer(int subLayer) = 0;
	virtual int		GetDrawLayer() = 0;
	virtual void	SetViewport(int x = 0, int y = 0, int width = 0, int height = 0) = 0;
	virtual void	SetBlendMode(int mode) = 0;
	virtual void	DrawColor(const col4_t col = NULL) = 0;
	virtual void	DrawColor(dword col) = 0;
	virtual void	GetDrawColor(col4_t color) = 0;
	virtual void	DrawImage(r_shaderHnd_c* hnd, glm::vec2 pos, glm::vec2 extent, glm::vec2 uv1 = { 0, 0 }, glm::vec2 uv2 = { 1, 1 }, int stackLayer = 0, std::optional<int> maskLayer = {}) = 0;
	virtual void	DrawImageQuad(r_shaderHnd_c* hnd, glm::vec2 p0, glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, glm::vec2 uv0 = { 0, 0 }, glm::vec2 uv1 = { 1, 0 }, glm::vec2 uv2 = { 1, 1 }, glm::vec2 uv3 = { 0, 1 }, int stackLayer = 0, std::optional<int> maskLayer = {}) = 0;
	virtual void	DrawString(float x, float y, int align, int height, const col4_t col, int font, const char* str) = 0;
	virtual void	DrawStringFormat(float x, float y, int align, int height, const col4_t col, int font, const char* fmt, ...) = 0;
	virtual int		DrawStringWidth(int height, int font, const char* str) = 0;
	virtual int		DrawStringCursorIndex(int height, int font, const char* str, int curX, int curY) = 0;

	virtual int		VirtualScreenWidth() = 0;
	virtual int		VirtualScreenHeight() = 0;
	virtual float	VirtualScreenScaleFactor() = 0;
	virtual void	SetDpiScaleOverridePercent(int percent) = 0;
	virtual int		DpiScaleOverridePercent() const = 0;
	virtual int		VirtualMap(int properValue) = 0;
	virtual int		VirtualUnmap(int mappedValue) = 0;
	
	virtual void	ToggleDebugImGui() = 0;
};
