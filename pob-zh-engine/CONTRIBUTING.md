## Instructions to build a DLL

The directory names and locations chosen for your environment are flexible, below we use the following:

* source directory - `PoB-SimpleGraphic`
* build directory - `build-SimpleGraphic`
* Path of Building dev tree - `PoB`
* install directory - `PoB\runtime`

Try to avoid paths with spaces, some of our dependencies are brittle and won't build properly then.

1. Clone the repository with `git clone --recursive` to obtain the sources and the submodules for dependencies, like:
    ```powershell
    git clone --recursive "https://github.com/PathOfBuildingCommunity/PathOfBuilding-SimpleGraphic" "PoB-SimpleGraphic"
    ```

2. Configure a build directory for the sources with CMake generating Visual Studio build files:
    ```powershell
    cmake -B "build-SimpleGraphic" -S "PoB-SimpleGraphic" -A x64 -G "Visual Studio 17 2022" --toolchain "vcpkg\scripts\buildsystems\vcpkg.cmake" -DCMAKE_INSTALL_PREFIX="PoB\runtime"
    ```

3. Open the generated `build-SimpleGraphic\SimpleGraphic.sln` file in Visual Studio.

4. There's three primary build configurations of interest for development largely aligning with the CMake configurations of the same name:

    * `Debug`: Low optimization, easy to step through logic and inspect values without it being all optimized out;
    * `RelWithDebInfo`: Medium optimization, still somewhat debuggable but also better performance;
    * `Release`:  Full optimization - modified from the stock CMake configuration to still generate debug symbols.

5. Build the solution to compile the dependencies and SimpleGraphic itself, the DLL and some of the dependencies can be found in the build directory in a directory named after the configuration.

6. Build the `INSTALL` target to install the SimpleGraphic DLL and the dependencies to the install directory, here `PoB\runtime`.


## Debugging SimpleGraphic

The debugger settings in Visual Studio can't quite be communicated through CMake scripts so we have to do some manual setup to point out which executable and what working directory and command line arguments we wish to use to debug the DLL.

1. In Visual Studio open the Project Properties for the SimpleGraphic project.

2. Go to the Debugging page and set the following fields, either for each configuration or once for "All Configurations":

    * "Debugger to launch" - `Local Windows Debugger`
    * "Command" - `$(ProjectDir)..\PoB\runtime\Path{space}of{space}Building.exe`
    * "Command Arguments" - leave this empty unless you wish to launch a specific Lua entrypoint or test something like URLs on the command line
    * "Working Directory" - `$(ProjectDir)..\PoB\runtime`

3. This debugger configuration will use the DLL file that is in the PoB install directory, so remember to build the `INSTALL` target before debugging to deploy the DLL and its dependencies.

4. Set breakpoints and launch new debug sessions from inside Visual Studio or attach to an existing process, as long as the sources match the debug information in the DLL it'll all work fine.


## Development tricks

A PowerShell script like the following can be run with Administrator rights to link a DLL file in the build directory into a PoB runtime tree to avoid having to build the `INSTALL` target for every change:

```powershell
param ($Config="RelWithDebInfo")
$BuildTree = "..\..\build-SimpleGraphic"
$BuildDir = "${BuildTree}\$Config"
$RunDir = ".\PoB\runtime"
$VcpkgBinDir = "${BuildTree}\vcpkg_installed\x64-windows\bin"

$Suffix = ""
if ($Config -eq "Debug")
{
	$Suffix = "d"
}

New-Item -ItemType SymbolicLink -Force -Path "${RunDir}\SimpleGraphic.dll" -Target "${BuildDir}\SimpleGraphic.dll"
```

Don't forget to re-run the script if you change the configuration so that the correct DLL is used by the application.
