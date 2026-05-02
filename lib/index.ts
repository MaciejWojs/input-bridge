import path from 'node:path';
import { fileURLToPath } from 'node:url';
import nodeGypBuild from 'node-gyp-build';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

export type ClipboardEventType = 'text' | 'files';

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
     * Queues an absolute mouse movement to a specific screen coordinate.
     * 
     * @param x - The absolute X coordinate.
     * @param y - The absolute Y coordinate.
     * @throws {TypeError} If `x` or `y` is not a number.
     * 
     * @example
     * ```typescript
     * bridge.moveMouseAbsolute(1920 / 2, 1080 / 2); // move to center of a 1080p screen
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
 */
export interface InputBridgeOptions {
    /**
     * If true, automatically calls `flush()` after every input action.
     * Disable this if you want to batch multiple actions together for performance.
     * @default false
     */
    autoFlush?: boolean;
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

    constructor(options?: InputBridgeOptions) {
        this.nativeBridge = new native.InputBridge();
        this.autoFlush = options?.autoFlush ?? false;
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
}

export default { InputBridge };
