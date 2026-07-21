// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Module: System Video
// Platform: Windows
//

#include <glad/gles2.h>

#include "sys_local.h"
#include "core.h"

#include <GLFW/glfw3.h>

#include <deque>
#include <map>
#include <optional>
#include <utility>

#include <imgui.h>
#include <stb_image_resize.h>

// ====================
// sys_IVideo Interface
// ====================

class sys_video_c : public sys_IVideo {
public:
	// Interface
	int		Apply(sys_vidSet_s* set);

	void	SetForeground();
	bool	IsActive();
	void	FramebufferSizeChanged(int width, int height);
	void	SizeChanged(int width, int height, bool max);
	void	PosChanged(int x, int y);
	void	GetMinSize(int& width, int& height);
	void	SetVisible(bool vis);
	bool	IsVisible();
	void	SetTitle(const char* title);
	void* GetWindowHandle();
	void	GetRelativeCursor(int& x, int& y);
	void	SetRelativeCursor(int x, int y);
	bool	IsCursorOverWindow();

	// Encapsulated
	sys_video_c(sys_IMain* sysHnd);
	~sys_video_c();

	sys_main_c* sys = nullptr;

	bool	initialised = false;
	bool	ignoreDpiScale = false;
	GLFWwindow* wnd = nullptr;

	void	RefreshMonitorInfo();

	int		numMon = 0;			// Number of monitors
	int		priMon = 0;			// Index of primary monitor
	struct {
		GLFWmonitor* hnd = nullptr;
		int		left = 0;
		int		top = 0;
		int		width = 0;
		int		height = 0;
	} mon[16];					// Array of monitor specs

	int		defRes[2] = {};		// Default resolution
	sys_vidSet_s cur;			// Current settings
	int		scrSize[2] = {};	// Screen size
	int		minSize[2] = {};	// Minimum window size
	char	curTitle[512] = {};	// Window title

	bool cursorInWindow = false;

	struct CursorPos {
		int x, y;
	};
	std::optional<CursorPos> lastCursorPos;

	struct ClickEvent
	{
		std::chrono::system_clock::time_point time;
		CursorPos pos;
		byte button;
	};
	std::optional<ClickEvent> lastClick;
};

sys_IVideo* sys_IVideo::GetHandle(sys_IMain* sysHnd)
{
	return new sys_video_c(sysHnd);
}

void sys_IVideo::FreeHandle(sys_IVideo* hnd)
{
	delete (sys_video_c*)hnd;
}

sys_video_c::sys_video_c(sys_IMain* sysHnd)
	: sys((sys_main_c*)sysHnd)
{
	initialised = false;

	minSize[0] = minSize[1] = 0;

	strcpy(curTitle, CFG_TITLE);

	int platformType = GLFW_ANGLE_PLATFORM_TYPE_NONE;
#ifdef _WIN32
	const std::string wineHost = GetWineHostVersion();
	if (wineHost == "Linux")
		platformType = GLFW_ANGLE_PLATFORM_TYPE_OPENGL;
	else if (wineHost == "Darwin")
		platformType = GLFW_ANGLE_PLATFORM_TYPE_D3D11;
	else // Native Windows
		platformType = GLFW_ANGLE_PLATFORM_TYPE_D3D11;
#endif
	glfwInitHint(GLFW_ANGLE_PLATFORM_TYPE, platformType);
	glfwInit();
}

sys_video_c::~sys_video_c()
{
	if (initialised) {
		glfwDestroyWindow(wnd);
	}

	glfwTerminate();
}

std::optional<std::pair<double, double>> PlatformGetCursorPos() {
#if _WIN32
	POINT curPos;
	GetCursorPos(&curPos);
	return std::make_pair((double)curPos.x, (double)curPos.y);
#else
	#warning LV : Global cursor position queries not implemented yet on this OS.
		// TODO(LV): Implement on other OSes
		return {};
#endif
}

