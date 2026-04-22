# @maciejwojs/input-bridge

Native Node.js addon for simulating hardware input events on Windows and Linux. The package exposes a single JavaScript API for mouse movement, clicks, keyboard events, scrolling, and typed text, with batched execution and optimization support.

## Features

- Windows backend using the native `SendInput` API
- Linux backend using the Freedesktop `RemoteDesktop` portal on Wayland
- Optional Linux X11 backend for environments where Wayland is unavailable
- Batched event queue with explicit `flush()` execution
- Relative and absolute mouse movement
- Mouse button clicks and wheel scrolling
- Raw keyboard events and typed Unicode text
- DOM `KeyboardEvent.code` mapping via `keyPressDOM()`
- Clipboard reading and writing text and files support for Wayland (`wl-copy`/`wl-paste`) and X11 (`xclip`)
- Movement optimization via `optimizeMouseMovesRelative()` and `optimizeMouseMovesAbsolute()`
- Optional native logger callback from the addon

## Install

```bash
bun install
```

## Quick start

```ts
import { InputBridge } from '@maciejwojs/input-bridge';

const bridge = new InputBridge({ autoFlush: false });
await bridge.init();

bridge.moveMouseRelative(10, 0);
bridge.mouseClick(0, true);
bridge.mouseClick(0, false);
bridge.flush();
```

## API overview

- `init()` - initializes the native bridge and requests permissions on Linux
- `moveMouseRelative(x, y)` - queue a relative mouse movement
- `moveMouseAbsolute(x, y)` - queue an absolute mouse movement
- `mouseClick(button, down)` - queue a mouse button press/release
- `keyPress(keyCode, down)` - queue a raw keyboard press/release
- `keyPressDOM(domCode, down)` - queue a scan-code based key event from DOM `KeyboardEvent.code`
- `scrollMouse(delta)` - queue a mouse wheel scroll event
- `typeString(text)` - queue typed Unicode text
- `setClipboardText(text)` - copy text to the clipboard
- `getClipboardText()` - read text from the clipboard
- `setClipboardFiles(paths)` - copy a list of files to the clipboard
- `getClipboardFiles()` - read a list of files from the clipboard
- `optimizeMouseMovesRelative(distanceThreshold)` - reduce buffered relative move events
- `optimizeMouseMovesAbsolute(distanceThreshold)` - reduce buffered absolute move events
- `toggleOptimization()` - enable/disable internal mouse move optimization
- `flush()` - execute all queued input events
- `setLogger(callback)` - receive native backend log messages

## Build and development

```bash
bun run build
bun run rebuild
```

To verify the native addon loads successfully:

```bash
bun run test:load
```

## Prebuilt binaries

Create a prebuilt binary for the current platform:

```bash
bun run prebuildify
```

Create prebuilt binaries for Windows, Linux, and macOS targets:

```bash
bun run prebuildify:all
```

Built artifacts are placed in `prebuilds/` and loaded automatically by `lib/index.ts`.

## Supported platforms

- `win32` - fully implemented with Windows `SendInput`
- `linux` - supported on Wayland via the RemoteDesktop portal and optionally on X11 when built with the X11 backend
- other OSes use the stub fallback if compiled, but native injection is only available on supported backends

## Project layout

- `lib/` - JavaScript API surface and wrapper exports
- `src/addon.cpp` - N-API bridge implementation
- `src/win/platform_input_win.cpp` - Windows backend
- `src/linux/platform_input_linux.cpp` - Linux backend
- `src/platform_input_stub.cpp` - fallback implementation
- `src/platform_input.hpp` - shared backend interface and event queue
- `binding.gyp` - native addon build configuration

## Notes

The repository is designed to keep the public JS API stable while allowing per-platform native backend extensions. The `InputBridge` wrapper always exposes the same methods regardless of the active backend.
