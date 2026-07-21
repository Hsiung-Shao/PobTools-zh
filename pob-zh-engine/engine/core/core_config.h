// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Core Config Header
//

// ==========
// Interfaces
// ==========

// Core Config
class core_IConfig {
public:
	static core_IConfig* GetHandle(sys_IMain* sysHnd);
	static void FreeHandle(core_IConfig* hnd);
	
	virtual bool	LoadConfig(std::filesystem::path const& cfgName) = 0;
	virtual bool	SaveConfig(std::filesystem::path const& cfgName) = 0;
};
