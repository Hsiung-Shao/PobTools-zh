// DyLua: SimpleGraphic
// (c) David Gowor, 2014
//
// UI Main Header
//

// =======
// Classes
// =======

struct ui_expectationFailed_s {};

// UI Manager
class ui_main_c: public ui_IMain {
public:
	// Interface
	void	Init(int argc, char** argv);
	void	Frame();
	void	Shutdown();
	void	KeyEvent(int key, int type);
	bool	CanExit();

	// Encapsulated
	ui_main_c(sys_IMain* sysHnd, core_IMain* coreHnd);

	sys_IMain* sys = nullptr;
	core_IMain* core = nullptr;

	r_IRenderer* renderer = nullptr;

	ui_IConsole* conUI = nullptr;
	ui_IDebug* debug = nullptr;

	dword	subScriptSize = 0;
	ui_ISubScript** subScriptList = nullptr;

	std::optional<sol::state> solState;
	lua_State* L = nullptr;
	std::filesystem::path scriptName;
	std::filesystem::path scriptCfg;
	std::filesystem::path scriptPath;
	std::filesystem::path scriptWorkDir;
	int		scriptArgc = 0;
	char**	scriptArgv = nullptr;
	bool	restartFlag = false;
	bool	didExit = false;
	bool	renderEnable = false;
	// POB_ZH_HEADLESS=1: run POB's Lua with no window, no GL, Draw* as no-ops,
	// and talk to the host over stdio (engine/headless_ipc.h). Read once in Init.
	bool	headless = false;
	// Registry ref of the Lua dispatcher the bridge registered with
	// PobToolsBridgeSetDispatcher(fn); LUA_NOREF until the bridge loaded.
	int		bridgeDispatchRef = -2 /* LUA_NOREF */;
	int		cursorX = 0;
	int		cursorY = 0;
	int		framesSinceWindowHidden = 0;
	volatile bool	inLua = false;
	bool	hasActiveCoroutine = false;
	int		ioOpenf = LUA_NOREF;

	float lastColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};

	static int InitAPI(lua_State* L);

	void	RenderInit(r_featureFlag_e features);
	void	ScriptInit();
	void	ScriptShutdown();
	// Headless only: load Data\bridge\bridge.lua (path from POB_ZH_BRIDGE) after
	// the inject script, and hand queued host requests to its dispatcher.
	void	HeadlessLoadBridge();
	void	HeadlessDispatch();

	void	LAssert(lua_State* L, int cond, const char* fmt, ...); // Non-local return to Lua code on failure
	void	LExpect(lua_State* L, int cond, const char* fmt, ...); // Throws ui_expectationFailed_s on failure, message on Lua stack
	int		IsUserData(lua_State* L, int index, const char* metaName);
	int		PushCallback(const char* name);
	void	PCall(int narg, int nret);
	void	DoError(const char* msg, const char* error);

	void	CallKeyHandler(const char* hname, int key, bool dblclk);
	const char* NameForKey(int key);
	int		KeyForName(const char* name);

	enum { REGISTRY_KEY = 1 };
};
