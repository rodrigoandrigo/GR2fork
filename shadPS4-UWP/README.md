# ShadPS4-UWP

ShadPS4-UWP adapts the shadPS4 PlayStation 4 emulator to the Universal Windows Platform (UWP), with an interface for Windows and Xbox in Developer Mode.

The application combines a C++/XAML and ImGui frontend with the emulation core compiled as `shadPS4.dll`. The interface lets you manage your game library, import system modules, and control execution using a controller or keyboard. The project integrates the UWP lifecycle and window resizing with game execution.

Rendering supports Vulkan through Mesa Dozen, selected by default, and a native D3D12 backend. Dozen translates Vulkan to Direct3D 12; the graphics libraries are built and included in the package by the build workflow.

## Interface

The interface adapts the appearance of the shadPS4 Qt Launcher to the UWP frontend: top menus, a toolbar with the original icons, a list or grid library, search, adjustable cover sizes, and a resizable log panel at the bottom. The dark palette and system font follow the launcher's style.

Settings are available in a single tabbed window with global or per-game profiles. The controller and keyboard shortcuts open this same window. Options come from the core API, and each option appears in a single category; audio, folders, and graphics backend selection have dedicated sections. Use **Save** or **Apply** to save changes, or **Close** to discard them.

The display mode, cover size, and log panel layout persist between launches. The application supports keyboard and controller navigation, as well as UWP folder pickers.

On Xbox, the library and menus request the system pointer. It is hidden when a game starts and restored when returning to the interface, ending the session, or canceling a failed launch.

## PKG games

Use **File > Add PKG...** to select an individual package, or add a folder containing `.pkg` files. Access authorization is saved in the application library.

PS4 packages in FPKG format are accessed directly, with on-demand reading, decryption, and decompression, following the Prosperity implementation. The game is mounted in the virtual filesystem as `/app0`, without extracting the entire package to application storage.

The **Format** column distinguishes **PKG** from **Folder**; the grid also displays this information. The title, serial, version, and cover are read from the package. Updates and DLCs are identified separately and are not launched as standalone games.

## Touchpad on the Xbox controller

Hold **View** and move the **left stick** to position your finger on the PS4 touchpad. Contact begins when the stick moves beyond the initial dead zone or is pressed, and continues while View is held, even when the stick returns to the center.

Use **View + L3** to click the touchpad. Releasing only L3 releases the click while keeping the finger in contact; releasing View lifts the finger and restores the stick's normal game function. Each player has an independent gesture and contact identifier.

The menu, loss of focus, and controller disconnection release contact. Afterward, release View before starting another gesture. The same instructions are available in the **Input** tab, without a separate configuration.

## Build requirements

- Windows x64 and PowerShell.
- Visual Studio with the C++ `v145` toolset (Visual Studio 2026), UWP C++ tools, MSBuild, Clang/LLVM, and CMake/Ninja integration.
- Windows SDK **10.0.26100.0**.
- CMake **3.24 or later**, available in `PATH`.
- Git, to fetch the repository submodules.
- MSYS2 in `C:\msys64`, with the UCRT64 environment, Python 3, Bison, and Flex. The Mesa script uses `C:\msys64\usr\bin` and `C:\msys64\ucrt64\bin`.
- In the Python environment used by the build: Meson **1.4 or later**, Mako, PyYAML, and packaging.
- The **Microsoft.VCLibs.140.00 x64** UWP runtime installed on Windows.
- The **SDL3-uwp** source tree, including `VisualC-WinRT\SDL-UWP.vcxproj`, in a separate folder.

The Mesa and D3D12 backend sources are located in `externals/mesa-uwp` and `externals/d3d12-native` in this repository.

## Preparation

From the repository root, initialize the submodules:

```powershell
git submodule update --init --recursive
```

In the MSYS2 UCRT64 terminal, install the build-generation tools:

```bash
pacman -S --needed bison flex mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-python-mako mingw-w64-ucrt-x86_64-python-yaml mingw-w64-ucrt-x86_64-python-packaging
```

Have the SDL3-uwp source tree available and adjust the `-SDL3Source` argument below to its location.

## Full build

Run the following in PowerShell from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-uwp.ps1 `
    -Configuration Release `
    -SDL3Source "C:\dev\SDL3-uwp" `
    -PythonExecutable "C:\msys64\ucrt64\bin\python.exe" `
    -Jobs 6