struct sys_programIcons_c {
#if _WIN32
	sys_programIcons_c()
	{
#pragma pack( push )
#pragma pack( 2 )
		typedef struct
		{
			BYTE   bWidth;               // Width, in pixels, of the image
			BYTE   bHeight;              // Height, in pixels, of the image
			BYTE   bColorCount;          // Number of colors in image (0 if >=8bpp)
			BYTE   bReserved;            // Reserved
			WORD   wPlanes;              // Color Planes
			WORD   wBitCount;            // Bits per pixel
			DWORD  dwBytesInRes;         // how many bytes in this resource?
			WORD   nID;                  // the ID
		} GRPICONDIRENTRY, * LPGRPICONDIRENTRY;

		typedef struct
		{
			WORD            idReserved;   // Reserved (must be 0)
			WORD            idType;       // Resource type (1 for icons)
			WORD            idCount;      // How many images?
			GRPICONDIRENTRY idEntries[1]; // The entries for each image
		} GRPICONDIR, * LPGRPICONDIR;
#pragma pack( pop )

		// Find the first icon group in the executable.
		HMODULE mod = nullptr;
		LPWSTR firstIconId{};
		auto enumFunc = [](HMODULE mod, LPCWSTR type, LPWSTR name, LONG_PTR lparam) -> BOOL {
			*(LPWSTR*)lparam = name;
			return FALSE;
			};
		if (EnumResourceNamesW(mod, RT_GROUP_ICON, enumFunc, (LONG_PTR)&firstIconId) == FALSE
			&& GetLastError() != ERROR_RESOURCE_ENUM_USER_STOP)
		{
			return;
		}

		// Find and decode all existing icons.
		std::map<int, GLFWimage> imagesBySize;
		{
			HDC dc = GetDC(nullptr);
			HRSRC groupRsrc = FindResourceW(mod, firstIconId, RT_GROUP_ICON);
			HGLOBAL groupGlobal = LoadResource(mod, groupRsrc);
			auto* groupIconDir = (GRPICONDIR*)LockResource(groupGlobal);

			for (int i = 0; i < groupIconDir->idCount; ++i) {
				auto& e = groupIconDir->idEntries[i];

				HRSRC iconRsrc = FindResourceW(mod, MAKEINTRESOURCEW(e.nID), RT_ICON);
				HGLOBAL iconGlobal = LoadResource(mod, iconRsrc);
				auto* data = (BYTE*)LockResource(iconGlobal);
				DWORD size = SizeofResource(mod, iconRsrc);
				HICON icon = CreateIconFromResourceEx(data, size, TRUE, 0x0003'0000, 0, 0, LR_DEFAULTCOLOR);

				ICONINFO iconInfo{};
				GetIconInfo(icon, &iconInfo);

				BITMAP bmp{};
				GetObject(iconInfo.hbmColor, sizeof(bmp), &bmp);

				std::vector<uint8_t> bytes(bmp.bmWidthBytes * bmp.bmHeight);
				std::vector<uint8_t> rgba(bmp.bmWidthBytes * bmp.bmHeight);

				BITMAPINFO bmi{};
				bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
				GetDIBits(dc, iconInfo.hbmColor, 0, 0, nullptr, &bmi, DIB_RGB_COLORS);

				bmi.bmiHeader.biBitCount = 32;
				bmi.bmiHeader.biCompression = BI_RGB;
				bmi.bmiHeader.biHeight = abs(bmi.bmiHeader.biHeight);
				if (GetDIBits(dc, iconInfo.hbmColor, 0, bmi.bmiHeader.biHeight, bytes.data(), &bmi, DIB_RGB_COLORS)) {
					GLFWimage image{};
					image.width = bmi.bmiHeader.biWidth;
					image.height = bmi.bmiHeader.biHeight;
					SwizzleFlipImage(image.width, image.height, bytes.data(), rgba.data());
					imageDatas.push_back(std::move(rgba));
					image.pixels = (unsigned char*)imageDatas.back().data();
					imagesBySize[image.width] = image;
				}
				DeleteObject(iconInfo.hbmColor);
				DeleteObject(iconInfo.hbmMask);
				DestroyIcon(icon);
				UnlockResource(iconGlobal);
			}

			UnlockResource(groupGlobal);
			ReleaseDC(nullptr, dc);
		}

		// Generate most smaller sizes that icons can be for different display scale factors.
		int desiredSizes[] = { 64, 48, 40, 32, 24, 20, 16 };
		auto srcImage = imagesBySize.rbegin()->second;
		for (auto size : desiredSizes) {
			if (imagesBySize.count(size) != 0) {
				continue;
			}
			std::vector<uint8_t> newRgba(size * size * 4);
			stbir_resize_uint8(srcImage.pixels, srcImage.width, srcImage.height, srcImage.width * 4,
				newRgba.data(), size, size, size * 4, 4);
			imageDatas.push_back(std::move(newRgba));
			GLFWimage newImage;
			newImage.width = size;
			newImage.height = size;
			newImage.pixels = imageDatas.back().data();
			imagesBySize[size] = newImage;
		}

		for (auto I = imagesBySize.rbegin(); I != imagesBySize.rend(); ++I) {
			images.push_back(I->second);
		}
	}

private:
	void SwizzleFlipImage(int width, int height, uint8_t const* inPtr, uint8_t* outPtr) {
		// Map from upside down rows of BGRA to RGBA.
		for (size_t row = 0; row < height; ++row) {
			size_t stride = width * 4;
			auto* src = &inPtr[row * stride];
			auto* dst = &outPtr[(height - row - 1) * stride];
			for (size_t col = 0; col < width; ++col) {
				*dst++ = src[2];
				*dst++ = src[1];
				*dst++ = src[0];
				*dst++ = src[3];
				src += 4;
			}
		}
	}
#endif

public:
	size_t Size()
	{
		return images.size();
	}

	GLFWimage const* Data()
	{
		return images.data();
	}

	std::vector<GLFWimage> images;
	std::deque<std::vector<uint8_t>> imageDatas;
};

bool ShouldIgnoreDpiScale() {
#ifdef _WIN32
	std::wstring const appChoice = L"HIGHDPIAWARE", sysChoice = L"DPIUNAWARE", enhanceFlag = L"GDIDPISCALING";
	std::wstring const subKey = LR"(Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers)";

	std::vector<wchar_t> progStr(1 << 16);
	GetModuleFileNameW(NULL, progStr.data(), progStr.size());
	std::filesystem::path progPath(progStr.data());
	progPath = std::filesystem::canonical(progPath);

	auto considerKey = [&](HKEY rootKey) -> std::optional<bool> {
		std::vector<wchar_t> valData(1 << 16);
		DWORD valType{}, valSize = valData.size() / 2;
		LRESULT res = RegGetValueW(rootKey, subKey.c_str(), progPath.wstring().c_str(), RRF_RT_REG_SZ, &valType, valData.data(), &valSize);
		if (res == ERROR_SUCCESS) {
			std::wstring val = valData.data();
			if (wcsstr(val.c_str(), appChoice.c_str())) {
				return true;
			}
			else if (wcsstr(val.c_str(), sysChoice.c_str())) {
				return false;
			}
		}
		return {};
		};

	struct ScopedRegKey {
		HKEY key{};

		ScopedRegKey& operator = (ScopedRegKey const&) = delete;
		ScopedRegKey(ScopedRegKey const&) = delete;
		~ScopedRegKey() {
			if (key) { RegCloseKey(key); }
		}

		HKEY* operator & () { return &key; }
		operator HKEY () const { return key; }
	};

	// Prioritize the global setting over the user setting to follow Windows' semantics.
	ScopedRegKey hklm{};
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, nullptr, 0, KEY_READ, &hklm) == ERROR_SUCCESS) {
		if (auto ignore = considerKey(hklm)) {
			return *ignore;
		}
	}

	ScopedRegKey hkcu{};
	if (RegOpenCurrentUser(KEY_READ, &hkcu) == ERROR_SUCCESS) {
		if (auto ignore = considerKey(hkcu)) {
			return *ignore;
		}
	}
