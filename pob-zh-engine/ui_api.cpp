// DyLua: SimpleGraphic
// (c) David Gowor, 2014
//
// Module: UI API
//

#include "ui_local.h"

#include <filesystem>
#include <fstream>
#include <vector>
#include <zlib.h>
#include <cmath>
#include <cstring>
#include <cstdio>

#include "core/core_tex_manipulation.h"
#include "translation_manager.h"

/* OnFrame()
** OnChar("<char>")
** OnKeyDown("<keyName>")
** OnKeyUp("<keyName>")
** canExit = CanExit()
** OnExit()
** OnSubCall("<name>", ...)
** OnSubError(ssID, "<errorMsg>")
** OnSubFinished(ssID, ...)
**
** SetCallback("<name>"[, func])
** func = GetCallback("<name>")
** SetMainObject(object)
**
** artHandle = NewArtHandle("<filename>")
** width, height = artHandle:Size()
**
** imgHandle = NewImageHandle()
** imgHandle:Load("<fileName>"[, "flag1"[, "flag2"...]])  flag:{"ASYNC"|"CLAMP"|"MIPMAP"}
** imgHandle:LoadArtRectangle(art, x1, y1, x2, y2[, "flag1"[, "flag2"... ]])  flag:{"CLAMP"|"MIPMAP"}
** imgHandle:LoadArtArcBand(art, xC, yC, rMin, rMax[, "flag1"[, "flag2"... ]])  flag:{"CLAMP"|"MIPMAP"}
** imgHandle:Unload()
** isValid = imgHandle:IsValid()
** isLoading = imgHandle:IsLoading()
** imgHandle:SetLoadingPriority(pri)
** width, height = imgHandle:ImageSize()
**
** texHandle = NewTexHandle()
** texHandle:Allocate(format, width, height, layerCount, mipCount)
** texHandle:Load("<fileName>")
** texHandle:Save("<fileName>")
** info = texHandle:Info()
** isvalid = texHandle:IsValid()
** texHandle:StackTextures({tex1, tex2, .. texN})  -- all textures must be same shape and format
** -- texHandle:SetLayer(srcTexHandle, layer)
** -- texHandle:CopyImage(srcTexHandle, targetX, targetY)
** -- texHandle:Transcode(newFormat)
** -- texHandle:GenerateMipmaps()
**
** RenderInit(["flag1"[, "flag2"...]])  flag:{"DPI_AWARE"}
** width, height = GetScreenSize()
** scaleFactor = GetScreenScale()
** SetClearColor(red, green, blue[, alpha])
** SetDrawLayer({layer|nil}[, subLayer)
** GetDrawLayer()
** SetViewport([x, y, width, height])
** SetDrawColor(red, green, blue[, alpha]) / SetDrawColor("<escapeStr>")
** DrawImage({imgHandle|nil}, left, top, width, height[, tcLeft, tcTop, tcRight, tcBottom][, stackIdx[, maskIdx]])  maskIdx: use a stack layer as multiplicative mask
** DrawImageQuad({imgHandle|nil}, x1, y1, x2, y2, x3, y3, x4, y4[, s1, t1, s2, t2, s3, t3, s4, t4][, stackIdx[, maskIdx]])
** DrawString(left, top, align{"LEFT"|"CENTER"|"RIGHT"|"CENTER_X"|"RIGHT_X"}, height, font{"FIXED"|"VAR"|"VAR BOLD"|"FONTIN SC"|"FONTIN SC ITALIC"|"FONTIN"|"FONTIN ITALIC"}, "<text>")
** width = DrawStringWidth(height, font{"FIXED"|"VAR"|"VAR BOLD"|"FONTIN SC"|"FONTIN SC ITALIC"|"FONTIN"|"FONTIN ITALIC"}, "<text>")
** index = DrawStringCursorIndex(height, font{"FIXED"|"VAR"|"VAR BOLD"|"FONTIN SC"|"FONTIN SC ITALIC"|"FONTIN"|"FONTIN ITALIC"}, "<text>", cursorX, cursorY)
** str = StripEscapes("<string>")
** count = GetAsyncCount()
**
** searchHandle = NewFileSearch("<spec>"[, findDirectories])
** found = searchHandle:NextFile()
** fileName = searchHandle:GetFileName()
** fileSize = searchHandle:GetFileSize()
** modified, date, time = searchHande:GetFileModifiedTime()
**
** provider, version, status = GetCloudProvider(path)
**
** SetWindowTitle("<title>")
** x, y = GetCursorPos()
** SetCursorPos(x, y)
** ShowCursor(doShow)
** down = IsKeyDown("<keyName>")
** Copy("<string>")
** string = Paste()
** compressed = Deflate(uncompressed)
** uncompressed = Inflate(compressed)
** msec = GetTime()
** path[, pathACP[, err]] = GetScriptPath()
** path[, pathACP[, err]] = GetRuntimePath()
** path[, pathACP[, err]] = GetUserPath() -- may return nil if the user path could not be determined
** SetWorkDir("<path>")
** path = GetWorkDir()
** ssID = LaunchSubScript("<scriptText>", "<funcList>", "<subList>"[, ...])
** AbortSubScript(ssID)
** isRunning = IsSubScriptRunning(ssID)
** ... = LoadModule("<modName>"[, ...])
** err, ... = PLoadModule("<modName>"[, ...])
** err, ... = PCall(func[, ...])
** ConPrintf("<format>"[, ...])
** ConPrintTable(table[, noRecurse])
** ConExecute("<cmd>")
** SpawnProcess("<cmdName>"[, "<args>"])
** err = OpenURL("<url>")
** SetProfiling(isEnabled)
** Restart()
** Exit(["<message>"])
** SetForeground()
*/

// Grab UI main pointer from the registry
static ui_main_c* GetUIPtr(lua_State* L)
{
	lua_geti(L, LUA_REGISTRYINDEX, ui_main_c::REGISTRY_KEY);
	ui_main_c* ui = (ui_main_c*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	return ui;
}

// ===============
// C++ scaffolding
// ===============

/*
* ui->LAssert transfers control immediately out of the function without destroying
* any C++ objects. To support RAII this scaffolding serves as a landing pad for
* ui->LExpect, to transfer control to Lua but only after the call stack has been
* unwound with normal C++ exception semantics.
* 
* Example use site:
* SG_LUA_CPP_FUN_BEGIN(DoTheThing)
* {
*   ui_main_c* ui = GetUIPtr(L);
*	auto foo = std::make_shared<Foo>();
*   ui->LExpect(L, lua_gettop(L) >= 1), "Usage: DoTheThing(x)");
*   ui->LExpect(L, lua_isstring(L, 1), "DoTheThing() argument 1: expected string, got %s", luaL_typename(L, 1));
*	return 0;
* }
* SG_LUA_CPP_FUN_END()
*/

#ifdef _WIN32
#define SG_NOINLINE __declspec(noinline)
#else
#define SG_NOINLINE [[gnu::noinline]]
#endif
#define SG_NORETURN [[noreturn]]

SG_NORETURN static void LuaErrorWrapper(lua_State* L)
{
	lua_error(L);
}

#define SG_LUA_CPP_FUN_BEGIN(Name)                                 \
static int l_##Name(lua_State* L) {                                \
	int (*fun)(lua_State*) = [](lua_State* L) SG_NOINLINE -> int { \
		try

#define SG_LUA_CPP_FUN_END()                                    \
		catch (ui_expectationFailed_s) { return -1; }           \
		catch (std::exception& e) {                             \
			lua_pushfstring(L, "C++ exception:\n%s", e.what()); \
			return -1;                                          \
		}                                                       \
    };                                                          \
	int rc = fun(L);                                            \
	if (rc < 0) { LuaErrorWrapper(L); }                         \
	return rc; }

// ===============
// Data validation
// ===============

/*
* ui_luaReader_c wraps the common validation of arguments or values from Lua in a class
* that ensures a consistent assertion message and reduces the risk of mistakes in
* parameter validation.
*
* As it has scoped RAII resources and uses ui->LExcept() it must only be used in functions
* exposed to Lua through the SG_LUA_CPP_FUN_BEGIN/END scheme as that ensures proper cleanup
* when unwinding.
*
* Example use site:
* SG_LUA_CPP_FUN_BEGIN(DoTheThing)
* {
*   ui_main_c* ui = GetUIPtr(L);
*   ui_luaReader_c reader(ui, L, "DoTheThing");
*   ui->LExpect(L, lua_gettop(L) >= 2), "Usage: DoTheThing(table, number)");
*   reader.ArgCheckTable(1); // short-hand to validate formal arguments to function
*   reader.ArgCheckNumber(2); // -''-
*   reader.ValCheckNumber(-1, "descriptive name"); // validates any value on the Lua stack, indicating what the value represents
*   // Do the thing
*   return 0;
* }
* SG_LUA_CPP_FUN_END()
*/

class ui_luaReader_c {
public:
	ui_luaReader_c(ui_main_c* ui, lua_State* L, std::string funName) : ui(ui), L(L), funName(funName) {}

	// Always zero terminated as all regular strings are terminated in Lua.
	std::string_view ArgToString(int k) {
		ui->LExpect(L, lua_isstring(L, k), "%s() argument %d: expected string, got %s",
			funName.c_str(), k, luaL_typename(L, k));
		return lua_tostring(L, k);
	}

	void ArgCheckTable(int k) {
		ui->LExpect(L, lua_istable(L, k), "%s() argument %d: expected table, got %s",
			funName.c_str(), k, luaL_typename(L, k));
	}

	void ArgCheckNumber(int k) {
		ui->LExpect(L, lua_isnumber(L, k), "%s() argument %d: expected number, got %s",
			funName.c_str(), k, luaL_typename(L, k));
	}

	void ValCheckNumber(int k, char const* ctx) {
		ui->LExpect(L, lua_isnumber(L, k), "%s() %s: expected number, got %s",
			funName.c_str(), ctx, k, luaL_typename(L, k));
	}

private:
	ui_main_c* ui;
	lua_State* L;
	std::string funName;
};

// =========
// Callbacks
// =========

static int l_SetCallback(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: SetCallback(name[, func])");
	ui->LAssert(L, lua_isstring(L, 1), "SetCallback() argument 1: expected string, got %s", luaL_typename(L, 1));
	lua_pushvalue(L, 1);
	if (n >= 2) {
		ui->LAssert(L, lua_isfunction(L, 2) || lua_isnil(L, 2), "SetCallback() argument 2: expected function or nil, got %s", luaL_typename(L, 2));
		lua_pushvalue(L, 2);
	}
	else {
		lua_pushnil(L);
	}
	lua_settable(L, lua_upvalueindex(1));
	return 0;
}

static int l_GetCallback(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: GetCallback(name)");
	ui->LAssert(L, lua_isstring(L, 1), "GetCallback() argument 1: expected string, got %s", luaL_typename(L, 1));
	lua_pushvalue(L, 1);
	lua_gettable(L, lua_upvalueindex(1));
	return 1;
}

static int l_SetMainObject(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	lua_pushstring(L, "MainObject");
	if (n >= 1) {
		ui->LAssert(L, lua_istable(L, 1) || lua_isnil(L, 1), "SetMainObject() argument 1: expected table or nil, got %s", luaL_typename(L, 1));
		lua_pushvalue(L, 1);
	}
	else {
		lua_pushnil(L);
	}
	lua_settable(L, lua_upvalueindex(1));
	return 0;
}

// ===========
// Art Handles
// ===========

/*
* An art handle contains CPU-side image data, typically sourced from a file on disk.
* This differs from image handles that represent a texture resident on the GPU.
*
* Art handles can be used to produce image handles by slicing out masked subimages.
* Their primary intent is to decompose nested artwork like the connector art for
* modern revisions of the passive skill tree which has multiple orbit arcs inside
* of each other in a single image file.
*/

struct artHandle_s {
	std::unique_ptr<image_c> img;
};

SG_LUA_CPP_FUN_BEGIN(NewArtHandle)
{
	ui_main_c* ui = GetUIPtr(L);
	ui_luaReader_c reader(ui, L, "NewArtHandle");

	int n = lua_gettop(L);
	ui->LExpect(L, n >= 1, "Usage: NewArtHandle(fileName)");
	std::filesystem::path filePath = std::filesystem::u8path(reader.ArgToString(1));
	if (filePath.is_relative())
		filePath = ui->scriptWorkDir / filePath;
	std::unique_ptr<image_c> img(image_c::LoaderForFile(ui->sys->con, filePath));
	if (!img)
		return 0;

	if (img->Load(filePath))
		return 0;

	const auto format = img->tex.format();
	if (is_compressed(format) || !is_unsigned(format))
		return 0;

	const auto comp = component_count(format);
	if (comp != 1 && comp != 3 && comp != 4)
		return 0;

	artHandle_s* artHandle = (artHandle_s*)lua_newuserdata(L, sizeof(artHandle_s));
	new(artHandle) artHandle_s();
	artHandle->img = std::move(img);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_setmetatable(L, -2);
	return 1;
}
SG_LUA_CPP_FUN_END()

