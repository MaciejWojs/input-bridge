import path from 'node:path';
import { fileURLToPath } from 'node:url';
import nodeGypBuild from 'node-gyp-build';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

export type ClipboardEventType = 'text' | 'files';

/**
 * Describes a monitor exposed by the native backend.
 */
export interface MonitorInfo {
    /** Stable session index used with `setCurrentMonitor()`. */
    index: number;
    /** Native monitor identifier or output name. */
    id: string;
    /** Human-readable monitor name. */
    name: string;
    /** Left edge in virtual desktop coordinates. */
    x: number;
    /** Top edge in virtual desktop coordinates. */
    y: number;
    /** Monitor width in pixels. */
    width: number;
    /** Monitor height in pixels. */
    height: number;
    /** Whether this is the primary/default monitor. */
    primary: boolean;
}

export interface ClipboardEvent {
    /**
     * The type of clipboard update.
     */
    type: ClipboardEventType;

    /**
     * Clipboard payload. For `text`, this is a string. For `files`, this is an array of file paths.
     */
    data: string | string[];
}

export type InputEventType =
    | 'mouse_move_relative'
    | 'mouse_move_absolute'
    | 'mouse_click'
    | 'key_press'
    | 'mouse_scroll'
    | 'type_character';

export interface InputEvent {
    type: InputEventType;
    x?: number;
    y?: number;
    button?: number;
    down?: boolean;
    keyCode?: number;
    charCode?: number;
    delta?: number;
    domCode?: string;
}

/**
 * Interface representing the native input bridge for simulating hardware events.
 * Actions are queued and must be dispatched using `flush()`.
 */
export interface IInputBridge {

    /**
     * Sets Linux portal input transport mode.
     * 
     * Supported values:
     * - `notify`: legacy `Notify*` D-Bus calls
     * - `eis`: request EIS transport via `ConnectToEIS`
     * 
     * @param mode - Transport mode (`notify` or `eis`).
     * @returns `true` on success.
     */
    setInputMode(mode: 'notify' | 'eis'): boolean;

    /**
     * Returns currently selected Linux portal input transport mode.
     */
    getInputMode(): string;

    /**
     * Attempts to connect the active RemoteDesktop session to EIS.
     * Must be called after successful `init()`.
     */
    connectToEIS(): boolean;

    /**
     * Closes active EIS connection and returns to notify mode.
     */
    disconnectEIS(): void;

    /**
     * Indicates whether EIS channel is currently connected.
     */
    isEISConnected(): boolean;

    /**
     * Sets the clipboard text (Unicode string).
     *
     * @param text - The text to set to the clipboard.
     * @returns `true` if successful, `false` otherwise.
     * @throws {TypeError} If `text` is not a string.
     *
     * @platform Windows: Supported. Linux: Not implemented.
     *
     * @example
     * ```typescript
     * bridge.setClipboardText("Hello!");
     * ``` 
     */
    setClipboardText(text: string): boolean;

    /**
     * Gets the current clipboard text (Unicode string).
     *
     * @returns The clipboard text, or `null` if unavailable or not text.
     *
     * @platform Windows: Supported. Linux: Not implemented.
     *
     * @example
     * ```typescript
     * const text = bridge.getClipboardText();
     * ```
     */
    getClipboardText(): string | null;

    /**
     * Sets the clipboard to contain a list of files (file paths).
     *
     * @param filePaths - Array of absolute file paths to set to the clipboard.
     * @returns `true` if successful, `false` otherwise.
     * @throws {TypeError} If `filePaths` is not an array of strings.
     *
     * @platform Windows: Supported. Linux: Not implemented.
     *
     * @example
     * ```typescript
     * bridge.setClipboardFiles(["C:/foo.txt", "C:/bar.png"]);
     * ```
     */
    setClipboardFiles(filePaths: string[]): boolean;

    /**
     * Gets the list of file paths from the clipboard (if clipboard contains files).
     *
     * @returns Array of file paths, or `null` if unavailable or clipboard does not contain files.
     *
     * @platform Windows: Supported. Linux: Not implemented.
     *
     * @example
     * ```typescript
     * const files = bridge.getClipboardFiles();
     * ```
     */
    getClipboardFiles(): string[] | null;

