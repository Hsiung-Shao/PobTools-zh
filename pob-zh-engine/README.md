# Path of Building Community SimpleGraphic.dll

## Introduction

`SimpleGraphic.dll` is the host environment for Lua.
It contains the API used by the application's Lua logic, as well as a
2D OpenGL ES 2.0 renderer, window management, input handling, and a
debug console.
It exports one symbol, `RunLuaFileAsWin`, which is passed a
C-style argc/argv argument list, with the script path as `argv[0]`.

The Windows-specific code is contained in 5 files:
- `win\entry.cpp`: Contains the DLL export
It just creates the system main module, and runs it
- `engine\system\win\sys_main.cpp`: The system main module.
It initialises the application, and contains generic OS interface functions,
such as input and clipboard handling
- `engine\system\win\sys_console.cpp`: Manages the debug console window that
appears during the program's initialisation
- `engine\system\win\sys_video.cpp`: Creates and manages the main program window
- `engine\system\win\sys_opengl.cpp`: Initialises OpenGL

## Building

`SimpleGraphic.dll` is currently built using Visual Studio 2022 for 64-bit.

The DLL depends on a number of 3rd-party libraries, all provided either as
direct submodules and built by the main `CMakeLists.txt` file or built from
ports in the `vcpkg` submodule as part of the build process.

The build process will also build the `lcurl` and `lzip` Lua extensions
against the same LuaJIT version as the DLL is built with.

A short guide on building and debugging the DLL is available in
[CONTRIBUTING.md](CONTRIBUTING.md).

The `INSTALL` target will deploy the DLL, its dependencies and the VC++
runtime to the installation directory.

## Debugging

Since SimpleGraphic.dll is dynamically loaded by `PathOfBuilding.exe`,
to debug it, run `PathOfBuilding.exe` and then attach to that process using the
"Debug" > "Attach to Process..." menu option in Visual Studio.

Visual Studio can also be configured to start the Path of Building executable
when debugging a target which troubleshooting of early startup.

## Project dependencies

Runtime and utilities:
* [LuaJIT](https://github.com/LuaJIT/LuaJIT) - fast Lua fork with JIT compilation that has diverged from upstream Lua at version 5.1
* [curl](https://curl.se/) - very common HTTP library, exposed to Lua
* [fmtlib](https://fmt.dev/) - modern string formatting
* [libsodium](https://doc.libsodium.org/) - friendly cryptographic primitives, used in SimpleGraphic for fast hashing
* [pkgconf](http://pkgconf.org/) - part of the build process to locate builds of bundled libraries
* [re2](https://github.com/google/re2) - regex library

Graphics:
* [GLFW](https://www.glfw.org/) - multi-platform windowing library for OpenGL (and other APIs)
* [ANGLE](https://github.com/google/angle) - OpenGL ES runtime from Google built on top of native rendering APIs
* [Glad 2](https://gen.glad.sh/) - OpenGL header generator

Compression and image formats:
* [stb](https://github.com/nothings/stb) - single-header libraries for many things, here image reading and writing
* [giflib](https://sourceforge.net/projects/giflib/) - GIF loading/saving
* [libjpeg-turbo](https://libjpeg-turbo.org/) - JPEG loading/saving
* [libpng](http://www.libpng.org/pub/png/libpng.html) - PNG loading/saving
* [liblzma](https://tukaani.org/xz/) - LZMA compression/decompression
* [zlib](https://www.zlib.net/) - zlib compression/decompression

## Licence

[MIT](https://opensource.org/licenses/MIT)

For 3rd-party licences, see [LICENSE](LICENSE).
The licencing information is considered to be part of the documentation.