#endif
	return false;
}

// ==================
// System Video Class
// ==================

int sys_video_c::Apply(sys_vidSet_s* set)
{
	cur = *set;

	GLFWmonitor** monitors = glfwGetMonitors(&numMon);
	for (int i = 0; i < numMon; ++i) {
		mon[i].hnd = monitors[i];
		glfwGetMonitorPos(monitors[i], &mon[i].left, &mon[i].top);
		GLFWvidmode const* mode = glfwGetVideoMode(monitors[i]);
		mon[i].width = mode->width;
		mon[i].height = mode->height;
	}
	priMon = 0;

	struct WindowRect {
		int left, top;
		int right, bottom;
	};

	// Determine which monitor to create window on
	// Use monitor containing the mouse cursor if available, otherwise primary monitor
	int display = 0;
	if (auto curPos = PlatformGetCursorPos()) {
		auto [curX, curY] = *curPos;
		for (int m = 0; m < numMon; ++m) {
			int right = mon[m].left + mon[m].width;
			int bottom = mon[m].top + mon[m].height;
			if (curX >= mon[m].left && curY >= mon[m].top && curX < right && curY < bottom) {
				display = m;
				break;
			}
		}
	}
	defRes[0] = mon[display].width;
	defRes[1] = mon[display].height;

	minSize[0] = minSize[1] = 0;

	if (cur.mode[0] == 0) {
		// Use default resolution if one isn't specified
		Vector2Copy(defRes, cur.mode);
	}
	Vector2Copy(cur.mode, vid.size);
	Vector2Copy(defRes, scrSize);

	// Get window rectangle
	WindowRect wrec{};
	std::optional<int> intersectedMonitor;
	if (cur.flags & VID_USESAVED) {
		if (cur.save.maximised) {
			cur.flags |= VID_MAXIMIZE;
		}
		else {
			cur.mode[0] = cur.save.size[0];
			cur.mode[1] = cur.save.size[1];
		}

		wrec.left = cur.save.pos[0];
		wrec.top = cur.save.pos[1];
		wrec.right = wrec.left + cur.mode[0];
		wrec.bottom = wrec.top + cur.mode[1];

		for (int m = 0; m < numMon; ++m) {
			WindowRect drec{ mon[m].left, mon[m].top };
			drec.right = drec.left + mon[m].width;
			drec.bottom = drec.top + mon[m].height;

			// A.lo < B.hi && A.hi > B.lo (half-open rects)
			bool intersectsDisplay = drec.left < wrec.right && drec.top < wrec.bottom && drec.right > wrec.left && drec.bottom > wrec.top;
			if (!intersectedMonitor && intersectsDisplay) {
				intersectedMonitor = m;
				break;
			}	
		}
	}

	if (!intersectedMonitor) {
		wrec.left = (scrSize[0] - cur.mode[0]) / 2 + mon[display].left;
		wrec.top = (scrSize[1] - cur.mode[1]) / 2 + mon[display].top;
	}
	vid.pos[0] = wrec.left;
	vid.pos[1] = wrec.top;

	if (initialised) {
		if (!!glfwGetWindowAttrib(wnd, GLFW_MAXIMIZED)) {
			glfwRestoreWindow(wnd);
		}
		glfwSetWindowPos(wnd, wrec.left, wrec.top);
		glfwSetWindowSize(wnd, wrec.right - wrec.left, wrec.bottom - wrec.top);
		if (cur.flags & VID_MAXIMIZE) {
			glfwMaximizeWindow(wnd);
		}
		if (cur.shown) {
			glfwShowWindow(wnd);
			sys->conWin->SetForeground();
		}
		else {
			glfwHideWindow(wnd);
		}
	}
	else {
		ignoreDpiScale = ShouldIgnoreDpiScale();
		glfwWindowHint(GLFW_RESIZABLE, !!(cur.flags & VID_RESIZABLE));
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); // Start hidden to not flash the user with a stock window.
		glfwWindowHint(GLFW_MAXIMIZED, GLFW_FALSE); // Start restored in order to position the window before maximizing.
		glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
		glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
		glfwWindowHint(GLFW_DEPTH_BITS, 24);
		//glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);

		wnd = glfwCreateWindow(cur.mode[0], cur.mode[1], curTitle, nullptr, nullptr);
		if (!wnd) {
			char const* errDesc = "Unknown error";
			glfwGetError(&errDesc);
			sys->con->Printf("Could not create window, %s\n", errDesc);
		}

		glfwMakeContextCurrent(wnd);
		gladLoadGLES2(glfwGetProcAddress);

		// Set up all our window callbacks
		glfwSetWindowUserPointer(wnd, sys);
		glfwSetCursorEnterCallback(wnd, [](GLFWwindow* wnd, int entered) {
			auto sys = (sys_main_c*)glfwGetWindowUserPointer(wnd);
			auto video = (sys_video_c*)sys->video;
			bool is_inside = !!entered;
			video->cursorInWindow = is_inside;
			if (!is_inside) {
				video->lastCursorPos.reset();
			}
			});
		glfwSetCursorPosCallback(wnd, [](GLFWwindow* wnd, double x, double y) {
			auto sys = (sys_main_c*)glfwGetWindowUserPointer(wnd);
			if (ImGui::GetIO().WantCaptureMouse) {
				return;
			}
			auto video = (sys_video_c*)sys->video;
			video->lastCursorPos = CursorPos{ (int)x, (int)y };
			});
		glfwSetWindowCloseCallback(wnd, [](GLFWwindow* wnd) {
			auto sys = (sys_main_c*)glfwGetWindowUserPointer(wnd);
			glfwSetWindowShouldClose(wnd, sys->initialised && sys->core->CanExit());
			});
		glfwSetWindowIconifyCallback(wnd, [](GLFWwindow* wnd, int value) {
			auto sys = (sys_main_c*)glfwGetWindowUserPointer(wnd);
			sys->minimized = value == 1 ? true : false;
			});
		glfwSetFramebufferSizeCallback(wnd, [](GLFWwindow* wnd, int width, int height) {
			auto sys = (sys_main_c*)glfwGetWindowUserPointer(wnd);
			sys->video->FramebufferSizeChanged(width, height);
			});
		glfwSetWindowSizeCallback(wnd, [](GLFWwindow* wnd, int width, int height) {
			auto sys = (sys_main_c*)glfwGetWindowUserPointer(wnd);
			bool maximized = glfwGetWindowAttrib(wnd, GLFW_MAXIMIZED);
			sys->video->SizeChanged(width, height, maximized);
			});
		glfwSetWindowMaximizeCallback(wnd, [](GLFWwindow* wnd, int maximized) {
			auto sys = (sys_main_c*)glfwGetWindowUserPointer(wnd);
			int width{}, height{};
			glfwGetWindowSize(wnd, &width, &height);
			sys->video->SizeChanged(width, height, maximized == GLFW_TRUE);
			});
		glfwSetWindowPosCallback(wnd, [](GLFWwindow* wnd, int x, int y) {
			auto sys = (sys_main_c*)glfwGetWindowUserPointer(wnd);
			sys->video->PosChanged(x, y);
			});
		glfwSetCharCallback(wnd, [](GLFWwindow* wnd, uint32_t codepoint) {
			auto sys = (sys_main_c*)glfwGetWindowUserPointer(wnd);
			/* Always pass character events to POB (no ImGui guard here).
			 * This is essential for Windows IME: composed CJK characters
			 * arrive via WM_CHAR → glfwCharCallback and must reach Lua.
			 * ImGui debug windows don't need exclusive char capture. */
			sys->core->KeyEvent(codepoint, KE_CHAR);
			});
		glfwSetKeyCallback(wnd, [](GLFWwindow* wnd, int key, int scancode, int action, int mods) {
			auto sys = (sys_main_c*)glfwGetWindowUserPointer(wnd);
			if (ImGui::GetIO().WantCaptureKeyboard) {
				return;
			}
			if (byte k = sys->GlfwKeyToKey(key, scancode)) {
				bool is_down = action == GLFW_PRESS || action == GLFW_REPEAT;
				sys->heldKeyState[k] = is_down;
				sys->core->KeyEvent(k, is_down ? KE_KEYDOWN : KE_KEYUP);
				char ch = sys->GlfwKeyExtraChar(key);
				if (is_down && ch) {
					sys->core->KeyEvent(ch, KE_CHAR);
				}
			}
			});
		glfwSetMouseButtonCallback(wnd, [](GLFWwindow* wnd, int button, int action, int mods) {
			auto sys = (sys_main_c*)glfwGetWindowUserPointer(wnd);
			if (ImGui::GetIO().WantCaptureMouse) {
				return;
			}
			auto video = (sys_video_c*)sys->video;
			int sg_key;
			switch (button) {
			case GLFW_MOUSE_BUTTON_LEFT:
				sg_key = KEY_LMOUSE;
				break;
			case GLFW_MOUSE_BUTTON_MIDDLE:
				sg_key = KEY_MMOUSE;
				break;
			case GLFW_MOUSE_BUTTON_RIGHT:
				sg_key = KEY_RMOUSE;
				break;
			case GLFW_MOUSE_BUTTON_4:
				sg_key = KEY_MOUSE4;
				break;
			case GLFW_MOUSE_BUTTON_5:
				sg_key = KEY_MOUSE5;
				break;
			default:
				return;
			}
			bool is_down = action == GLFW_PRESS;
			sys->heldKeyState[sg_key] = is_down;
			keyEvent_s sg_type = is_down ? KE_KEYDOWN : KE_KEYUP;

			// Determine if a click is a doubleclick by checking for recent nearby click of same type.
			auto& lastClick = video->lastClick;
			if (is_down && video->lastCursorPos) {
				auto now = std::chrono::system_clock::now();

				// Was the last click with the same button?
				if (lastClick && lastClick->button == sg_key) {
					std::chrono::milliseconds const DOUBLECLICK_DELAY(500);
					int const DOUBLECLICK_RANGE = 5;

					// Was it recent and close enough?
					auto durationSinceClick = now - lastClick->time;
					auto prevPos = lastClick->pos;
					auto curPos = *video->lastCursorPos;
					if (durationSinceClick <= DOUBLECLICK_DELAY &&
						abs(prevPos.x - curPos.x) <= DOUBLECLICK_RANGE &&
						abs(prevPos.y - curPos.y) <= DOUBLECLICK_RANGE)
					{
						sg_type = KE_DBLCLK;
					}
				}
				if (sg_type == KE_DBLCLK) {
					lastClick.reset();
				}
				else {
					ClickEvent e;
					e.time = now;
					e.pos = *video->lastCursorPos;
					e.button = sg_key;
					lastClick = e;
				}
			}
			sys->core->KeyEvent(sg_key, sg_type);
			});
		glfwSetScrollCallback(wnd, [](GLFWwindow* wnd, double xoffset, double yoffset) {
			auto sys = (sys_main_c*)glfwGetWindowUserPointer(wnd);
			if (ImGui::GetIO().WantCaptureMouse) {
				return;
			}
			if (yoffset > 0) {
				sys->core->KeyEvent(KEY_MWHEELUP, KE_KEYDOWN);
				sys->core->KeyEvent(KEY_MWHEELUP, KE_KEYUP);
			}
			else if (yoffset < 0) {
				sys->core->KeyEvent(KEY_MWHEELDOWN, KE_KEYDOWN);
				sys->core->KeyEvent(KEY_MWHEELDOWN, KE_KEYUP);
			}
			});
		glfwSetWindowContentScaleCallback(wnd, [](GLFWwindow* wnd, float xScale, float yScale) {
			auto sys = (sys_main_c*)glfwGetWindowUserPointer(wnd);
			auto video = (sys_video_c*)sys->video;
			if (!video->ignoreDpiScale) {
				video->vid.dpiScale = xScale;
			}
			});

		// Adjust window look and position
		{
			sys_programIcons_c icons;
			if (icons.Size() > 0) {
				glfwSetWindowIcon(wnd, (int)icons.Size(), icons.Data());
			}
		}
		glfwSetWindowSizeLimits(wnd, cur.minSize[0], cur.minSize[1], GLFW_DONT_CARE, GLFW_DONT_CARE);
		glfwSetWindowPos(wnd, vid.pos[0], vid.pos[1]);
		if (!!(cur.flags & VID_MAXIMIZE)) {
			glfwMaximizeWindow(wnd);
		}
		glfwShowWindow(wnd);

		// Clear early to avoid flash
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		glfwSwapBuffers(wnd);
	}

	glfwGetFramebufferSize(wnd, &vid.fbSize[0], &vid.fbSize[1]);
	glfwGetWindowSize(wnd, &vid.size[0], &vid.size[1]);
	if (!ignoreDpiScale) {
		glfwGetWindowContentScale(wnd, &sys->video->vid.dpiScale, nullptr);
	}

	initialised = true;
	return 0;
}