    /**
     * Sets remote clipboard file descriptors and file contents in a format suitable
     * for Remote Desktop / redirected clipboard file transfer.
     * 
     * @param filePaths - Array of absolute file paths to send remotely.
     * @returns `true` if the remote clipboard object was created successfully.
     * @throws {TypeError} If `filePaths` is not an array of strings.
     * 
     * @platform Windows: Supported. Linux: Not implemented.
     */
    setClipboardFilesRemote(filePaths: string[]): boolean;

    /**
     * Gets the list of remote clipboard file names from the current clipboard.
     * This reads a remote-capable file descriptor object, if available.
     * 
     * @returns Array of file names, or `null` if unavailable.
     * 
     * @platform Windows: Supported. Linux: Not implemented.
     */
    getClipboardFilesRemote(): string[] | null;

    /**
     * Returns the monitors known to the native backend.
     *
     * @returns A list of monitor descriptors.
     */
    getMonitors(): MonitorInfo[];

    /**
     * Selects the active monitor used by `moveMouseAbsolute(x, y)`.
     *
     * @param index - Monitor index returned by `getMonitors()`.
     * @returns `true` if the monitor was selected, `false` otherwise.
     */
    setCurrentMonitor(index: number): boolean;

    /**
     * Asynchronously initializes the native input bridge.
     * On Linux (Wayland), this requests a RemoteDesktop portal session and waits
     * for the user to grant permission. Returns a resolved Promise on success,
     * or rejects if the initialization times out (10s) or is denied.
     * On Windows, it resolves immediately.
     */
    init(): Promise<void>;

    /**
     * Queues a relative mouse movement.
     * 
     * @param x - The relative movement on the X axis.
     * @param y - The relative movement on the Y axis.
     * @throws {TypeError} If `x` or `y` is not a number.
     * 
     * @example
     * ```typescript
     * bridge.moveMouseRelative(10, -5);
     * bridge.flush(); // apply movement
     * ```
     */
    moveMouseRelative(x: number, y: number): void;

    /**
     * Queues an absolute mouse movement within the currently selected monitor.
     * Coordinates are local to that monitor (`0,0` = top-left of active monitor).
     * 
     * @param x - The local X coordinate on the active monitor.
     * @param y - The local Y coordinate on the active monitor.
     * @throws {TypeError} If `x` or `y` is not a number.
     * 
     * @example
     * ```typescript
     * bridge.setCurrentMonitor(0);
     * bridge.moveMouseAbsolute(960, 540); // center of selected 1920x1080 monitor
     * bridge.flush();
     * ```
     */
    moveMouseAbsolute(x: number, y: number): void;

    /**
     * Queues a mouse button press or release.
     * 
     * @param button - The mouse button (0 = left, 1 = right, 2 = middle).
     * @param down - `true` to press the button, `false` to release.
     * @throws {TypeError} If `button` is not a number or `down` is not a boolean.
     * 
     * @example
     * ```typescript
     * bridge.mouseClick(0, true);  // press left mouse button
     * bridge.mouseClick(0, false); // release left mouse button
     * bridge.flush();
     * ```
     */
    mouseClick(button: number, down: boolean): void;

    /**
     * Queues a keyboard key press or release using raw virtual key codes.
     * 
     * @param keyCode - The virtual key code of the key.
     * @param down - `true` to press the key, `false` to release.
     * @throws {TypeError} If `keyCode` is not a number or `down` is not a boolean.
     * 
     * @example
     * ```typescript
     * bridge.keyPress(0x41, true);  // press 'A'
     * bridge.keyPress(0x41, false); // release 'A'
     * bridge.flush();
     * ```
     */
    keyPress(keyCode: number, down: boolean): void;

    /**
     * Queues a mouse wheel scroll event.
     * 
     * @param delta - The scroll delta amount. Positive values scroll up, negative scroll down.
     * @throws {TypeError} If `delta` is not a number.
     * 
     * @example
     * ```typescript
     * bridge.scrollMouse(120); // scroll up
     * bridge.flush();
     * ```
     */
    scrollMouse(delta: number): void;