static artHandle_s* GetArtHandle(lua_State* L, ui_main_c* ui, const char* method)
{
	ui->LAssert(L, ui->IsUserData(L, 1, "uiarthandlemeta"), "artHandle:%s() must be used on an image handle", method);
	artHandle_s* artHandle = (artHandle_s*)lua_touserdata(L, 1);
	lua_remove(L, 1);
	return artHandle;
}

static int l_artHandleGC(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	artHandle_s* artHandle = GetArtHandle(L, ui, "__gc");
	artHandle->~artHandle_s();
	return 0;
}

static int l_artHandleSize(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	artHandle_s* artHandle = GetArtHandle(L, ui, "Size");

	const auto extent = artHandle->img ? artHandle->img->tex.extent() : gli::texture2d_array::extent_type{0, 0};
	lua_pushinteger(L, extent.x);
	lua_pushinteger(L, extent.y);
	return 2;
}

// =============
// Image Handles
// =============

struct imgHandle_s {
	r_shaderHnd_c* hnd;
};

static int l_NewImageHandle(lua_State* L)
{
	imgHandle_s* imgHandle = (imgHandle_s*)lua_newuserdata(L, sizeof(imgHandle_s));
	imgHandle->hnd = NULL;
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_setmetatable(L, -2);
	return 1;
}

static imgHandle_s* GetImgHandle(lua_State* L, ui_main_c* ui, const char* method, bool loaded)
{
	ui->LAssert(L, ui->IsUserData(L, 1, "uiimghandlemeta"), "imgHandle:%s() must be used on an image handle", method);
	imgHandle_s* imgHandle = (imgHandle_s*)lua_touserdata(L, 1);
	lua_remove(L, 1);
	if (loaded) {
		ui->LAssert(L, imgHandle->hnd != NULL, "imgHandle:%s(): image handle has no image loaded", method);
	}
	return imgHandle;
}

static int l_imgHandleGC(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	imgHandle_s* imgHandle = GetImgHandle(L, ui, "__gc", false);
	delete imgHandle->hnd;
	return 0;
}

SG_LUA_CPP_FUN_BEGIN(imgHandleLoad)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LExpect(L, ui->renderer != NULL, "Renderer is not initialised");
	imgHandle_s* imgHandle = GetImgHandle(L, ui, "Load", false);
	int n = lua_gettop(L);
	ui->LExpect(L, n >= 1, "Usage: imgHandle:Load(fileName[, flag1[, flag2...]])");
	ui->LExpect(L, lua_isstring(L, 1), "imgHandle:Load() argument 1: expected string, got %s", luaL_typename(L, 1));
	auto fileName = std::filesystem::u8path(lua_tostring(L, 1));
	if (!fileName.is_absolute() && !ui->scriptWorkDir.empty()) {
		fileName = ui->scriptWorkDir / fileName;
	}
	delete imgHandle->hnd;
	int flags = TF_NOMIPMAP;
	for (int f = 2; f <= n; f++) {
		if (!lua_isstring(L, f)) {
			continue;
		}
		std::string flag = lua_tostring(L, f);
		if (flag == "ASYNC") {
			flags |= TF_ASYNC;
		}
		else if (flag == "CLAMP") {
			flags |= TF_CLAMP;
		}
		else if (flag == "MIPMAP") {
			flags &= ~TF_NOMIPMAP;
		}
		else if (flag == "NEAREST") {
			flags |= TF_NEAREST;
		}
		else {
			ui->LExpect(L, 0, "imgHandle:Load(): unrecognised flag '%s'", flag.c_str());
		}
	}
	// TODO(LV): should we use u8path throughout here, to support any callers that use paths outside of working directory?
	imgHandle->hnd = ui->renderer->RegisterShader(fileName.generic_u8string(), flags);
	return 0;
}
SG_LUA_CPP_FUN_END()

static int l_imgHandleUnload(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	imgHandle_s* imgHandle = GetImgHandle(L, ui, "Unload", false);
	delete imgHandle->hnd;
	imgHandle->hnd = NULL;
	return 0;
}

static int l_imgHandleIsValid(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	imgHandle_s* imgHandle = GetImgHandle(L, ui, "IsValid", false);
	lua_pushboolean(L, imgHandle->hnd != NULL);
	return 1;
}

static int l_imgHandleIsLoading(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	imgHandle_s* imgHandle = GetImgHandle(L, ui, "IsLoading", true);
	int width, height;
	ui->renderer->GetShaderImageSize(imgHandle->hnd, width, height);
	lua_pushboolean(L, width == 0);
	return 1;
}

static int l_imgHandleSetLoadingPriority(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	imgHandle_s* imgHandle = GetImgHandle(L, ui, "SetLoadingPriority", true);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: imgHandle:SetLoadingPriority(pri)");
	ui->LAssert(L, lua_isnumber(L, 1), "imgHandle:SetLoadingPriority() argument 1: expected number, got %s", luaL_typename(L, 1));
	ui->renderer->SetShaderLoadingPriority(imgHandle->hnd, (int)lua_tointeger(L, 1));
	return 0;
}

static int l_imgHandleImageSize(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	imgHandle_s* imgHandle = GetImgHandle(L, ui, "ImageSize", true);
	int width, height;
	ui->renderer->GetShaderImageSize(imgHandle->hnd, width, height);
	lua_pushinteger(L, width);
	lua_pushinteger(L, height);
	return 2;
}

namespace {
	int ParseArtFlags(ui_main_c* ui, lua_State* L, int k, int n)
	{
		int flags = TF_NOMIPMAP;
		for (int f = k; f <= n; f++) {
			if (!lua_isstring(L, f)) {
				continue;
			}
			const char* flag = lua_tostring(L, f);
			if (!strcmp(flag, "CLAMP")) {
				flags |= TF_CLAMP;
			}
			else if (!strcmp(flag, "MIPMAP")) {
				flags &= ~TF_NOMIPMAP;
			}
			else if (!strcmp(flag, "NEAREST")) {
				flags |= TF_NEAREST;
			}
			else {
				ui->LExpect(L, 0, "imgHandle:LoadArtRectangle(): unrecognised flag '%s'", flag);
			}
		}
		return flags;
	}

	r_shaderHnd_c* RegisterShaderFromImage(r_IRenderer& renderer, std::unique_ptr<image_c> img, int flags)
	{
		return renderer.RegisterShaderFromImage(std::move(img), flags);
	}
}

SG_LUA_CPP_FUN_BEGIN(imgHandleLoadArtRectangle)
{
	ui_main_c* ui = GetUIPtr(L);
	imgHandle_s* imgHandle = GetImgHandle(L, ui, "LoadArtRectangle", false);

	const int n = lua_gettop(L);
	ui->LExpect(L, n >= 5, "Usage: imgHandle:LoadArtRectangle(art, x1, y1, x2, y2[, flag1[, flag2...]])");

	ui_luaReader_c reader(ui, L, "imgHandle::LoadArtRectangle");
	reader.ArgCheckNumber(2);
	reader.ArgCheckNumber(3);
	reader.ArgCheckNumber(4);
	reader.ArgCheckNumber(5);
	int x1 = (int)lua_tointeger(L, 2);
	int y1 = (int)lua_tointeger(L, 3);
	int x2 = (int)lua_tointeger(L, 4);
	int y2 = (int)lua_tointeger(L, 5);

	// Grab the art handle after extracting the parameters so that their error messages have the correct indices.
	artHandle_s* artHandle = GetArtHandle(L, ui, "LoadArtRectangle");

	if (x1 > x2)
		std::swap(x1, x2);
	if (y1 > y2)
		std::swap(y1, y2);

	auto* srcImg = artHandle->img.get();
	const auto srcFormat = srcImg->tex.format();
	auto extent = srcImg->tex.extent();
	const int srcWidth = extent.x;
	const int srcHeight = extent.y;
	const int comp = (int)component_count(srcFormat);
	ui->LExpect(L, x1 >= 0 && x2 <= srcWidth, "imgHandle:LoadArtRectangle(): X range %d to %d outside of the 0 to %d bounds", x1, x2, srcWidth);
	ui->LExpect(L, y1 >= 0 && y2 <= srcHeight, "imgHandle:LoadArtRectangle(): Y range %d to %d outside of the 0 to %d bounds", y1, y2, srcHeight);

	// Slice rectangle into temporary target image.
	auto dstImg = std::make_unique<image_c>(ui->sys->con);
	const int dstWidth = x2 - x1;
	const int dstHeight = y2 - y1;

	const int srcStride = srcWidth * comp;
	const int dstStride = dstWidth * comp;
	const int dstByteCount = dstHeight * dstStride;
	dstImg->tex = gli::texture2d_array(srcFormat, glm::ivec2(dstWidth, dstHeight), 1, 1);

	byte* srcPtr = srcImg->tex.data<byte>(0, 0, 0) + y1 * srcStride + x1 * comp;
	byte* dstPtr = dstImg->tex.data<byte>(0, 0, 0);
	for (int col = 0; col < (int)dstWidth; ++col) {
		for (int row = 0; row < (int)dstHeight; ++row) {
			memcpy(dstPtr, srcPtr, dstStride);
			srcPtr += srcStride;
			dstPtr += dstStride;
		}
	}

	const int flags = ParseArtFlags(ui, L, 5, n);
	delete imgHandle->hnd;
	imgHandle->hnd = RegisterShaderFromImage(*ui->renderer, std::move(dstImg), flags);

	return 0;
}
SG_LUA_CPP_FUN_END()

