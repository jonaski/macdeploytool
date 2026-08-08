# macdeploytool

`macdeploytool` is a macOS deployment tool for Qt 6 applications, based on and forked from Qt's own `macdeployqt`.

It takes an application bundle (`.app`) as input and makes it self-contained by copying in the Qt frameworks, plugins, and other shared libraries that the application depends on, fixing up their install names and rpaths, and (optionally) code signing and packaging the result as a `.dmg` disk image.

Compared to upstream `macdeployqt`, it additionally supports deploying GIO modules and GStreamer plugins, which are commonly needed by applications built with GLib/GStreamer-based multimedia stacks.

## What it does

1. **Deploys Qt frameworks** — inspects the app binary (and any additional executables/libraries) with `otool`, resolves their Qt framework and dylib dependencies (including transitive ones), copies them into `Contents/Frameworks`, and rewrites install names and rpaths with `install_name_tool` so the bundle no longer depends on anything outside itself.
2. **Deploys Qt plugins** — copies the platform, style, image format, network, SQL, WebView, and other plugins that are actually needed, based on which Qt modules ended up being deployed. Plugins that use private API (and would make the bundle App Store non-compliant) can be skipped with `--appstore-compliant`.
3. **Deploys GIO modules** — if the app depends on `libgio-2.0`, its modules directory (GSettings backends, TLS backend, proxy resolvers, volume monitors) is located and bundled automatically.
4. **Deploys GStreamer plugins** — if the app depends on `libgstreamer-1.0`, you can opt in to bundling either all installed GStreamer plugins or a specific, named subset.
5. **Strips debug symbols** from the deployed binaries (unless disabled).
6. **Code signs** the bundle, inside-out (dependencies before dependents), optionally with Hardened Runtime and a secure timestamp for notarization.
7. **Creates a `.dmg` disk image** of the finished bundle, if requested.
8. **Verifies** the final bundle by checking that every Mach-O file's remaining linked libraries actually resolve to something inside the bundle (or the system).

## Requirements

- macOS
- Qt 6.4 or later (Core module)
- CMake 3.19 or later
- A C++17 compiler (Xcode command line tools)

At runtime, `macdeploytool` also expects the following command-line utilities to be available in `PATH` (it checks for these on startup and exits early with an error if any required one is missing):

| Tool | When it's required |
|---|---|
| `otool` | Always |
| `install_name_tool` | Always |
| `strip` | Unless `--no-strip` (or `--use-debug-libs`, which implies it) is used |
| `codesign` | Only when code signing is requested (`--codesign` or `--sign-for-notarization`) |
| `hdiutil` | Only when `--dmg` is used |

`pkg-config` and `gst-inspect-1.0` are used opportunistically (for locating GIO/GStreamer install paths and resolving named GStreamer plugins) but are not hard requirements — deployment falls back gracefully, with a warning, if they're unavailable.

## Building

```sh
cmake -S . -B build
cmake --build build -j
```

The resulting `macdeploytool` binary is written to the build directory. Optionally install it with:

```sh
cmake --install build
```

## Usage

```sh
macdeploytool [options] app-bundle
```

### Options

| Option | Description |
|---|---|
| `--verbose <level>` | `0` = no output, `1` = error/warning (default), `2` = normal, `3` = debug. |
| `--no-plugins` | Skip plugin deployment. |
| `--dmg` | Create a `.dmg` disk image. |
| `--no-strip` | Don't run `strip` on the binaries. |
| `--use-debug-libs` | Deploy with debug versions of frameworks and plugins (implies `--no-strip`). |
| `--executable <path>` | Let the given executable use the deployed frameworks too. Can be repeated. |
| `--always-overwrite` | Copy files even if the target file already exists. |
| `--codesign <identity>` | Run `codesign` with the given identity on all executables (use `-` for ad-hoc signing). |
| `--no-codesign` | Disable code signing. Cannot be combined with `--codesign` or `--sign-for-notarization`. |
| `--hardened-runtime` | Enable Hardened Runtime when code signing. |
| `--timestamp` | Include a secure timestamp when code signing (requires an internet connection). |
| `--sign-for-notarization <identity>` | Shortcut that enables code signing, Hardened Runtime, and a secure timestamp — everything required for notarization. |
| `--appstore-compliant` | Skip deployment of components that use private API and would be rejected by the Mac App Store (currently the `qsqlodbc` and `qsqlpsql` SQL plugins, and the QtWebEngine-based WebView plugin). |
| `--libpath <path>` | Add the given path to the library search path used when resolving non-absolute library references. Can be repeated. |
| `--fs <filesystem>` | Filesystem to use for the `.dmg` disk image (default: `HFS+`). |
| `--gstreamer-plugins <set>` | Which GStreamer plugins to bundle if the app depends on `libgstreamer-1.0`: `none` (default), `all`, or `list`. |
| `--gstreamer-plugin-list <names>` | Comma-separated GStreamer plugin names to bundle exactly (e.g. `coreelements,playback,typefindfunctions,audioconvert`). Only used when `--gstreamer-plugins=list`. |
| `-h`, `--help` | Display help on command-line options. |
| `--help-all` | Display help, including generic Qt options. |

### Examples

Deploy Qt frameworks and plugins into an app bundle:

```sh
macdeploytool MyApp.app
```

Deploy, ad-hoc sign, and package as a `.dmg`:

```sh
macdeploytool MyApp.app --codesign - --dmg
```

Sign for notarization with a Developer ID identity, bundling all GStreamer plugins:

```sh
macdeploytool MyApp.app --sign-for-notarization "Developer ID Application: My Company (TEAMID)" --gstreamer-plugins all --dmg
```

Deploy only a specific set of GStreamer plugins, skipping App Store-incompatible components:

```sh
macdeploytool MyApp.app --gstreamer-plugins list --gstreamer-plugin-list coreelements,playback,typefindfunctions,audioconvert --appstore-compliant
```

## License

This project is a fork of Qt's `macdeployqt` and inherits its licensing — see the SPDX header in each source file: `LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0`.