    /**
     * Queues a literal string of text to be typed by simulating Unicode character sequences.
     * 
     * @param text - The literal string to type.
     * @throws {TypeError} If `text` is not a string.
     * 
     * @example
     * ```typescript
     * bridge.typeString("Hello, World! ą, ć, ż, ź");
     * bridge.flush();
     * ```
     */
    typeString(text: string): void;

    /**
     * Optimizes queued relative mouse movements by dropping intermediate points 
     * that are closer than the given threshold.
     * 
     * @param distanceThreshold - The distance threshold in pixels.
     * @throws {TypeError} If `distanceThreshold` is not a number.
     * 
     * @example
     * ```typescript
     * bridge.optimizeMouseMovesRelative(5); // drop relative moves < 5px
     * ```
     */
    optimizeMouseMovesRelative(distanceThreshold: number): void;

    /**
     * Optimizes queued absolute mouse movements by dropping intermediate points 
     * that are closer than the given threshold.
     * 
     * @param distanceThreshold - The distance threshold in pixels.
     * @throws {TypeError} If `distanceThreshold` is not a number.
     * 
     * @example
     * ```typescript
     * bridge.optimizeMouseMovesAbsolute(10); // drop absolute moves < 10px
     * ```
     */
    optimizeMouseMovesAbsolute(distanceThreshold: number): void;

    /**
     * Toggles the internal state that allows or blocks mouse movement optimization.
     * 
     * @returns The new optimization state (`true` if enabled, `false` if disabled).
     * 
     * @example
     * ```typescript
     * const isEnabled = bridge.toggleOptimization();
     * console.log(isEnabled ? "Optimizations enabled" : "Optimizations disabled");
     * ```
     */
    toggleOptimization(): boolean;

    /**
     * Executes all queued input events as a single batch operation using the native OS API.
     * Clears the queue after execution.
     * 
     * @example
     * ```typescript
     * bridge.moveMouseRelative(5, 5);
     * bridge.mouseClick(0, true);
     * bridge.mouseClick(0, false);
     * bridge.flush(); // Sends all actions above to OS
     * ```
     */
    flush(): void;

    /**
     * Registers a callback to receive internal log messages from the C++ native backend.
     * 
     * @param callback - The function to call with log messages.
     * @throws {TypeError} If `callback` is not a function.
     * 
     * @example
     * ```typescript
     * bridge.setLogger((msg) => console.log(`[Native Bridge] ${msg}`));
     * ```
     */
    setLogger(callback: (msg: string) => void): void;

    /**
     * Registers a callback to receive clipboard push events.
     * The callback is invoked whenever `setClipboardText` or `setClipboardFiles` succeeds.
     * 
     * @param callback - The function to call with clipboard event data.
     * @throws {TypeError} If `callback` is not a function.
     * 
     * @example
     * ```typescript
     * bridge.onClipboard((event) => {
     *   if (event.type === 'text') {
     *     console.log('Text copied:', event.data);
     *   } else {
     *     console.log('Files copied:', event.data);
     *   }
     * });
     * ```
     */
    onClipboard(callback: (event: ClipboardEvent) => void): void;

    /**
     * Removes the registered clipboard event listener.
     * 
     * @example
     * ```typescript
     * bridge.offClipboard();
     * ```
     */
    offClipboard(): void;

    /**
     * Registers a callback to receive pushed input events (keyboard/mouse) from the platform.
     * The callback receives an `InputEvent` object describing the event.
     */
    onInput(callback: (event: InputEvent) => void): void;

    /**
     * Removes the registered input event listener.
     */
    offInput(): void;

    /**
     * Starts global input detection (hooks) on supported platforms (Windows).
     * Needs to be called to start receiving events via `onInput`.
     * 
     * @returns `true` if detection started successfully, `false` otherwise.
     * 
     * @platform Windows: Supported. Linux: Not implemented.
     */
    startInputDetection(): boolean;