SG_LUA_CPP_FUN_BEGIN(imgHandleLoadArtArcBand)
{
	ui_main_c* ui = GetUIPtr(L);
	imgHandle_s* imgHandle = GetImgHandle(L, ui, "LoadArtArcBand", false);

	const int n = lua_gettop(L);
	ui->LExpect(L, n >= 5, "Usage: imgHandle:LoadArtArcBand(art, xC, yC, rMin, rMax[, flag1[, flag2...]])");

	ui_luaReader_c reader(ui, L, "imgHandle::LoadArtArcBand");
	reader.ArgCheckNumber(2);
	reader.ArgCheckNumber(3);
	reader.ArgCheckNumber(4);
	reader.ArgCheckNumber(5);
	const int xC = (int)lua_tointeger(L, 2);
	const int yC = (int)lua_tointeger(L, 3);
	int rMin = (int)lua_tointeger(L, 4);
	int rMax = (int)lua_tointeger(L, 5);

	if (rMin > rMax)
		std::swap(rMin, rMax);

	const int x1 = xC - rMax;
	const int y1 = yC - rMax;

	// Grab the art handle after extracting the parameters so that their error messages have the correct indices.
	artHandle_s* artHandle = GetArtHandle(L, ui, "LoadArtArcBand");

	auto* srcImg = artHandle->img.get();
	const auto srcFormat = srcImg->tex.format();
	const auto srcExtent = srcImg->tex.extent();
	const int srcWidth = srcExtent.x;
	const int srcHeight = srcExtent.y;
	const int comp = (int)component_count(srcFormat);
	ui->LExpect(L, xC >= 0 && xC <= srcWidth, "imgHandle:LoadArtArcBand(): X origin %d outside of the 0 to %d bounds", xC, srcWidth);
	ui->LExpect(L, yC >= 0 && yC <= srcHeight, "imgHandle:LoadArtArcBand(): Y origin %d outside of the 0 to %d bounds", yC, srcHeight);

	ui->LExpect(L, x1 >= 0 && x1 <= srcWidth, "imgHandle:LoadArtArcBand(): X corner %d outside of the 0 to %d bounds", x1, srcWidth);
	ui->LExpect(L, y1 >= 0 && y1 <= srcHeight, "imgHandle:LoadArtArcBand(): Y corner %d outside of the 0 to %d bounds", y1, srcHeight);


	// Slice rectangle into temporary target image.
	auto dstImg = std::make_unique<image_c>(ui->sys->con);
	const int dstWidth = xC - x1;
	const int dstHeight = yC - y1;

	const int srcStride = srcWidth * comp;
	const int dstStride = dstWidth * comp;
	const int dstByteCount = dstHeight * dstStride;
	dstImg->tex = gli::texture2d_array(srcFormat, glm::ivec2(dstWidth, dstHeight), 1, 1);

	const byte* srcData = srcImg->tex.data<byte>(0, 0, 0);
	byte* dstData = dstImg->tex.data<byte>(0, 0, 0);
	memset(dstData, 0x00, dstByteCount);

	// Copy all pixels whose center are between the two radii, inclusive.
	{
		// By doubling all coordinates, we can reference both pixel edges and pixel centers.
		// Even numbers are between pixel samples, odd numbers are on pixel samples.
		// This makes the distance test math more robust.
		// As this is ad-hoc 31.1 bit fixed point math, we should technically shift down the result of
		// the multiplications that go into the squares but as the same number of operations occur on both
		// sides of the squared equalities, it's fine. Just something to keep in mind for future changes.
		const int rMinSq = (rMin * 2) * (rMin * 2), rMaxSq = (rMax * 2) * (rMax * 2);
		const int width = dstWidth * 2, height = dstHeight * 2;
		for (int row = 1; row < height; row += 2)
		{
			const int dy = height - row;
			// Find the first pixel center that is inside the outer radius
			int colLo = -1;
			for (int x = 1; x < width; x += 2) {
				const int dx = width - x;
				const int rSq = dx * dx + dy * dy;
				if (rSq <= rMaxSq) {
					colLo = x;
					break;
				}
			}
			
			// If no pixel was found to be inside, the row does not contribute.
			if (colLo == -1)
				continue;

			int colHi = width;
			// Find the first pixel center that is inside the inner radius
			for (int x = colLo; x < width; x += 2) {
				const int dx = width - x;
				const int rSq = dx * dx + dy * dy;
				if (rSq < rMinSq) {
					colHi = x;
					break;
				}
			}

			// We now have a half-open span of touched pixel centers (or the far border).
			// Convert that to regular pixel coordinates and copy to the destination image.
			const int xLo = colLo / 2;
			const int xHi = colHi / 2;

			if (xLo != xHi) {
				const int y = row / 2;
				const int spanByteSize = (xHi - xLo) * comp;
				const int srcRow = y1 + y;
				const int dstRow = y;
				const int srcCol = x1 + xLo;
				const int dstCol = xLo;
				const byte* srcPtr = srcData + srcRow * srcStride + srcCol * comp;
				byte* dstPtr = dstData + dstRow * dstStride + dstCol * comp;
				memcpy(dstPtr, srcPtr, spanByteSize);
			}
		}
	}

	const int flags = ParseArtFlags(ui, L, 5, n);
	delete imgHandle->hnd;
	imgHandle->hnd = RegisterShaderFromImage(*ui->renderer, std::move(dstImg), flags);

	return 0;
}
SG_LUA_CPP_FUN_END()

// =========
// Rendering
// =========

static int l_RenderInit(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	bool dpiAware = false;
	for (int i = 1; i <= n; ++i) {
		ui->LAssert(L, lua_isstring(L, i), "RenderInit() argument %d: expected string, got %s", i, luaL_typename(L, i));
		char const* str = lua_tostring(L, i);
		if (strcmp(str, "DPI_AWARE") == 0) {
			dpiAware = true;
		}
	}
	r_featureFlag_e features{};
	if (dpiAware) {
		features = (r_featureFlag_e)(features | F_DPI_AWARE);
	}
	ui->RenderInit(features);
	return 0;
}

static int l_GetScreenSize(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	lua_pushinteger(L, ui->renderer->VirtualScreenWidth());
	lua_pushinteger(L, ui->renderer->VirtualScreenHeight());
	return 2;
}

static int l_GetScreenScale(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	lua_pushnumber(L, ui->renderer->VirtualScreenScaleFactor());
	return 1;
}

static int l_SetClearColor(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LAssert(L, ui->renderer != NULL, "Renderer is not initialised");
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 3, "Usage: SetClearColor(red, green, blue[, alpha])");
	col4_t color;
	for (int i = 1; i <= 3; i++) {
		ui->LAssert(L, lua_isnumber(L, i), "SetClearColor() argument %d: expected number, got %s", i, luaL_typename(L, i));
		color[i - 1] = (float)lua_tonumber(L, i);
	}
	if (n >= 4 && !lua_isnil(L, 4)) {
		ui->LAssert(L, lua_isnumber(L, 4), "SetClearColor() argument 4: expected number or nil, got %s", luaL_typename(L, 4));
		color[3] = (float)lua_tonumber(L, 4);
	}
	else {
		color[3] = 1.0;
	}
	ui->renderer->SetClearColor(color);
	return 0;
}

static int l_SetDrawLayer(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LAssert(L, ui->renderer != NULL, "Renderer is not initialised");
	ui->LAssert(L, ui->renderEnable, "SetDrawLayer() called outside of OnFrame");
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: SetDrawLayer({layer|nil}[, subLayer])");
	ui->LAssert(L, lua_isnumber(L, 1) || lua_isnil(L, 1), "SetDrawLayer() argument 1: expected number or nil, got %s", luaL_typename(L, 1));
	if (n >= 2) {
		ui->LAssert(L, lua_isnumber(L, 2), "SetDrawLayer() argument 2: expected number, got %s", luaL_typename(L, 2));
	}
	if (lua_isnil(L, 1)) {
		ui->LAssert(L, n >= 2, "SetDrawLayer(): must provide subLayer if layer is nil");
		ui->renderer->SetDrawSubLayer((int)lua_tointeger(L, 2));
	}
	else if (n >= 2) {
		ui->renderer->SetDrawLayer((int)lua_tointeger(L, 1), (int)lua_tointeger(L, 2));
	}
	else {
		ui->renderer->SetDrawLayer((int)lua_tointeger(L, 1));
	}
	return 0;
}

static int l_GetDrawLayer(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	lua_pushinteger(L, ui->renderer->GetDrawLayer());
	return 1;
}

static int l_SetViewport(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LAssert(L, ui->renderer != NULL, "Renderer is not initialised");
	ui->LAssert(L, ui->renderEnable, "SetViewport() called outside of OnFrame");
	int n = lua_gettop(L);
	const float dpiScale = ui->renderer->VirtualScreenScaleFactor();
	if (n) {
		ui->LAssert(L, n >= 4, "Usage: SetViewport([x, y, width, height])");
		for (int i = 1; i <= 4; i++) {
			ui->LAssert(L, lua_isnumber(L, i), "SetViewport() argument %d: expected number, got %s", i, luaL_typename(L, i));
		}
		int vpX = static_cast<int>(std::lround(lua_tonumber(L, 1) * dpiScale));
		int vpY = static_cast<int>(std::lround(lua_tonumber(L, 2) * dpiScale));
		int vpWidth = static_cast<int>(std::ceil(lua_tonumber(L, 3) * dpiScale));
		int vpHeight = static_cast<int>(std::ceil(lua_tonumber(L, 4) * dpiScale));
		ui->renderer->SetViewport(vpX, vpY, vpWidth, vpHeight);
	}
	else {
		ui->renderer->SetViewport();
	}
	return 0;
}

static int l_SetBlendMode(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LAssert(L, ui->renderer != NULL, "Renderer is not initialised");
	ui->LAssert(L, ui->renderEnable, "SetViewport() called outside of OnFrame");
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: SetBlendMode(mode)");
	static const char* modeMap[6] = { "ALPHA", "PREALPHA", "ADDITIVE", NULL };
	ui->renderer->SetBlendMode(luaL_checkoption(L, 1, "ALPHA", modeMap));
	return 0;
}

static int l_SetDrawColor(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LAssert(L, ui->renderer != NULL, "Renderer is not initialised");
	ui->LAssert(L, ui->renderEnable, "SetDrawColor() called outside of OnFrame");
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: SetDrawColor(red, green, blue[, alpha]) or SetDrawColor(escapeStr)");
	col4_t color;
	if (lua_type(L, 1) == LUA_TSTRING) {
		ui->LAssert(L, IsColorEscape(lua_tostring(L, 1)), "SetDrawColor() argument 1: invalid color escape sequence");
		ReadColorEscape(lua_tostring(L, 1), color);
		color[3] = 1.0;
	}
	else {
		ui->LAssert(L, n >= 3, "Usage: SetDrawColor(red, green, blue[, alpha]) or SetDrawColor(escapeStr)");
		for (int i = 1; i <= 3; i++) {
			ui->LAssert(L, lua_isnumber(L, i), "SetDrawColor() argument %d: expected number, got %s", i, luaL_typename(L, i));
			color[i - 1] = (float)lua_tonumber(L, i);
		}
		if (n >= 4 && !lua_isnil(L, 4)) {
			ui->LAssert(L, lua_isnumber(L, 4), "SetDrawColor() argument 4: expected number or nil, got %s", luaL_typename(L, 4));
			color[3] = (float)lua_tonumber(L, 4);
		}
		else {
			color[3] = 1.0;
		}
	}
	ui->renderer->DrawColor(color);
		
	// Store last applied color from renderer
	col4_t finalColor;
	ui->renderer->GetDrawColor(finalColor);
	ui->lastColor[0] = finalColor[0];
	ui->lastColor[1] = finalColor[1];
	ui->lastColor[2] = finalColor[2];
	ui->lastColor[3] = finalColor[3];
		
	return 0;
}

static int l_GetDrawColor(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LAssert(L, ui->renderer != NULL, "Renderer is not initialised");

	lua_pushnumber(L, ui->lastColor[0]);
	lua_pushnumber(L, ui->lastColor[1]);
	lua_pushnumber(L, ui->lastColor[2]);
	lua_pushnumber(L, ui->lastColor[3]);

	return 4; // returning r,g,b,a
}

static int l_DrawImage(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LAssert(L, ui->renderer != NULL, "Renderer is not initialised");
	ui->LAssert(L, ui->renderEnable, "DrawImage() called outside of OnFrame");
	int n = lua_gettop(L);
	const char* usage = "Usage: DrawImage({imgHandle|nil}, left, top, width, height[, tcLeft, tcTop, tcRight, tcBottom][, stackIdx[, mask]])";
	ui->LAssert(L, n >= 5, usage);
	ui->LAssert(L, lua_isnil(L, 1) || ui->IsUserData(L, 1, "uiimghandlemeta"), "DrawImage() argument 1: expected image handle or nil, got %s", luaL_typename(L, 1));
	r_shaderHnd_c* hnd = NULL;
	glm::vec2 xys[2]{}, uvs[2]{};
	int stackLayer = 0;
	std::optional<int> maskLayer{};

	// | n  |img| corners | uvs | stack | mask |
	// | 5  | X | X       |	    |       |      |
	// | 6  | X | X       |     | X     |      |
	// | 7  | X | X       |     | X     | X    |
	// | 9  | X | X       | X   |       |      |
	// | 10 | X | X       | X   | X     |      |
	// | 11 | X | X       | X   | X     | X    |
	
	enum ArgFlag : uint8_t { AF_IMG = 0x1, AF_XY = 0x2, AF_UV = 0x4, AF_STACK = 0x8, AF_MASK = 0x10 };
	ArgFlag af{};
	switch (n) {
	case 11: af = (ArgFlag)(af | AF_MASK);
	case 10: af = (ArgFlag)(af | AF_STACK);
	case 9: af = (ArgFlag)(af | AF_IMG | AF_XY | AF_UV); break;
	case 7: af = (ArgFlag)(af | AF_MASK);
	case 6: af = (ArgFlag)(af | AF_STACK);
	case 5: af = (ArgFlag)(af | AF_IMG | AF_XY); break;
	default: ui->LAssert(L, false, usage);
	}

	int k = 1;
	if (af & AF_IMG) {
		if (!lua_isnil(L, k)) {
			imgHandle_s* imgHandle = (imgHandle_s*)lua_touserdata(L, k);
			ui->LAssert(L, imgHandle->hnd != NULL, "DrawImage(): image handle has no image loaded");
			hnd = imgHandle->hnd;
		}
		k += 1;
	}
	
	if (af & AF_XY) {
		const float dpiScale = ui->renderer->VirtualScreenScaleFactor();
		for (int i = k; i < k + 4; i++) {
			ui->LAssert(L, lua_isnumber(L, i), "DrawImage() argument %d: expected number, got %s", i, luaL_typename(L, i));
			const int idx = i - k;
			xys[idx/2][idx%2] = (float)lua_tonumber(L, i) * dpiScale;
		}
		k += 4;
	}

	if (af & AF_UV) {
		for (int i = k; i < k + 4; i++) {
			ui->LAssert(L, lua_isnumber(L, i), "DrawImage() argument %d: expected number, got %s", i, luaL_typename(L, i));
			int idx = i - k;
			uvs[idx/2][idx%2] = (float)lua_tonumber(L, i);
		}
		k += 4;
	}
	else {
		uvs[0] = { 0, 0 };
		uvs[1] = { 1, 1 };
	}

	std::optional<int> maxStackValue;
	if (hnd)
		maxStackValue = hnd->StackCount();

	if (af & AF_STACK) {
		ui->LAssert(L, lua_isinteger(L, k), "DrawImage() argument %d: expected integer, got %s", k, luaL_typename(L, k));
		const int val = (int)lua_tointeger(L, k);
		ui->LAssert(L, val > 0, "DrawImage() argument %d: expected positive integer, got %d", k, val);
		if (maxStackValue.has_value())
			ui->LAssert(L, val <= *maxStackValue, "DrawImage() argument %d: expected valid stack index <= %d, got %d", k, *maxStackValue, val);
		stackLayer = val - 1;
		k += 1;
	}

	if (af & AF_MASK) {
		ui->LAssert(L, lua_isnil(L, k) || lua_isinteger(L, k), "DrawImage() argument %d: expected integer or nil, got %s", k, luaL_typename(L, k));
		if (lua_isinteger(L, k)) {
			const int val = (int)lua_tointeger(L, k);
			ui->LAssert(L, val > 0, "DrawImage() argument %d: expected positive integer, got %d", k, val);
			if (maxStackValue.has_value())
				ui->LAssert(L, val <= *maxStackValue, "DrawImage() argument %d: expected valid stack index <= %d, got %d", k, *maxStackValue, val);
			maskLayer = val - 1;
		}
		k += 1;
	}

	ui->renderer->DrawImage(hnd, xys[0], xys[1], uvs[0], uvs[1], stackLayer, maskLayer);

	return 0;
}

