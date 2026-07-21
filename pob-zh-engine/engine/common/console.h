// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Console Header
//

#include <optional>
#include <string>

// =======
// Classes
// =======

// Console buffer scroll modes
enum conBufScroll_e {
	CBSC_DOWN,	// Scroll down
	CBSC_UP,	// Scroll up
	CBSC_BOTTOM	// Scroll to bottom
};

// Console print hook base
class conPrintHook_c {
public:
	virtual void ConPrintHook(const char* text) = 0;
	virtual void ConPrintClear() { }
protected:
	conPrintHook_c(class IConsole* conHnd);
	~conPrintHook_c();
	void	InstallPrintHook();
	void	RemovePrintHook();
private:
	class console_c* _con;
};

// Cvar flags
enum conVarFlags_e {
	CV_ARCHIVE	= 0x01,	// Save the value of this cvar
	CV_READONLY	= 0x02,	// Read-only cvar
	CV_CLAMP	= 0x04,	// Clamp the cvar value
	CV_SET		= 0x08,	// Cvar is set but not configured
};

// Cvar
class conVar_c {
public:
	std::string	name;			// Cvar name
	int		flags = 0;			// Flags
	bool	mod = false;		// Modified?

	int		intVal = 0;			// Integer value
	float	floatVal = 0.f;		// Float value
	std::string strVal;			// String value

	std::string	defVal;			// Default string
	int		min = 0, max = 0;	// Clamp limits

	conVar_c(IConsole* conHnd);
	~conVar_c();

	void	Set(int val);
	void	Set(float val);
	void	Set(char const* val);
	void	Toggle();
	bool	GetMod();	// Return and clear modified flag
	void	Reset();
	void	Clamp();
private:
	IConsole* con;
};

// Console input handler base
class conInputHandler_c {
protected:
	conInputHandler_c(class IConsole* conHnd);
	void	ClearConInput();
	void	RefreshConInput();
	void	ConInputKeyEvent(int key, int type);
	virtual void SetConInput(char* text, int caret) = 0;
private:
	class console_c* _con;
};

// Console command handler base
class conCmdHandler_c;
typedef void (conCmdHandler_c::*conCmdMethod_t)(class IConsole*,args_c&);
class conCmdHandlerA_c {};
class conCmdHandlerB_c {};
class conCmdHandler_c: public conCmdHandlerA_c, public conCmdHandlerB_c {
protected:
	conCmdHandler_c(class IConsole* conHnd);
	~conCmdHandler_c();
	template <class T>
	void	Cmd_Add(const char* name, int minArgs, const char* usage, T* objPtr, void (T::*method)(class IConsole*,args_c&))
	{
		Cmd_PrivAdd(name, minArgs, usage, objPtr, (conCmdMethod_t)method);
	}
private:
	class console_c* _con;
	void	Cmd_PrivAdd(const char* name, int minArgs, const char* usage, conCmdHandler_c* obj, conCmdMethod_t method);
};

// Command class
struct conCmd_c {
	std::string	name;	// Command name
	int			minArgs;// Required number of arguments
	std::string	usage;	// Usage string
	conCmdHandler_c* obj;  // Handler
	conCmdMethod_t method;	// Method
};

// ==========
// Interfaces
// ==========

// Console
class IConsole {
public:
	static IConsole* GetHandle();
	static void FreeHandle(IConsole* hnd);

	virtual	void	Print(const char* text) = 0;		// Print
	virtual	void	Printf(const char* fmt, ...) = 0;	// Formatted print
	virtual void	PrintFunc(const char* func) = 0;	// Function title print
	virtual void	Warning(const char* fmt, ...) = 0;	// Formatted warning print
	virtual	void	Clear() = 0;						// Clear the console
	virtual	void	Scroll(int mode) = 0;				// Scroll the console
	virtual	const char*	EnumLines(int* index) = 0;		// Retrieve lines of console text
	virtual char*	BuildBuffer() = 0;					// Retrieve entire console buffer text

	virtual void	Execute(const char* cmd) = 0;		// Execute string
	virtual void	Executef(const char* fmt, ...) = 0;	// Execute formatted string
	virtual void	ExecCommands(bool deferUnknown = false) = 0; // Flush command buffer

	virtual conVar_c* Cvar_Add(std::string_view name, int flags, std::string_view def, int minVal = 0, int maxVal = 0) = 0; // Add a variable
	virtual conVar_c* Cvar_Ptr(std::string_view name) = 0;	// Get pointer to an existing variable

	virtual conCmd_c* EnumCmd(int* index) = 0;			// Retrieve commands
	virtual conVar_c* EnumCvar(int* index) = 0;			// Retrieve variables
};