    /**
     * Stops global input detection (hooks) on supported platforms.
     * 
     * @platform Windows: Supported. Linux: Not implemented.
     */
    stopInputDetection(): void;

    /**
     * Sets the distance threshold for optimizing detected input moves (hooks).
     * Movements smaller than this threshold will be filtered out.
     * 
     * @param distanceThreshold - The distance threshold in pixels. 0 disables optimization.
     */
    optimizeInputDetection(distanceThreshold: number): void;

    /**
     * Simulates a hardware key press using a standard `KeyboardEvent.code` string, 
     * acting as a universal Plug-and-Play mechanism.
     * 
     * Converts a DOM physical key code (e.g., "KeyA", "ShiftLeft") into the proper 
     * native layout-agnostic hardware code (Evdev for Linux, VK for Windows) 
     * and bypasses internal OS layout translation. Ideal for Remote Desktop Clients (RDC).
     * 
     * @param domCode - The physical key representation (e.g., "KeyA", "ShiftLeft", "Enter") from `event.code`.
     * @param down - `true` to press the key, `false` to release.
     * @returns `true` if the key was found in the internal dictionary and executed, `false` otherwise.
     * 
     * @example
     * ```typescript
     * // In an Electron App or web page capturing standard KeyboardEvents:
     * window.addEventListener('keydown', (e) => {
     *     bridge.keyPressDOM(e.code, true);
     *     bridge.flush();
     * });
     * 
     * window.addEventListener('keyup', (e) => {
     *     bridge.keyPressDOM(e.code, false);
     *     bridge.flush();
     * });
     * ```
     */
    keyPressDOM(domCode: string, down: boolean): boolean;
}

interface INativeAddon {
    InputBridge: new () => IInputBridge;
}

const rootDir = path.resolve(__dirname, '..');
const native = nodeGypBuild(rootDir) as INativeAddon;

import { mapDomCodeToNativeTarget } from './dom_mapper.js';

/**
 * Options for configuring the InputBridge instance.
 * Currently supports:
 * - `autoFlush`: If true, automatically calls `flush()` after every input action.
 *   Disable this if you want to batch multiple actions together for performance.
 *   @default false
 * - `flushIntervalMs`: If set to a positive number, automatically flushes queued input events
 *   at this interval (in milliseconds). If 0 or negative, auto-flush is disabled.
 *   @default 0 (disabled)
 */
export interface InputBridgeOptions {
    /**
     * If true, automatically calls `flush()` after every input action.
     * Disable this if you want to batch multiple actions together for performance.
     * @default false
     */
    autoFlush?: boolean;

    /**
     * If set to a positive number, automatically flushes queued input events
     * at this interval (in milliseconds). Helps prevent blocking Node.js event loop
     * by batching events and flushing them periodically.
     * @default 0 (disabled)
     */
    flushIntervalMs?: number;
}

/**
 * A TypeScript wrapper class around the native node-gyp module.
 * Provides standard methods for controlling system input devices as well as
 * higher-level abstractions for cross-platform DOM to Raw mapping (`keyPressDOM`).
 * 
 * Instance manages an internal native bridge state. Always call `flush()` 
 * to execute queued input actions on the system.
 */
export class InputBridge implements IInputBridge {


    private nativeBridge: IInputBridge;
    public autoFlush: boolean;
    private flushIntervalMs: number;
    private flushTimer: NodeJS.Timeout | null = null;

    constructor(options?: InputBridgeOptions) {
        this.nativeBridge = new native.InputBridge();
        this.autoFlush = options?.autoFlush ?? false;
        this.flushIntervalMs = options?.flushIntervalMs ?? 0;

        // Start auto-flush timer if interval is specified
        if (this.flushIntervalMs > 0) {
            this.flushTimer = setInterval(() => {
                this.flush();
            }, this.flushIntervalMs);
        }
    }

    async init(): Promise<void> {
        return this.nativeBridge.init();
    }

    setInputMode(mode: 'notify' | 'eis'): boolean {
        return this.nativeBridge.setInputMode(mode);
    }

    getInputMode(): string {
        return this.nativeBridge.getInputMode();
    }