static int l_DrawImageQuad(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LAssert(L, ui->renderer != NULL, "Renderer is not initialised");
	ui->LAssert(L, ui->renderEnable, "DrawImageQuad() called outside of OnFrame");
	int n = lua_gettop(L);
	const char* usage = "Usage: DrawImageQuad({imgHandle|nil}, x1, y1, x2, y2, x3, y3, x4, y4[, s1, t1, s2, t2, s3, t3, s4, t4][, stackIdx[, mask]])";
	ui->LAssert(L, n >= 9, usage);
	ui->LAssert(L, lua_isnil(L, 1) || ui->IsUserData(L, 1, "uiimghandlemeta"), "DrawImageQuad() argument 1: expected image handle or nil, got %s", luaL_typename(L, 1));

	r_shaderHnd_c* hnd = NULL;
	glm::vec2 xys[4]{}, uvs[4]{};
	int stackLayer = 0;
	std::optional<int> maskLayer{};
	
	// | n  |img| corners | uvs | stack | mask |
	// | 9  | X | X       |	    |       |      |
	// | 10 | X | X       |     | X     |      |
	// | 11 | X | X       |     | X     | X    |
	// | 17 | X | X       | X   |       |      |
	// | 18 | X | X       | X   | X     |      |
	// | 19 | X | X       | X   | X     | X    |

	enum ArgFlag : uint8_t { AF_IMG = 0x1, AF_XY = 0x2, AF_UV = 0x4, AF_STACK = 0x8, AF_MASK = 0x10 };
	ArgFlag af{};
	switch (n) {
	case 19: af = (ArgFlag)(af | AF_MASK);
	case 18: af = (ArgFlag)(af | AF_STACK);
	case 17: af = (ArgFlag)(af | AF_IMG | AF_XY | AF_UV); break;
	case 11: af = (ArgFlag)(af | AF_MASK);
	case 10: af = (ArgFlag)(af | AF_STACK);
	case 9: af = (ArgFlag)(af | AF_IMG | AF_XY); break;
	default: ui->LAssert(L, false, usage);
	}

	int k = 1;
	if (af & AF_IMG) {
		if (!lua_isnil(L, k)) {
			imgHandle_s* imgHandle = (imgHandle_s*)lua_touserdata(L, k);
			ui->LAssert(L, imgHandle->hnd != NULL, "DrawImageQuad(): image handle has no image loaded");
			hnd = imgHandle->hnd;
		}
		k += 1;
	}

	if (af & AF_XY) {
		const float dpiScale = ui->renderer->VirtualScreenScaleFactor();
		for (int i = k; i < k + 8; i++) {
			ui->LAssert(L, lua_isnumber(L, i), "DrawImageQuad() argument %d: expected number, got %s", i, luaL_typename(L, i));
			const int idx = i - k;
			xys[idx / 2][idx % 2] = (float)lua_tonumber(L, i) * dpiScale;
		}
		k += 8;
	}

	if (af & AF_UV) {
		for (int i = k; i < k + 8; i++) {
			ui->LAssert(L, lua_isnumber(L, i), "DrawImageQuad() argument %d: expected number, got %s", i, luaL_typename(L, i));
			int idx = i - k;
			uvs[idx / 2][idx % 2] = (float)lua_tonumber(L, i);
		}
		k += 8;
	}
	else {
		uvs[0] = { 0, 0 };
		uvs[1] = { 1, 0 };
		uvs[2] = { 1, 1 };
		uvs[3] = { 0, 1 };
	}

	std::optional<int> maxStackValue;
	if (hnd)
		maxStackValue = hnd->StackCount();

	if (af & AF_STACK) {
		ui->LAssert(L, lua_isinteger(L, k), "DrawImageQuad() argument %d: expected integer, got %s", k, luaL_typename(L, k));
		const int val = (int)lua_tointeger(L, k);
		ui->LAssert(L, val > 0, "DrawImageQuad() argument %d: expected positive integer, got %d", k, val);
		if (maxStackValue.has_value())
			ui->LAssert(L, val <= *maxStackValue, "DrawImageQuad() argument %d: expected valid stack index <= %d, got %d", k, *maxStackValue, val);
		stackLayer = val - 1;
		k += 1;
	}

	if (af & AF_MASK) {
		ui->LAssert(L, lua_isnil(L, k) || lua_isinteger(L, k), "DrawImageQuad() argument %d: expected integer or nil, got %s", k, luaL_typename(L, k));
		if (lua_isinteger(L, k)) {
			const int val = (int)lua_tointeger(L, k);
			ui->LAssert(L, val > 0, "DrawImageQuad() argument %d: expected positive integer, got %d", k, val);
			if (maxStackValue.has_value())
				ui->LAssert(L, val <= *maxStackValue, "DrawImageQuad() argument %d: expected valid stack index <= %d, got %d", k, *maxStackValue, val);
			maskLayer = val - 1;
		}
		k += 1;
	}


	ui->renderer->DrawImageQuad(hnd, xys[0], xys[1], xys[2], xys[3], uvs[0], uvs[1], uvs[2], uvs[3], stackLayer, maskLayer);

	return 0;
}

// ==============================
// PoeCharm translation injection
// ==============================
//
// Display translation (English -> Chinese) is applied to the text argument of
// DrawString/DrawStringWidth/DrawStringCursorIndex before it reaches the renderer.
// Logic ported from pob-translate-proxy/src/hooks/draw_string_hook.cpp, but returns
// a std::string instead of rewriting the Lua stack (the engine owns these functions).

// Strip POB inline color codes (^[0-9] preset, ^xRRGGBB hex). The first leading
// color prefix is captured in `leading` so it can be re-attached to the translation.
static int tr_strip_color_codes(const char* src, int src_len,
	char* dst, int dst_cap, char* leading, int leading_cap)
{
	int di = 0, li = 0;
	bool got_leading = false;
	for (int i = 0; i < src_len && di < dst_cap - 1; ) {
		if (src[i] == '^' && i + 1 < src_len) {
			char next = src[i + 1];
			if (next >= '0' && next <= '9') {
				if (!got_leading && di == 0 && li + 2 < leading_cap) {
					leading[li++] = src[i];
					leading[li++] = src[i + 1];
					got_leading = true;
				}
				i += 2;
				continue;
			}
			if (next == 'x' && i + 7 < src_len) {
				if (!got_leading && di == 0 && li + 8 < leading_cap) {
					memcpy(leading + li, src + i, 8);
					li += 8;
					got_leading = true;
				}
				i += 8;
				continue;
			}
		}
		dst[di++] = src[i++];
	}
	dst[di] = '\0';
	leading[li] = '\0';
	return di;
}

// Translate a display string for rendering. Returns the translated string when a
// translation exists, otherwise the original text unchanged.
static std::string tr_display(const char* text)
{
	if (!text) return std::string();
	if (!translation_is_enabled()) return std::string(text);
	size_t len = strlen(text);
	if (len == 0) return std::string();

	// 1. Direct lookup on the raw text (handles exact matches incl. color codes)
	const char* raw = translation_lookup(text);
	bool has_colors = false;
	for (size_t i = 0; i + 1 < len; i++) {
		if (text[i] == '^' && (text[i + 1] == 'x' || (text[i + 1] >= '0' && text[i + 1] <= '9'))) {
			has_colors = true;
			break;
		}
	}
	if (raw && raw != text && !has_colors) {
		return std::string(raw);
	}

	// 2. Strip color codes and retry (cleaner pattern match on the bare text)
	char stripped[4096];
	char leading[16];
	int slen = tr_strip_color_codes(text, (int)len, stripped, sizeof(stripped), leading, sizeof(leading));
	if (slen == (int)len) {
		return std::string(raw && raw != text ? raw : text);
	}
	const char* sres = translation_lookup(stripped);
	if (sres) {
		if (leading[0] != '\0') return std::string(leading) + sres;
		return std::string(sres);
	}
	return std::string(raw && raw != text ? raw : text);
}

static int l_DrawString(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LAssert(L, ui->renderer != NULL, "Renderer is not initialised");
	ui->LAssert(L, ui->renderEnable, "DrawString() called outside of OnFrame");
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 6, "Usage: DrawString(left, top, align, height, font, text)");
	ui->LAssert(L, lua_isnumber(L, 1), "DrawString() argument 1: expected number, got %s", luaL_typename(L, 1));
	ui->LAssert(L, lua_isnumber(L, 2), "DrawString() argument 2: expected number, got %s", luaL_typename(L, 2));
	ui->LAssert(L, lua_isstring(L, 3) || lua_isnil(L, 3), "DrawString() argument 3: expected string or nil, got %s", luaL_typename(L, 3));
	ui->LAssert(L, lua_isnumber(L, 4), "DrawString() argument 4: expected number, got %s", luaL_typename(L, 4));
	ui->LAssert(L, lua_isstring(L, 5), "DrawString() argument 5: expected string, got %s", luaL_typename(L, 5));
	ui->LAssert(L, lua_isstring(L, 6), "DrawString() argument 6: expected string, got %s", luaL_typename(L, 6));
	static const char* alignMap[6] = { "LEFT", "CENTER", "RIGHT", "CENTER_X", "RIGHT_X", NULL };
	static const char* fontMap[8] = { "FIXED", "VAR", "VAR BOLD", "FONTIN SC", "FONTIN SC ITALIC", "FONTIN", "FONTIN ITALIC", NULL };
	const float dpiScale = ui->renderer->VirtualScreenScaleFactor();
	const float left = lua_tonumber(L, 1) * dpiScale;
	const float top = lua_tonumber(L, 2) * dpiScale;
	const lua_Number logicalHeight = lua_tonumber(L, 4);
	int scaledHeight = (int)std::lround(logicalHeight * dpiScale);
	if (scaledHeight <= 1) {
		scaledHeight = 1;
	}
	else {
		scaledHeight = (scaledHeight + 1) & ~1;
	}
	std::string s_text = tr_display(lua_tostring(L, 6));
	ui->renderer->DrawString(
		left,
		top,
		luaL_checkoption(L, 3, "LEFT", alignMap),
		scaledHeight,
		NULL,
		luaL_checkoption(L, 5, "FIXED", fontMap),
		s_text.c_str()
	);

	// Get the final color from the renderer after DrawString processes color codes
	col4_t finalColor;
	ui->renderer->GetDrawColor(finalColor);
	ui->lastColor[0] = finalColor[0];
	ui->lastColor[1] = finalColor[1];
	ui->lastColor[2] = finalColor[2];
	ui->lastColor[3] = finalColor[3];

	return 0;
}

