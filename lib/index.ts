import path from 'node:path';
import { fileURLToPath } from 'node:url';
import nodeGypBuild from 'node-gyp-build';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

/**
 * Interface representing the native input bridge for simulating hardware events.
 * Actions are queued and must be dispatched using `flush()`.
 */
export interface IInputBridge {
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
     * 
     * @example
     * ```typescript
     * bridge.setLogger((msg) => console.log(`[Native Bridge] ${msg}`));
     * ```
     */
    setLogger(callback: (msg: string) => void): void;

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

    keyPressDOM(domCode: string, down: boolean): boolean {
        const rawCode = mapDomCodeToNativeTarget(domCode);
        if (rawCode !== null) {
            this.nativeBridge.keyPress(rawCode, down);
            if (this.autoFlush) this.flush();
            return true;
        }
        return false;
    }
}

export default { InputBridge };
