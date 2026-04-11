# @maciejwojs/input-bridge

Native Node.js addon for simulating hardware input events (mouse movement, clicks, keyboard events). Packages are published to NPM with provenance (OIDC) and include prebuilt binaries via GitHub Actions.

## Current state

- Windows backend is implemented using the `SendInput` API, supporting batched event execution for high performance.
- Linux backend is implemented using the Freedesktop `RemoteDesktop` portal on Wayland, requesting authorization via D-Bus before injecting mouse and keyboard events.
- Optional `libxkbcommon` support is loaded at runtime to map Unicode characters for Linux text input.
- Features a cross-platform `InputQueue` that buffers input events.
- Includes a movement decimation algorithm (`optimizeMouseMovesRelative` / `optimizeMouseMovesAbsolute`) to filter out redundant micro-movements, vastly reducing native API call overhead.
- Runtime loading uses `node-gyp-build`, so local build and `prebuilds/` binaries are both supported.

## Install

```bash
bun install
```

## Local build

```bash
bun run build # to compile TypeScript
bun x node-gyp rebuild
```

## Prebuilt binaries (prebuildify)

Build prebuild for current platform:

```bash
bun run prebuildify
```

Build prebuilds for selected platforms:

```bash
bun run prebuildify:all
```

Output goes to `prebuilds/` and is loaded automatically by `lib/index.ts`.
Scripts are configured to run `node-gyp` via `node-gyp` internally and build TypeScript beforehand.

## How to add other systems / window managers

Recommended backend mapping:

- Windows: `SendInput` (already in place)
- Linux: `RemoteDesktop` portal on Wayland (currently implemented), or `uinput` / `XTest` (X11) / `ydotool` (Wayland) for alternate environments
- macOS: `CGEventCreateMouseEvent` and `CoreGraphics` APIs

Practical architecture:

- Keep one JS API (`InputBridge`) for all OSes.
- Keep one addon target in `binding.gyp`.
- Keep per-platform backend in separate `.cpp` files and select in `binding.gyp` conditions.
- Current split is:
	- `src/addon.cpp` - shared N-API wrapper
	- `src/win/platform_input_win.cpp` - Windows backend
	- `src/linux/platform_input_linux.cpp` - Linux backend entry point
	- `src/platform_input_stub.cpp` - fallback for other systems
	- `src/platform_input.hpp` - common backend interface and `InputQueue` implementation
- Unsupported combinations default to a stub fallback implementing the interface.

To add the next platform, create the backend file (for example `src/macos/platform_input_macos.cpp`), implement the `IPlatformInput` virtual methods (the `InputQueue` and batching mechanics will be handled automatically by the base classes), and add the file to the matching `binding.gyp` condition branch.

This gives you one npm package with many prebuilt binaries and no user-side compile step.