    connectToEIS(): boolean {
        return this.nativeBridge.connectToEIS();
    }

    disconnectEIS(): void {
        this.nativeBridge.disconnectEIS();
    }

    isEISConnected(): boolean {
        return this.nativeBridge.isEISConnected();
    }

    moveMouseRelative(x: number, y: number): void {
        this.nativeBridge.moveMouseRelative(x, y);
        if (this.autoFlush) this.flush();
    }

    moveMouseAbsolute(x: number, y: number): void {
        this.nativeBridge.moveMouseAbsolute(x, y);
        if (this.autoFlush) this.flush();
    }

    mouseClick(button: number, down: boolean): void {
        this.nativeBridge.mouseClick(button, down);
        if (this.autoFlush) this.flush();
    }

    keyPress(keyCode: number, down: boolean): void {
        this.nativeBridge.keyPress(keyCode, down);
        if (this.autoFlush) this.flush();
    }

    scrollMouse(delta: number): void {
        this.nativeBridge.scrollMouse(delta);
        if (this.autoFlush) this.flush();
    }

    typeString(text: string): void {
        this.nativeBridge.typeString(text);
        if (this.autoFlush) this.flush();
    }

    optimizeMouseMovesRelative(distanceThreshold: number): void {
        this.nativeBridge.optimizeMouseMovesRelative(distanceThreshold);
        // Config change, doesn't need flush
    }

    optimizeMouseMovesAbsolute(distanceThreshold: number): void {
        this.nativeBridge.optimizeMouseMovesAbsolute(distanceThreshold);
        // Config change, doesn't need flush
    }

    toggleOptimization(): boolean {
        return this.nativeBridge.toggleOptimization();
    }

    flush(): void {
        this.nativeBridge.flush();
    }

    setLogger(callback: (msg: string) => void): void {
        this.nativeBridge.setLogger(callback);
    }

    onClipboard(callback: (event: ClipboardEvent) => void): void {
        this.nativeBridge.onClipboard(callback);
    }

    offClipboard(): void {
        this.nativeBridge.offClipboard();
    }

    onInput(callback: (event: InputEvent) => void): void {
        this.nativeBridge.onInput(callback as any);
    }

    offInput(): void {
        this.nativeBridge.offInput();
    }

    startInputDetection(): boolean {
        return this.nativeBridge.startInputDetection();
    }

    stopInputDetection(): void {
        this.nativeBridge.stopInputDetection();
    }

    optimizeInputDetection(distanceThreshold: number): void {
        this.nativeBridge.optimizeInputDetection(distanceThreshold);
    }

    /**
     * Stops the auto-flush timer if it is running.
     * Call this before discarding the instance to clean up resources.
     */
    stopAutoFlush(): void {
        if (this.flushTimer !== null) {
            clearInterval(this.flushTimer);
            this.flushTimer = null;
        }
    }

    keyPressDOM(domCode: string, down: boolean): boolean {
        const rawCode = mapDomCodeToNativeTarget(domCode);
        if (rawCode !== null) {
            this.nativeBridge.keyPress(rawCode, down);
            if (this.autoFlush) this.flush();
            return true;
        }
        return false;
    }

    setClipboardText(text: string): boolean {
        return this.nativeBridge.setClipboardText(text);
    }

    getClipboardText(): string | null {
        return this.nativeBridge.getClipboardText();
    }

    setClipboardFiles(filePaths: string[]): boolean {
        return this.nativeBridge.setClipboardFiles(filePaths);
    }

    getClipboardFiles(): string[] | null {
        return this.nativeBridge.getClipboardFiles();
    }

    setClipboardFilesRemote(filePaths: string[]): boolean {
        return this.nativeBridge.setClipboardFilesRemote(filePaths);
    }

    getClipboardFilesRemote(): string[] | null {
        return this.nativeBridge.getClipboardFilesRemote();
    }

    getMonitors(): MonitorInfo[] {
        return this.nativeBridge.getMonitors();
    }

    setCurrentMonitor(index: number): boolean {
        return this.nativeBridge.setCurrentMonitor(index);
    }
}

export default { InputBridge };