```

The script configures the Visual Studio x64 environment and performs these steps:

1. Builds SDL3 for UWP.
2. Builds Mesa Dozen and the `spirv_to_dxil` compiler.
3. Configures and builds the `shadPS4.dll` core using CMake, Ninja, and clang-cl.
4. Builds the UWP frontend and generates the MSIX Bundle package.
5. Signs the package, verifies its CMS signature, and exports the public certificate.

### Options

| Parameter | Purpose |
| --- | --- |
| `-Configuration Release` | Selects `Release` or `Debug`; defaults to `Release`. |
| `-SDL3Source <folder>` | Location of the SDL3-uwp source tree. |
| `-D3D12Source <folder>` | Overrides the default `externals/d3d12-native` folder. |
| `-PythonExecutable <file>` | Specifies the Python executable with the Mesa dependencies. |
| `-Jobs 6` | Sets parallelism for the core and Mesa builds. |
| `-CoreOnly` | Builds SDL3, Mesa, and the core without generating the frontend or package. |
| `-SkipSigning` | Generates the application and package without performing the final signing step. |
| `-CertificatePath <file.pfx>` | Uses a custom certificate for signing. |

To generate a debug build, use the same command with `-Configuration Debug`.

## Signing

By default, the script uses `shadPS4-UWP/shadPS4-UWP_TemporaryKey.pfx`. A custom certificate must contain the private key, be within its validity period, be loadable with an empty password, and have a subject matching the manifest's `Publisher`.

To sign an existing package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\sign-uwp.ps1 `
    -Package "C:\build\shadPS4-UWP_1.0.0.0_x64.msixbundle" `
    -CertificatePath "C:\certificados\shadPS4-UWP.pfx"
```

The script uses SignTool from SDK 10.0.26100.0, validates the signature, and writes `shadPS4-UWP.cer` next to the package. Successful completion reports `CMS_SIGNATURE=PASS`.

## Generated files

The paths below are relative to the repository root:

| Path | Contents |
| --- | --- |
| `Build/UWP-Release/` | CMake/Ninja core build and SDL3 build. |
| `Build/Dozen-UWP/` | Mesa build, including `vulkan_dzn.dll` and `spirv_to_dxil.dll`. |
| `shadPS4-UWP/Runtime/Release/` | Core and runtime libraries used by the frontend. |
| `shadPS4-UWP/AppPackages/` | `.msixbundle` packages and the public certificate exported during signing. |

For debug builds, the core and runtime directories use `Debug` instead of `Release`.

## Project layout

- `shadPS4-UWP/`: frontend, manifest, assets, and UWP project.
- `src/`: emulation core and graphics backends.
- `externals/mesa-uwp/`: Mesa Dozen implementation for UWP.
- `externals/d3d12-native/`: native D3D12 backend integration.
- [scripts/build-uwp.ps1](../scripts/build-uwp.ps1): full build.
- [scripts/build-dozen.ps1](../scripts/build-dozen.ps1): Mesa library build.
- [scripts/sign-uwp.ps1](../scripts/sign-uwp.ps1): package signing and verification.

## Cheats, patches, and in-game information

Select a game and open **Cheats / Patches > Manage selected game...**. The tabs let you download files from the shadPS4 and GoldHEN repositories, review versions, credits, and notes, delete files, and select patches. Selections are saved automatically and applied on the next launch. Updating the files preserves previously selected patches. During execution, open the emulator menu and use the cheat checkboxes or buttons for the running game; clearing a checkbox applies the file's disable bytes.

**Settings > Show FPS counter** controls the counter in the top-left corner of the image. It measures actual game flips, updating every half second, without keeping the interface swapchain presenting frames during gameplay.

**Memory swap** is enabled by default on Xbox and disabled by default on PC. The global `General.memory_swap_enabled` setting preserves an explicit choice. The backing uses on-demand 16 KiB private pages, promoted to shared sections when aliases are created. The pager moves cold pages to compressed records in temporary files under `cache/swap`, with compressed RAM fallback, second-chance eviction, and leases for host/CPU/GPU accesses. The policy and integration are described in [UWP-MEMORY.md](../documents/UWP-MEMORY.md).

Vulkan/Dozen graphics pipelines are compiled in the background. **Synchronous pipeline budget (ms)** controls how long a draw waits for its pipeline before being skipped; the default is 50 ms, and a value of 0 avoids this wait. Photo captures use a minimum wait of 700 ms. This option can be adjusted globally or per game. **Readback on copy queue** is enabled by default, and older settings are migrated when this version is first opened.
