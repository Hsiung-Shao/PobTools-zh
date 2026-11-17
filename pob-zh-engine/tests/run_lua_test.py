"""Run a Lua regression with the packaged lua51.dll (no installed Lua needed)."""
import ctypes
import os
import sys

dll_path, script = map(os.path.abspath, sys.argv[1:3])
dll_directory = os.add_dll_directory(os.path.dirname(dll_path))
os.environ["POBTOOLS_TEST_ENGINE"] = os.path.dirname(dll_path)
lua = ctypes.CDLL(dll_path)
lua.luaL_newstate.restype = ctypes.c_void_p
lua.luaL_openlibs.argtypes = [ctypes.c_void_p]
lua.luaL_loadfile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lua.lua_pcall.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
lua.lua_tolstring.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
lua.lua_tolstring.restype = ctypes.c_char_p
lua.lua_close.argtypes = [ctypes.c_void_p]
state = lua.luaL_newstate()
if not state:
    raise RuntimeError("Lua state creation failed")
try:
    lua.luaL_openlibs(state)
    status = lua.luaL_loadfile(state, os.fsencode(script))
    if not status:
        status = lua.lua_pcall(state, 0, -1, 0)
    if status:
        print(lua.lua_tolstring(state, -1, None).decode("utf-8", errors="replace"))
        sys.exit(1)
finally:
    lua.lua_close(state)
    dll_directory.close()