static int l_DrawStringWidth(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LAssert(L, ui->renderer != NULL, "Renderer is not initialised");
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 3, "Usage: DrawStringWidth(height, font, text)");
	ui->LAssert(L, lua_isnumber(L, 1), "DrawStringWidth() argument 1: expected number, got %s", luaL_typename(L, 1));
	ui->LAssert(L, lua_isstring(L, 2), "DrawStringWidth() argument 2: expected string, got %s", luaL_typename(L, 2));
	ui->LAssert(L, lua_isstring(L, 3), "DrawStringWidth() argument 3: expected string, got %s", luaL_typename(L, 3));
	static const char* fontMap[8] = { "FIXED", "VAR", "VAR BOLD", "FONTIN SC", "FONTIN SC ITALIC", "FONTIN", "FONTIN ITALIC", NULL };
	const float dpiScale = ui->renderer->VirtualScreenScaleFactor();
	const lua_Number logicalHeight = lua_tonumber(L, 1);
	int scaledHeight = static_cast<int>(std::lround(logicalHeight * dpiScale));
	if (scaledHeight <= 1) {
		scaledHeight = 1;
	}
	else {
		scaledHeight = (scaledHeight + 1) & ~1;
	}
	std::string s_text = tr_display(lua_tostring(L, 3));
	double const physicalWidth = ui->renderer->DrawStringWidth(
		scaledHeight,
		luaL_checkoption(L, 2, "FIXED", fontMap),
		s_text.c_str());
	lua_pushnumber(L, physicalWidth / dpiScale);
	return 1;
}

static int l_DrawStringCursorIndex(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LAssert(L, ui->renderer != NULL, "Renderer is not initialised");
	int n = lua_gettop(L);
	const float dpiScale = ui->renderer->VirtualScreenScaleFactor();
	ui->LAssert(L, n >= 5, "Usage: DrawStringCursorIndex(height, font, text, cursorX, cursorY)");
	ui->LAssert(L, lua_isnumber(L, 1), "DrawStringCursorIndex() argument 1: expected number, got %s", luaL_typename(L, 1));
	ui->LAssert(L, lua_isstring(L, 2), "DrawStringCursorIndex() argument 2: expected string, got %s", luaL_typename(L, 2));
	ui->LAssert(L, lua_isstring(L, 3), "DrawStringCursorIndex() argument 3: expected string, got %s", luaL_typename(L, 3));
	ui->LAssert(L, lua_isnumber(L, 4), "DrawStringCursorIndex() argument 4: expected number, got %s", luaL_typename(L, 4));
	ui->LAssert(L, lua_isnumber(L, 5), "DrawStringCursorIndex() argument 5: expected number, got %s", luaL_typename(L, 5));
	static const char* fontMap[8] = { "FIXED", "VAR", "VAR BOLD", "FONTIN SC", "FONTIN SC ITALIC", "FONTIN", "FONTIN ITALIC", NULL };
	const lua_Number logicalHeight = lua_tonumber(L, 1);
	const lua_Number logicalCursorX = lua_tonumber(L, 4);
	const lua_Number logicalCursorY = lua_tonumber(L, 5);
	int scaledHeight = static_cast<int>(std::lround(logicalHeight * dpiScale));
	if (scaledHeight <= 1) {
		scaledHeight = 1;
	}
	else {
		scaledHeight = (scaledHeight + 1) & ~1;
	}
	const int scaledCursorX = static_cast<int>(std::lround(logicalCursorX * dpiScale));
	const int scaledCursorY = static_cast<int>(std::lround(logicalCursorY * dpiScale));
	std::string s_text = tr_display(lua_tostring(L, 3));
	lua_pushinteger(L, static_cast<lua_Integer>(ui->renderer->DrawStringCursorIndex(
		scaledHeight,
		luaL_checkoption(L, 2, "FIXED", fontMap),
		s_text.c_str(),
		scaledCursorX, scaledCursorY) + 1));
	return 1;
}

static int l_SetDPIScaleOverridePercent(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LAssert(L, ui->renderer != NULL, "Renderer is not initialised");
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: SetDPIScaleOverridePercent(percent)");
	ui->LAssert(L, lua_isnumber(L, 1), "SetDPIScaleOverridePercent() argument 1: expected number, got %s", luaL_typename(L, 1));
	int percent = (int)lua_tointeger(L, 1);
	ui->renderer->SetDpiScaleOverridePercent(percent);
	return 0;
}

static int l_GetDPIScaleOverridePercent(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LAssert(L, ui->renderer != NULL, "Renderer is not initialised");
	lua_pushinteger(L, ui->renderer->DpiScaleOverridePercent());
	return 1;
}

static int l_StripEscapes(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: StripEscapes(string)");
	ui->LAssert(L, lua_isstring(L, 1), "StripEscapes() argument 1: expected string, got %s", luaL_typename(L, 1));
	const char* str = lua_tostring(L, 1);
	char* strip = new char[strlen(str) + 1];
	char* p = strip;
	while (*str) {
		int esclen = IsColorEscape(str);
		if (esclen) {
			str += esclen;
		}
		else {
			*(p++) = *(str++);
		}
	}
	*p = 0;
	lua_pushstring(L, strip);
	delete[] strip;
	return 1;
}

static int l_GetAsyncCount(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->LAssert(L, ui->renderer != NULL, "Renderer is not initialised");
	lua_pushinteger(L, ui->renderer->GetTexAsyncCount());
	return 1;
}

// ============
// File Handles
// ============

#ifdef _WIN32
#include <Windows.h>

static void stackDump(lua_State* L)
{
	char buf[4096]{};
	char* p = buf;
	int i;
	int top = lua_gettop(L);
	for (i = 1; i <= top; i++) {  /* repeat for each level */
		int t = lua_type(L, i);
		switch (t) {

		case LUA_TSTRING:  /* strings */
			p += sprintf(p, "`%s'", lua_tostring(L, i));
			break;

		case LUA_TBOOLEAN:  /* booleans */
			p += sprintf(p, lua_toboolean(L, i) ? "true" : "false");
			break;

		case LUA_TNUMBER:  /* numbers */
			p += sprintf(p, "%g", lua_tonumber(L, i));
			break;

		default:  /* other values */
			p += sprintf(p, "%s", lua_typename(L, t));
			break;

		}
		p += sprintf(p, "  ");  /* put a separator */
	}
	p += sprintf(p, "\n");  /* end the listing */
	OutputDebugStringA(buf);
}
#endif

// ==============
// Search Handles
// ==============

struct searchHandle_s {
	find_c* find;
	bool	dirOnly;
};

SG_LUA_CPP_FUN_BEGIN(NewFileSearch)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LExpect(L, n >= 1, "Usage: NewFileSearch(spec[, findDirectories])");
	ui->LExpect(L, lua_isstring(L, 1), "NewFileSearch() argument 1: expected string, got %s", luaL_typename(L, 1));
	find_c* find = new find_c();
	auto path = std::filesystem::u8path(lua_tostring(L, 1));
	if (!find->FindFirst(std::move(path))) {
		delete find;
		return 0;
	}
	bool dirOnly = lua_toboolean(L, 2) != 0;
	while (find->isDirectory != dirOnly || find->fileName.filename() == "." || find->fileName.filename() == "..") {
		if (!find->FindNext()) {
			delete find;
			return 0;
		}
	}
	searchHandle_s* searchHandle = (searchHandle_s*)lua_newuserdata(L, sizeof(searchHandle_s));
	searchHandle->find = find;
	searchHandle->dirOnly = dirOnly;
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_setmetatable(L, -2);
	return 1;
}
SG_LUA_CPP_FUN_END()

static searchHandle_s* GetSearchHandle(lua_State* L, ui_main_c* ui, const char* method, bool valid)
{
	ui->LAssert(L, ui->IsUserData(L, 1, "uisearchhandlemeta"), "searchHandle:%s() must be used on a search handle", method);
	searchHandle_s* searchHandle = (searchHandle_s*)lua_touserdata(L, 1);
	lua_remove(L, 1);
	if (valid) {
		ui->LAssert(L, searchHandle->find != NULL, "searchHandle:%s(): search handle is no longer valid (ran out of files to find)", method);
	}
	return searchHandle;
}

static int l_searchHandleGC(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	searchHandle_s* searchHandle = GetSearchHandle(L, ui, "__gc", false);
	delete searchHandle->find;
	return 0;
}

static int l_searchHandleNextFile(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	searchHandle_s* searchHandle = GetSearchHandle(L, ui, "NextFile", true);
	auto &find = searchHandle->find;
	do {
		if (!find->FindNext()) {
			delete find;
			find = NULL;
			return 0;
		}
	} while (find->isDirectory != searchHandle->dirOnly || find->fileName.filename() == "." || find->fileName.filename() == "..");
	lua_pushboolean(L, 1);
	return 1;
}

static int l_searchHandleGetFileName(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	searchHandle_s* searchHandle = GetSearchHandle(L, ui, "GetFileName", true);
	lua_pushstring(L, searchHandle->find->fileName.generic_u8string().c_str());
	return 1;
}

static int l_searchHandleGetFileSize(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	searchHandle_s* searchHandle = GetSearchHandle(L, ui, "GetFileSize", true);
	lua_pushinteger(L, (lua_Integer)searchHandle->find->fileSize);
	return 1;
}

static int l_searchHandleGetFileModifiedTime(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	searchHandle_s* searchHandle = GetSearchHandle(L, ui, "GetFileModifiedTime", true);
	lua_pushnumber(L, (double)searchHandle->find->modified);
	return 1;
}

// ===================
// Cloud provider info
// ===================

struct CloudProviderInfo {
	std::string name;
	std::string version;
	uint32_t status;
};

#ifdef _WIN32
#include <Windows.h>
#include <cfapi.h>

static std::string NarrowString(std::wstring_view ws) {
	auto cb = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
	std::string ret(cb, '\0');
	WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), ret.data(), (int)ret.size(), nullptr, nullptr);
	return ret;
}

struct CloudProviderLibrary {
	CloudProviderLibrary() {
		cldLib = LoadLibraryW(L"cldapi.dll");
		if (cldLib != nullptr) {
			CfGetSyncRootInfoByPath = (decltype (CfGetSyncRootInfoByPath))GetProcAddress(cldLib, "CfGetSyncRootInfoByPath");
		}
	}

	~CloudProviderLibrary() {
		FreeLibrary(cldLib);
	}

	bool Loaded() const { return cldLib != nullptr && CfGetSyncRootInfoByPath != nullptr; }

	CloudProviderLibrary(CloudProviderLibrary const&) = delete;
	CloudProviderLibrary& operator = (CloudProviderLibrary const&) = delete;

	decltype (&::CfGetSyncRootInfoByPath) CfGetSyncRootInfoByPath{};

	HMODULE cldLib{};
};

static std::optional<CloudProviderInfo> GetCloudProviderInfo(std::filesystem::path const& path) {
	HRESULT hr{ S_OK };
	DWORD len{};
	static std::vector<char> buf(65536);
	static CloudProviderLibrary lib;
	if (!lib.Loaded()) {
		return {};
	}
	hr = lib.CfGetSyncRootInfoByPath(path.generic_wstring().c_str(), CF_SYNC_ROOT_INFO_PROVIDER, buf.data(), (DWORD)buf.size(), &len);
	if (FAILED(hr) && GetLastError() != ERROR_MORE_DATA) {
		return {};
	}
	auto* syncRootInfo = (CF_SYNC_ROOT_PROVIDER_INFO const*)buf.data();
	buf.resize(len);
	hr = lib.CfGetSyncRootInfoByPath(path.c_str(), CF_SYNC_ROOT_INFO_PROVIDER, buf.data(), len, &len);
	if (FAILED(hr)) {
		return {};
	}
	CloudProviderInfo ret{};
	ret.name = NarrowString(syncRootInfo->ProviderName);
	ret.version = NarrowString(syncRootInfo->ProviderVersion);
	ret.status = syncRootInfo->ProviderStatus;
	return ret;
}
#else
static std::optional<CloudProviderInfo> GetCloudProviderInfo(std::filesystem::path const& path) {
	return {};
}
#endif

static int l_GetCloudProvider(lua_State* L) {
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: GetCloudProvider(path)");
	ui->LAssert(L, lua_isstring(L, 1), "GetCloudProvider() argument 1: expected string, got %s", luaL_typename(L, 1));

	auto path = std::filesystem::u8path(lua_tostring(L, 1));
	auto info = GetCloudProviderInfo(path);
	if (info) {
		lua_pushstring(L, info->name.c_str());
		lua_pushstring(L, info->version.c_str());
		lua_pushinteger(L, info->status);
		return 3;
	}

	return 0;
}

// =================
// General Functions
// =================