void sys_video_c::SetForeground()
{
	if (initialised) {
		glfwFocusWindow(wnd);
	}
}

bool sys_video_c::IsActive()
{
	return glfwGetWindowAttrib(wnd, GLFW_FOCUSED);
}

void sys_video_c::FramebufferSizeChanged(int width, int height)
{
	// Avoid persisting an invalid window size from being minimized.
	if (!glfwGetWindowAttrib(wnd, GLFW_ICONIFIED)) {
		vid.fbSize[0] = width;
		vid.fbSize[1] = height;
	}
}

void sys_video_c::SizeChanged(int width, int height, bool max)
{
	// Avoid persisting an invalid window size from being minimized.
	if (!glfwGetWindowAttrib(wnd, GLFW_ICONIFIED)) {
		vid.size[0] = width;
		vid.size[1] = height;
		vid.maximised = max;
	}
}

void sys_video_c::PosChanged(int x, int y)
{
	// Avoid persisting an invalid window location from being minimized.
	if (!glfwGetWindowAttrib(wnd, GLFW_ICONIFIED)) {
		vid.pos[0] = x;
		vid.pos[1] = y;
	}
}

void sys_video_c::GetMinSize(int& width, int& height)
{
	width = minSize[0];
	height = minSize[1];
}

void sys_video_c::SetVisible(bool vis)
{
	if (!initialised) return;
	if (vis) {
		glfwShowWindow(wnd);
	}
	else {
		glfwHideWindow(wnd);
	}
}

bool sys_video_c::IsVisible()
{
	if (!initialised || !wnd) return false;
	return !!glfwGetWindowAttrib(wnd, GLFW_VISIBLE);
}

void sys_video_c::SetTitle(const char* title)
{
	strcpy(curTitle, (title && *title) ? title : CFG_TITLE);
	if (initialised) {
		glfwSetWindowTitle(wnd, curTitle);
	}
}

void* sys_video_c::GetWindowHandle()
{
	return wnd;
}

void sys_video_c::GetRelativeCursor(int& x, int& y)
{
	if (!initialised) return;
	double xpos, ypos;
	glfwGetCursorPos(wnd, &xpos, &ypos);
	x = (int)floor(xpos);
	y = (int)floor(ypos);
}

void sys_video_c::SetRelativeCursor(int x, int y)
{
	if (!initialised) return;
	glfwSetCursorPos(wnd, (double)x, (double)y);
}

bool sys_video_c::IsCursorOverWindow()
{
	if (initialised && wnd && IsVisible())
	{
		return cursorInWindow;
	}
	return true;
}

void sys_video_c::RefreshMonitorInfo()
{
	GLFWmonitor** monitors = glfwGetMonitors(&numMon);
	for (int m = 0; m < numMon; m++) {
		mon[m].hnd = monitors[m];
		glfwGetMonitorPos(monitors[m], &mon[m].left, &mon[m].top);
		GLFWvidmode const* mode = glfwGetVideoMode(monitors[m]);
		mon[m].width = mode->width;
		mon[m].height = mode->height;
	}
}