static int l_SetWindowTitle(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: SetWindowTitle(title)");
	ui->LAssert(L, lua_isstring(L, 1), "SetWindowTitle() argument 1: expected string, got %s", luaL_typename(L, 1));
	ui->sys->video->SetTitle(lua_tostring(L, 1));
	ui->sys->conWin->SetTitle(lua_tostring(L, 1));
	return 0;
}

static int l_GetCursorPos(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	const float dpiScale = ui->renderer->VirtualScreenScaleFactor();
	lua_pushinteger(L, (lua_Integer)std::lround(ui->renderer->VirtualMap(ui->cursorX) / dpiScale));
	lua_pushinteger(L, (lua_Integer)std::lround(ui->renderer->VirtualMap(ui->cursorY) / dpiScale));
	return 2;
}

static int l_SetCursorPos(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	const float dpiScale = ui->renderer->VirtualScreenScaleFactor();
	ui->LAssert(L, n >= 2, "Usage: SetCursorPos(x, y)");
	ui->LAssert(L, lua_isnumber(L, 1), "SetCursorPos() argument 1: expected number, got %s", luaL_typename(L, 1));
	ui->LAssert(L, lua_isnumber(L, 2), "SetCursorPos() argument 2: expected number, got %s", luaL_typename(L, 2));
	const int scaledX = (int)std::lround(lua_tonumber(L, 1) * dpiScale);
	const int scaledY = (int)std::lround(lua_tonumber(L, 2) * dpiScale);
	int x = ui->renderer->VirtualUnmap(scaledX);
	int y = ui->renderer->VirtualUnmap(scaledY);
	ui->sys->video->SetRelativeCursor(x, y);
	return 0;
}

static int l_ShowCursor(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: ShowCursor(doShow)");
	return 0;
}

static int l_IsKeyDown(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: IsKeyDown(keyName)");
	ui->LAssert(L, lua_isstring(L, 1), "IsKeyDown() argument 1: expected string, got %s", luaL_typename(L, 1));
	size_t len;
	const char* kname = lua_tolstring(L, 1, &len);
	ui->LAssert(L, len >= 1, "IsKeyDown() argument 1: string is empty", 1);
	int key = ui->KeyForName(kname);
	ui->LAssert(L, key, "IsKeyDown(): unrecognised key name");
	lua_pushboolean(L, ui->sys->IsKeyDown(key));
	return 1;
}

static int l_Copy(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: Copy(string)");
	ui->LAssert(L, lua_isstring(L, 1), "Copy() argument 1: expected string, got %s", luaL_typename(L, 1));
	ui->sys->ClipboardCopy(lua_tostring(L, 1));
	return 0;
}

static int l_Paste(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);

	// Read the clipboard directly via Win32 (CF_UNICODETEXT -> UTF-8). GLFW's
	// glfwGetClipboardString (used by sys->ClipboardPaste) corrupts CJK to '?',
	// which would make reverse-translation of pasted Chinese items impossible.
	std::string utf8_text;
	bool got_clipboard = false;
	if (OpenClipboard(NULL)) {
		HANDLE hData = GetClipboardData(CF_UNICODETEXT);
		if (hData) {
			wchar_t* wstr = (wchar_t*)GlobalLock(hData);
			if (wstr) {
				int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
				if (needed > 0) {
					utf8_text.resize(needed - 1);
					WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &utf8_text[0], needed, NULL, NULL);
					got_clipboard = true;
				}
				GlobalUnlock(hData);
			}
		}
		CloseClipboard();
	}

	if (got_clipboard && !utf8_text.empty()) {
		// Reverse-translate Chinese -> English so POB can parse pasted items.
		char* reversed = translation_reverse_text(utf8_text.c_str());
		if (reversed) {
			lua_pushstring(L, reversed);
			translation_free(reversed);
		} else {
			lua_pushstring(L, utf8_text.c_str());
		}
		return 1;
	}

	// Fallback: original GLFW-based clipboard read.
	char* data = ui->sys->ClipboardPaste();
	if (data) {
		lua_pushstring(L, data);
		FreeString(data);
		return 1;
	}
	return 0;
}

static int l_Deflate(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: Deflate(string)");
	ui->LAssert(L, lua_isstring(L, 1), "Deflate() argument 1: expected string, got %s", luaL_typename(L, 1));
	z_stream_s z;
	z.zalloc = NULL;
	z.zfree = NULL;
	deflateInit(&z, 9);
	size_t inLen;
	byte* in = (byte*)lua_tolstring(L, 1, &inLen);
	// Prevent deflation of input data larger than 128 MiB.
	size_t const maxInLen = 128ull << 20;
	if (inLen > maxInLen) {
		lua_pushnil(L);
		lua_pushstring(L, "Input larger than 128 MiB");
		return 2;
	}
	uLong outSz = deflateBound(&z, (uLong)inLen);
	// Clamp deflate bound to a fairly reasonable 128 MiB.
	size_t const maxOutLen = 128ull << 20;
	outSz = std::min<uLong>(outSz, maxOutLen);
	std::vector<byte> out(outSz);
	z.next_in = in;
	z.avail_in = (uInt)inLen;
	z.next_out = out.data();
	z.avail_out = outSz;
	int err = deflate(&z, Z_FINISH);
	deflateEnd(&z);
	if (err == Z_STREAM_END) {
		lua_pushlstring(L, (const char*)out.data(), z.total_out);
		return 1;
	}
	else {
		lua_pushnil(L);
		lua_pushstring(L, zError(err));
		return 2;
	}
}

static int l_Inflate(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: Inflate(string)");
	ui->LAssert(L, lua_isstring(L, 1), "Inflate() argument 1: expected string, got %s", luaL_typename(L, 1));
	size_t inLen;
	byte* in = (byte*)lua_tolstring(L, 1, &inLen);
	size_t const maxInLen = 128ull << 20;
	if (inLen > maxInLen) {
		lua_pushnil(L);
		lua_pushstring(L, "Input larger than 128 MiB");
	}
	uInt outSz = (uInt)(inLen * 4);
	std::vector<byte> out(outSz);
	z_stream_s z;
	z.next_in = in;
	z.avail_in = (uInt)inLen;
	z.zalloc = NULL;
	z.zfree = NULL;
	z.next_out = out.data();
	z.avail_out = outSz;
	inflateInit(&z);
	int err;
	while ((err = inflate(&z, Z_NO_FLUSH)) == Z_OK) {
		// Output buffer filled, try to embiggen it.
		if (z.avail_out == 0) {
			// Avoid growing inflate output size after 128 MiB.
			size_t const maxOutLen = 128ull << 20;
			if (outSz > maxOutLen) {
				break;
			}
			int newSz = outSz * 2;
			out.resize(newSz);
			z.next_out = out.data() + outSz;
			z.avail_out = outSz;
			outSz = newSz;
		}
	}
	inflateEnd(&z);
	if (err == Z_STREAM_END) {
		lua_pushlstring(L, (const char*)out.data(), z.total_out);
		return 1;
	}
	else {
		lua_pushnil(L);
		lua_pushstring(L, zError(err));
		return 2;
	}
}

static int l_GetTime(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	lua_pushinteger(L, ui->sys->GetTime());
	return 1;
}

static int l_GetScriptPath(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	lua_pushstring(L, ui->scriptPath.generic_u8string().c_str());
	try
	{
		lua_pushstring(L, ui->scriptPath.generic_string().c_str());
		return 2;
	}
	catch (std::exception& e)
	{
		lua_pushnil(L);
		lua_pushstring(L, e.what());
		return 3;
	}
}

static int l_GetRuntimePath(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	// Report the POB script's own folder, not basePath (our engine dir).
	// POB bundles its own runtime there (Update.exe, lua51.dll, ...), and its
	// updater writes "runtime" files to GetRuntimePath(): pointing this at the
	// engine dir would make POB's self-update overwrite the translation engine.
	lua_pushstring(L, ui->scriptPath.generic_u8string().c_str());
	try
	{
		lua_pushstring(L, ui->scriptPath.generic_string().c_str());
		return 2;
	}
	catch (std::exception& e)
	{
		lua_pushnil(L);
		lua_pushstring(L, e.what());
		return 3;
	}
}

static int l_GetUserPath(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	auto& userPath = ui->sys->userPath;
	if (!userPath) {
		lua_pushnil(L);
		lua_pushnil(L);
		if (auto& reason = ui->sys->userPathReason)
		{
			lua_pushstring(L, reason->c_str());
			return 3;
		}
		return 2;
	}

	lua_pushstring(L, userPath->generic_u8string().c_str());
	try
	{
		lua_pushstring(L, userPath->generic_string().c_str());
		return 2;
	}
	catch (std::exception& e)
	{
		lua_pushnil(L);
		lua_pushstring(L, e.what());
		return 3;
	}
}

static int l_MakeDir(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: MakeDir(path)");
	ui->LAssert(L, lua_isstring(L, 1), "MakeDir() argument 1: expected string, got %s", luaL_typename(L, 1));
	char const* givenPath = lua_tostring(L, 1);
	auto path = std::filesystem::u8path(givenPath);
	std::error_code ec;
	if (!create_directory(path, ec)) {
		lua_pushnil(L);
		lua_pushstring(L, strerror(ec.value()));
		return 2;
	}
	else {
		lua_pushboolean(L, true);
		return 1;
	}
}

static int l_RemoveDir(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: l_RemoveDir(path, recurse)");
	ui->LAssert(L, lua_isstring(L, 1), "l_RemoveDir() argument 1: expected string, got %s", luaL_typename(L, 1));
	char const* givenPath = lua_tostring(L, 1);
	auto path = std::filesystem::u8path(givenPath);
	bool recursive = false;
	if (n > 1) {
		ui->LAssert(L, lua_isboolean(L, 2), "l_RemoveDir() argument 2: expected boolean, got %s", luaL_typename(L, 2));
		recursive = lua_toboolean(L, 2);
	}
	std::error_code ec;
	if (!is_directory(path, ec) || ec
		|| (recursive && !remove_all(path, ec)) || ec
		|| (!recursive && !remove(path, ec)) || ec)
	{
		lua_pushnil(L);
		lua_pushstring(L, strerror(ec.value()));
		return 2;
	}
	else {
		lua_pushboolean(L, true);
		return 1;
	}
}

SG_LUA_CPP_FUN_BEGIN(SetWorkDir)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LExpect(L, n >= 1, "Usage: SetWorkDir(path)");
	ui->LExpect(L, lua_isstring(L, 1), "SetWorkDir() argument 1: expected string, got %s", luaL_typename(L, 1));
	auto newWorkDir = std::filesystem::u8path(lua_tostring(L, 1));

	if (!ui->sys->SetWorkDir(newWorkDir)) {
		ui->scriptWorkDir = newWorkDir;
	}
	return 0;
}
SG_LUA_CPP_FUN_END()

static int l_GetWorkDir(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	lua_pushstring(L, ui->scriptWorkDir.generic_u8string().c_str());
	return 1;
}

static int l_LaunchSubScript(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 3, "Usage: LaunchSubScript(scriptText, funcList, subList[, ...])");
	for (int i = 1; i <= 3; i++) {
		ui->LAssert(L, lua_isstring(L, i), "LaunchSubScript() argument %d: expected string, got %s", i, luaL_typename(L, i));
	}
	for (int i = 4; i <= n; i++) {
		ui->LAssert(L, lua_isnil(L, i) || lua_isboolean(L, i) || lua_isnumber(L, i) || lua_isstring(L, i),
			"LaunchSubScript() argument %d: only nil, boolean, number and string types can be passed to sub script", i);
	}
	dword slot = -1;
	for (dword i = 0; i < ui->subScriptSize; i++) {
		if (!ui->subScriptList[i]) {
			slot = i;
			break;
		}
	}
	if (slot == -1) {
		slot = ui->subScriptSize;
		ui->subScriptSize <<= 1;
		trealloc(ui->subScriptList, ui->subScriptSize);
		for (dword i = slot; i < ui->subScriptSize; i++) {
			ui->subScriptList[i] = NULL;
		}
	}
	ui->subScriptList[slot] = ui_ISubScript::GetHandle(ui, slot);
	if (ui->subScriptList[slot]->Start()) {
		lua_pushlightuserdata(L, (void*)(uintptr_t)slot);
	}
	else {
		lua_pushnil(L);
	}
	return 1;
}

static int l_AbortSubScript(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: AbortSubScript(ssID)");
	ui->LAssert(L, lua_islightuserdata(L, 1), "AbortSubScript() argument 1: expected subscript ID, got %s", luaL_typename(L, 1));
	dword slot = (dword)(uintptr_t)lua_touserdata(L, 1);
	ui->LAssert(L, slot < ui->subScriptSize && ui->subScriptList[slot], "AbortSubScript() argument 1: invalid subscript ID");
	ui->LAssert(L, ui->subScriptList[slot]->IsRunning(), "AbortSubScript(): subscript isn't running");
	ui_ISubScript::FreeHandle(ui->subScriptList[slot]);
	ui->subScriptList[slot] = NULL;
	return 0;
}

static int l_IsSubScriptRunning(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: IsSubScriptRunning(ssID)");
	ui->LAssert(L, lua_islightuserdata(L, 1), "IsSubScriptRunning() argument 1: expected subscript ID, got %s", luaL_typename(L, 1));
	dword slot = (dword)(uintptr_t)lua_touserdata(L, 1);
	ui->LAssert(L, slot < ui->subScriptSize && ui->subScriptList[slot], "IsSubScriptRunning() argument 1: invalid subscript ID");
	lua_pushboolean(L, ui->subScriptList[slot]->IsRunning());
	return 1;
}

SG_LUA_CPP_FUN_BEGIN(LoadModule)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LExpect(L, n >= 1, "Usage: LoadModule(name[, ...])");
	ui->LExpect(L, lua_isstring(L, 1), "LoadModule() argument 1: expected string, got %s", luaL_typename(L, 1));
	const char* modName = lua_tostring(L, 1);
	auto fileName = std::filesystem::u8path(modName);
	if (!fileName.has_extension()) {
		fileName.replace_extension(".lua");
	}

	ui->sys->SetWorkDir(ui->scriptPath);
	auto fileStr = fileName.generic_u8string();
	int err = luaL_loadfile(L, fileStr.c_str());
	ui->sys->SetWorkDir(ui->scriptWorkDir);
	ui->LExpect(L, err == 0, "LoadModule() error loading '%s' (%d):\n%s", fileStr.c_str(), err, lua_tostring(L, -1));
	lua_replace(L, 1);	// Replace module name with module main chunk
	lua_call(L, n - 1, LUA_MULTRET);
	return lua_gettop(L);
}
SG_LUA_CPP_FUN_END()

SG_LUA_CPP_FUN_BEGIN(PLoadModule)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LExpect(L, n >= 1, "Usage: PLoadModule(name[, ...])");
	ui->LExpect(L, lua_isstring(L, 1), "PLoadModule() argument 1: expected string, got %s", luaL_typename(L, 1));
	const char* modName = lua_tostring(L, 1);
	auto fileName = std::filesystem::u8path(modName);
	if (!fileName.has_extension()) {
		fileName.replace_extension(".lua");
	}

	ui->sys->SetWorkDir(ui->scriptPath);
	int err = luaL_loadfile(L, fileName.generic_u8string().c_str());
	ui->sys->SetWorkDir(ui->scriptWorkDir);
	if (err) {
		return 1;
	}
	lua_replace(L, 1);	// Replace module name with module main chunk
	lua_getfield(L, LUA_REGISTRYINDEX, "traceback");
	lua_insert(L, 1); // Insert traceback function at start of stack
	err = lua_pcall(L, n - 1, LUA_MULTRET, 1);
	if (err) {
		return 1;
	}
	lua_pushnil(L);
	lua_replace(L, 1); // Replace traceback function with nil
	return lua_gettop(L);
}
SG_LUA_CPP_FUN_END()

static int l_PCall(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: PCall(func[, ...])");
	ui->LAssert(L, lua_isfunction(L, 1), "PCall() argument 1: expected function, got %s", luaL_typename(L, 1));
	lua_getfield(L, LUA_REGISTRYINDEX, "traceback");
	lua_insert(L, 1); // Insert traceback function at start of stack
	int err = lua_pcall(L, n - 1, LUA_MULTRET, 1);
	if (err) {
		return 1;
	}
	lua_pushnil(L);
	lua_replace(L, 1); // Replace traceback function with nil
	return lua_gettop(L);
}

static int l_ConPrintf(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: ConPrintf(fmt[, ...])");
	ui->LAssert(L, lua_isstring(L, 1), "ConPrintf() argument 1: expected string, got %s", luaL_typename(L, 1));
	lua_pushvalue(L, lua_upvalueindex(1));	// string.format
	lua_insert(L, 1);
	lua_call(L, n, 1);
	ui->LAssert(L, lua_isstring(L, 1), "ConPrintf() error: string.format returned non-string");
	ui->sys->con->Printf("%s\n", lua_tostring(L, 1));
	return 0;
}

static void printTableItter(lua_State* L, IConsole* con, int index, int level, bool recurse)
{
	lua_checkstack(L, 5);
	lua_pushnil(L);
	while (lua_next(L, index)) {
		for (int t = 0; t < level; t++) con->Print("  ");
		// Print key
		if (lua_type(L, -2) == LUA_TSTRING) {
			con->Printf("[\"%s^7\"] = ", lua_tostring(L, -2));
		}
		else {
			lua_pushvalue(L, 2);	// Push tostring function
			lua_pushvalue(L, -3);	// Push key
			lua_call(L, 1, 1);		// Call tostring
			con->Printf("%s = ", lua_tostring(L, -1));
			lua_pop(L, 1);			// Pop result of tostring
		}
		// Print value
		if (lua_type(L, -1) == LUA_TTABLE) {
			bool expand = recurse;
			if (expand) {
				lua_pushvalue(L, -1);	// Push value
				lua_gettable(L, 3);		// Index printed tables list
				expand = lua_toboolean(L, -1) == 0;
				lua_pop(L, 1);			// Pop result of indexing
			}
			if (expand) {
				lua_pushvalue(L, -1);	// Push value
				lua_pushboolean(L, 1);
				lua_settable(L, 3);		// Add to printed tables list
				con->Printf("table: %08x {\n", lua_topointer(L, -1));
				printTableItter(L, con, lua_gettop(L), level + 1, true);
				for (int t = 0; t < level; t++) con->Print("  ");
				con->Print("}\n");
			}
			else {
				con->Printf("table: %08x { ... }\n", lua_topointer(L, -1));
			}
		}
		else if (lua_type(L, -1) == LUA_TSTRING) {
			con->Printf("\"%s\"\n", lua_tostring(L, -1));
		}
		else {
			lua_pushvalue(L, 2);	// Push tostring function
			lua_pushvalue(L, -2);	// Push value
			lua_call(L, 1, 1);		// Call tostring
			con->Printf("%s\n", lua_tostring(L, -1));
			lua_pop(L, 1);			// Pop result of tostring
		}
		lua_pop(L, 1);	// Pop value
	}
}

static int l_ConPrintTable(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: ConPrintTable(tbl[, noRecurse])");
	ui->LAssert(L, lua_istable(L, 1), "ConPrintTable() argument 1: expected table, got %s", luaL_typename(L, 1));
	bool recurse = lua_toboolean(L, 2) == 0;
	lua_settop(L, 1);
	lua_getglobal(L, "tostring");
	lua_newtable(L);		// Printed tables list
	lua_pushvalue(L, 1);	// Push root table
	lua_pushboolean(L, 1);
	lua_settable(L, 3);		// Add root table to printed tables list
	printTableItter(L, ui->sys->con, 1, 0, recurse);
	return 0;
}

static int l_ConExecute(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: ConExecute(cmd)");
	ui->LAssert(L, lua_isstring(L, 1), "ConExecute() argument 1: expected string, got %s", luaL_typename(L, 1));
	ui->sys->con->Execute(lua_tostring(L, 1));
	return 0;
}

static int l_ConClear(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->sys->con->Clear();
	return 0;
}

static int l_print(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	lua_getglobal(L, "tostring");
	for (int i = 1; i <= n; i++) {
		lua_pushvalue(L, -1);	// Push tostring function
		lua_pushvalue(L, i);
		lua_call(L, 1, 1);		// Call tostring
		const char* s = lua_tostring(L, -1);
		ui->LAssert(L, s != NULL, "print() error: tostring returned non-string");
		if (i > 1) ui->sys->con->Print(" ");
		ui->sys->con->Print(s);
		lua_pop(L, 1);			// Pop result of tostring
	}
	ui->sys->con->Print("\n");
	return 0;
}

// Directory containing this engine module (SimpleGraphic.dll).
std::filesystem::path EngineModuleDir()
{
#ifdef _WIN32
	HMODULE mod = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCWSTR)&EngineModuleDir, &mod)) {
		wchar_t buf[MAX_PATH];
		if (GetModuleFileNameW(mod, buf, MAX_PATH)) {
			return std::filesystem::path(buf).parent_path();
		}
	}
#endif
	return {};
}

// Engine config files (SimpleGraphic/<name>): prefer the CWD-relative
// location when it exists (upstream and legacy flat layouts keep it next to
// the exe = basePath), otherwise resolve next to the DLL (engine\ layout).
std::filesystem::path ResolveEngineCfg(const char* name)
{
	std::filesystem::path rel = std::filesystem::path("SimpleGraphic") / name;
	std::error_code ec;
	if (std::filesystem::exists(rel, ec)) {
		return rel;
	}
	std::filesystem::path dir = EngineModuleDir();
	if (!dir.empty()) {
		return dir / "SimpleGraphic" / name;
	}
	return rel;
}

// PobCharm: POB's runtime updater (Update.exe, spawned by Launch.lua's
// ApplyUpdate) finishes by executing the op list's `start "...\Path of
// Building.exe"` line, which would bring back the stock untranslated POB.
// Before the updater launches, rewrite that target to the host exe that is
// running us (pob-zh.exe), and drop a relaunch marker next to it so the host
// skips its launcher UI once and reopens this POB directly.
static void pobcharm_redirect_update_restart(ui_main_c* ui)
{
#ifdef _WIN32
	wchar_t exeBuf[MAX_PATH];
	if (!GetModuleFileNameW(nullptr, exeBuf, MAX_PATH)) return;
	std::filesystem::path hostExe = exeBuf;

	std::filesystem::path opFile = ui->scriptWorkDir / "Update" / "opFileRuntime.txt";
	std::ifstream in(opFile);
	if (!in) return;
	std::vector<std::string> lines;
	std::string line;
	bool found = false;
	while (std::getline(in, line)) {
		if (line.rfind("start ", 0) == 0) {
			line = "start \"" + hostExe.generic_u8string() + "\"";
			found = true;
		}
		lines.push_back(line);
	}
	in.close();
	if (!found) lines.push_back("start \"" + hostExe.generic_u8string() + "\"");
	std::ofstream out(opFile, std::ios::trunc);
	if (!out) return;
	for (auto& l : lines) out << l << "\n";
	out.close();

	// Marker tells the host which Launch.lua to reopen, bypassing the launcher UI.
	std::ofstream marker(hostExe.parent_path() / "pob-zh.relaunch", std::ios::trunc);
	marker << ui->scriptName.generic_u8string();
#endif
}

static int l_SpawnProcess(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: SpawnProcess(cmdName[, args])");
	ui->LAssert(L, lua_isstring(L, 1), "SpawnProcess() argument 1: expected string, got %s", luaL_typename(L, 1));
	auto cmdPath = std::filesystem::u8path(lua_tostring(L, 1));
	auto args = lua_tostring(L, 2);
	if (cmdPath.stem() == "Update") {
		pobcharm_redirect_update_restart(ui);
	}
	ui->sys->SpawnProcess(cmdPath, args);
	return 0;
}

static int l_OpenURL(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: OpenURL(url)");
	ui->LAssert(L, lua_isstring(L, 1), "OpenURL() argument 1: expected string, got %s", luaL_typename(L, 1));
	if (auto errMsg = ui->sys->OpenURL(lua_tostring(L, 1))) {
		lua_pushstring(L, errMsg->c_str());
		return 1;
	}
	return 0;
}

static int l_SetProfiling(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	ui->LAssert(L, n >= 1, "Usage: SetProfiling(isEnabled)");
	ui->debug->SetProfiling(lua_toboolean(L, 1) == 1);
	return 0;
}

static int l_Restart(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->restartFlag = true;
	return 0;
}

static int l_Exit(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	int n = lua_gettop(L);
	const char* msg = NULL;
	if (n >= 1 && !lua_isnil(L, 1)) {
		ui->LAssert(L, lua_isstring(L, 1), "Exit() argument 1: expected string or nil, got %s", luaL_typename(L, 1));
		msg = lua_tostring(L, 1);
	}
	ui->sys->Exit(msg);
	ui->didExit = true;
	//	lua_pushstring(L, "dummy");
	//	lua_error(L);
	return 0;
}

static int l_TakeScreenshot(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->sys->con->Execute("screenshot");
	return 0;
}

static int l_SetForeground(lua_State* L)
{
	ui_main_c* ui = GetUIPtr(L);
	ui->sys->video->SetForeground();
	return 0;
}

// PobToolsTranslate(english) -> chinese, or nil. Lua-callable forward lookup.
// (Legacy global name PoeCharmTranslate is kept as an alias in InitAPI.)
static int l_PobToolsTranslate(lua_State* L)
{
	const char* text = lua_tostring(L, 1);
	if (!text) { lua_pushnil(L); return 1; }
	const char* zh = translation_lookup(text);
	if (zh) lua_pushstring(L, zh); else lua_pushnil(L);
	return 1;
}

// PobToolsReverse(chinese) -> english, or nil. Lua-callable single-term reverse lookup.
static int l_PobToolsReverse(lua_State* L)
{
	const char* text = lua_tostring(L, 1);
	if (!text) { lua_pushnil(L); return 1; }
	const char* en = translation_reverse_lookup(text);
	if (en) lua_pushstring(L, en); else lua_pushnil(L);
	return 1;
}

// PobToolsSetTranslate(enabled) -> previous enabled state. Lets POB turn off
// DrawString translation while rendering content that must stay in English
// (e.g. the About/Update changelog), then restore the previous state.
static int l_PobToolsSetTranslate(lua_State* L)
{
	bool prev = translation_is_enabled();
	translation_set_enabled(lua_toboolean(L, 1) != 0);
	lua_pushboolean(L, prev ? 1 : 0);
	return 1;
}

// ==============================
// Library and API Initialisation
// ==============================

#define ADDFUNC(n) lua_pushcclosure(L, l_##n, 0);lua_setglobal(L, #n);
#define ADDFUNCCL(n, u) lua_pushcclosure(L, l_##n, u);lua_setglobal(L, #n);
// Register C function l_##fn under a different global name (for legacy aliases).
#define ADDFUNCALIAS(global, fn) lua_pushcclosure(L, l_##fn, 0);lua_setglobal(L, #global);

int ui_main_c::InitAPI(lua_State* L)
{
	sol::state_view lua(L);
	luaL_openlibs(L);

	// Add "lua/" subdir for non-JIT Lua
	{
		lua_getglobal(L, "package");
		char const* tn = lua_typename(L, -1);
		lua_getfield(L, -1, "path");
		std::string old_path = lua_tostring(L, -1);
		lua_pop(L, 1);
		// Also cover directory modules (lua/sha1/init.lua); LuaJIT's default
		// "!\lua\?\init.lua" expands from the exe dir, which is not the POB dir here.
		old_path += ";lua/?.lua;lua/?/init.lua";
		// ANSI on purpose: LuaJIT's loadlib/loadfile use the ANSI APIs, the
		// same limitation as its built-in "!\?.dll" expansion.
		std::string dllDir = EngineModuleDir().generic_string();
		if (!dllDir.empty()) {
			old_path += ";" + dllDir + "/lua/?.lua;" + dllDir + "/lua/?/init.lua";
		}
		lua_pushstring(L, old_path.c_str());
		lua_setfield(L, -2, "path");

		// Lua C modules (lcurl/socket/lua-utf8/lzip) ship next to this DLL.
		// PREPEND: the POB folder bundles its own older copies of these DLLs
		// and both LuaJIT's "!\?.dll" (exe dir) and ".\?.dll" (CWD = POB
		// folder) would otherwise win the search.
		if (!dllDir.empty()) {
			lua_getfield(L, -1, "cpath");
			std::string cpath = lua_tostring(L, -1);
			lua_pop(L, 1);
			cpath = dllDir + "/?.dll;" + cpath;
			lua_pushstring(L, cpath.c_str());
			lua_setfield(L, -2, "cpath");
		}
		lua_pop(L, 1);
	}

	// Callbacks
	lua_newtable(L);		// Callbacks table
	lua_pushvalue(L, -1);	// Push callbacks table
	ADDFUNCCL(SetCallback, 1);
	lua_pushvalue(L, -1);	// Push callbacks table
	ADDFUNCCL(GetCallback, 1);
	lua_pushvalue(L, -1);	// Push callbacks table
	ADDFUNCCL(SetMainObject, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, "uicallbacks");

	// Image handles
	lua_newtable(L);		// Image handle metatable
	lua_pushvalue(L, -1);	// Push image handle metatable
	ADDFUNCCL(NewImageHandle, 1);
	lua_pushvalue(L, -1);	// Push image handle metatable
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, l_imgHandleGC);
	lua_setfield(L, -2, "__gc");
	lua_pushcfunction(L, l_imgHandleLoad);
	lua_setfield(L, -2, "Load");
	lua_pushcfunction(L, l_imgHandleUnload);
	lua_setfield(L, -2, "Unload");
	lua_pushcfunction(L, l_imgHandleIsValid);
	lua_setfield(L, -2, "IsValid");
	lua_pushcfunction(L, l_imgHandleIsLoading);
	lua_setfield(L, -2, "IsLoading");
	lua_pushcfunction(L, l_imgHandleSetLoadingPriority);
	lua_setfield(L, -2, "SetLoadingPriority");
	lua_pushcfunction(L, l_imgHandleImageSize);
	lua_setfield(L, -2, "ImageSize");
	lua_pushcfunction(L, l_imgHandleLoadArtRectangle);
	lua_setfield(L, -2, "LoadArtRectangle");
	lua_pushcfunction(L, l_imgHandleLoadArtArcBand);
	lua_setfield(L, -2, "LoadArtArcBand");
	lua_setfield(L, LUA_REGISTRYINDEX, "uiimghandlemeta");

	// Art handles
	lua_newtable(L);		// Art handle metatable
	lua_pushvalue(L, -1);	// Push art handle metatable
	ADDFUNCCL(NewArtHandle, 1);
	lua_pushvalue(L, -1);	// Push art handle metatable
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, l_artHandleGC);
	lua_setfield(L, -2, "__gc");
	lua_pushcfunction(L, l_artHandleSize);
	lua_setfield(L, -2, "Size");
	lua_setfield(L, LUA_REGISTRYINDEX, "uiarthandlemeta");

	sol::usertype<Texture_c> textureType = lua.new_usertype<Texture_c>("Texture",
		sol::constructors<Texture_c()>());

	textureType["Allocate"] = sol::overload(
		sol::resolve<bool(gli::format,int,int,int,int)>(&Texture_c::Allocate),
		sol::resolve<bool(std::string_view,int,int,int,int)>(&Texture_c::Allocate));
	textureType["Load"] = &Texture_c::Load;
	textureType["Save"] = &Texture_c::Save;
	textureType["Info"] = &Texture_c::Info;
	textureType["IsValid"] = &Texture_c::IsValid;
	
	//textureType["SetLayer"] = &Texture_c::SetLayer;
	//textureType["CopyImage"] = &Texture_c::CopyImage;
	//textureType["Transcode"] = &Texture_c::Transcode;
	//textureType["GenerateMipmaps"] = &Texture_c::Transcode;

	textureType["StackTextures"] = &Texture_c::StackTextures;

	sol::usertype<TextureInfo_s> textureInfoType = lua.new_usertype<TextureInfo_s>("TextureInfo");

	textureInfoType["formatId"] = &TextureInfo_s::formatId;
	textureInfoType["formatStr"] = &TextureInfo_s::formatStr;
	textureInfoType["width"] = &TextureInfo_s::width;
	textureInfoType["height"] = &TextureInfo_s::height;
	textureInfoType["layerCount"] = &TextureInfo_s::layerCount;
	textureInfoType["mipCount"] = &TextureInfo_s::mipCount;

	// Rendering
	ADDFUNC(RenderInit);
	ADDFUNC(GetScreenSize);
	ADDFUNC(GetScreenScale);
	ADDFUNC(SetClearColor);
	ADDFUNC(SetDrawLayer);
	ADDFUNC(GetDrawLayer);
	ADDFUNC(SetViewport);
	ADDFUNC(SetBlendMode);
	ADDFUNC(SetDrawColor);
	ADDFUNC(GetDrawColor);
	ADDFUNC(SetDPIScaleOverridePercent);
	ADDFUNC(GetDPIScaleOverridePercent);
	ADDFUNC(DrawImage);
	ADDFUNC(DrawImageQuad);
	ADDFUNC(DrawString);
	ADDFUNC(DrawStringWidth);
	ADDFUNC(DrawStringCursorIndex);
	ADDFUNC(StripEscapes);
	ADDFUNC(GetAsyncCount);
	ADDFUNC(RenderInit);

	// Search handles
	lua_newtable(L);	// Search handle metatable
	lua_pushvalue(L, -1);	// Push search handle metatable
	ADDFUNCCL(NewFileSearch, 1);
	lua_pushvalue(L, -1);	// Push search handle metatable
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, l_searchHandleGC);
	lua_setfield(L, -2, "__gc");
	lua_pushcfunction(L, l_searchHandleNextFile);
	lua_setfield(L, -2, "NextFile");
	lua_pushcfunction(L, l_searchHandleGetFileName);
	lua_setfield(L, -2, "GetFileName");
	lua_pushcfunction(L, l_searchHandleGetFileSize);
	lua_setfield(L, -2, "GetFileSize");
	lua_pushcfunction(L, l_searchHandleGetFileModifiedTime);
	lua_setfield(L, -2, "GetFileModifiedTime");
	lua_setfield(L, LUA_REGISTRYINDEX, "uisearchhandlemeta");

	// General function
	ADDFUNC(GetCloudProvider);
	ADDFUNC(SetWindowTitle);
	ADDFUNC(GetCursorPos);
	ADDFUNC(SetCursorPos);
	ADDFUNC(ShowCursor);
	ADDFUNC(IsKeyDown);
	ADDFUNC(Copy);
	ADDFUNC(Paste);
	ADDFUNC(Deflate);
	ADDFUNC(Inflate);
	ADDFUNC(GetTime);
	ADDFUNC(GetScriptPath);
	ADDFUNC(GetRuntimePath);
	ADDFUNC(GetUserPath);
	ADDFUNC(MakeDir);
	ADDFUNC(RemoveDir);
	ADDFUNC(SetWorkDir);
	ADDFUNC(GetWorkDir);
	ADDFUNC(LaunchSubScript);
	ADDFUNC(AbortSubScript);
	ADDFUNC(IsSubScriptRunning);
	ADDFUNC(LoadModule);
	ADDFUNC(PLoadModule);
	ADDFUNC(PCall);
	lua_getglobal(L, "string");
	lua_getfield(L, -1, "format");
	ADDFUNCCL(ConPrintf, 1);
	lua_pop(L, 1);		// Pop 'string' table
	ADDFUNC(ConPrintTable);
	ADDFUNC(ConExecute);
	ADDFUNC(ConClear);
	ADDFUNC(print);
	ADDFUNC(SpawnProcess);
	ADDFUNC(OpenURL);
	ADDFUNC(SetProfiling);
	ADDFUNC(TakeScreenshot);
	ADDFUNC(Restart);
	ADDFUNC(Exit);
	ADDFUNC(SetForeground);
	lua_getglobal(L, "os");
	lua_pushcfunction(L, l_Exit);
	lua_setfield(L, -2, "exit");
	lua_pop(L, 1);		// Pop 'os' table

	// PobTools: Lua-callable translation helpers. The legacy PoeCharm* global
	// names are kept as aliases so existing injected Lua keeps working.
	ADDFUNC(PobToolsTranslate);
	ADDFUNC(PobToolsReverse);
	ADDFUNC(PobToolsSetTranslate);
	ADDFUNCALIAS(PoeCharmTranslate, PobToolsTranslate);
	ADDFUNCALIAS(PoeCharmReverse, PobToolsReverse);
	ADDFUNCALIAS(PoeCharmSetTranslate, PobToolsSetTranslate);

	// PobTools: load translation dictionaries (POB_LOCALE / POB_GAME env-driven).
	// No-op passthrough when no locale is configured.
	translation_init();

	// PoeCharm: expose lua-utf8 as the global `utf8` BEFORE POB runs, so POB's
	// main:DetectUnicodeSupport() (checks type(_G.utf8)=="table") turns on
	// main.unicode and EditControl accepts CJK input. Engine-side only — POB's
	// own files are never modified. cpath was prepended with the engine dir
	// above so require() finds lua-utf8.dll; pcall guards a missing module.
	if (luaL_dostring(L, "local ok, m = pcall(require, 'lua-utf8'); if ok then _G.utf8 = m end")) {
		lua_pop(L, 1); // ignore (should not happen: inner pcall handles failure)
	}

	return 0;
}


