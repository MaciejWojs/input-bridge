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
     * Queues a relative mouse movement.
     * @param x - The relative movement on the X axis.
     * @param y - The relative movement on the Y axis.
     */
    moveMouseRelative(x: number, y: number): void;

    /**
     * Queues an absolute mouse movement to a specific screen coordinate.
     * @param x - The absolute X coordinate.
     * @param y - The absolute Y coordinate.
     */
    moveMouseAbsolute(x: number, y: number): void;

    /**
     * Queues a mouse button press or release.
     * @param button - The mouse button (0 = left, 1 = right, 2 = middle).
     * @param down - `true` to press the button, `false` to release.
     */
    mouseClick(button: number, down: boolean): void;

    /**
     * Queues a keyboard key press or release.
     * @param keyCode - The virtual key code of the key.
     * @param down - `true` to press the key, `false` to release.
     */
    keyPress(keyCode: number, down: boolean): void;

    /**
     * Queues a mouse wheel scroll event.
     * @param delta - The scroll delta amount.
     */
    scrollMouse(delta: number): void;

    /**
     * Queues a literal string of text to be typed by simulating Unicode character sequences.
     * @param text - The literal string to type.
     */
    typeString(text: string): void;

    /**
     * Optimizes queued relative mouse movements by dropping intermediate points 
     * that are closer than the given threshold.
     * @param distanceThreshold - The distance threshold in pixels.
     */
    optimizeMouseMovesRelative(distanceThreshold: number): void;

    /**
     * Optimizes queued absolute mouse movements by dropping intermediate points 
     * that are closer than the given threshold.
     * @param distanceThreshold - The distance threshold in pixels.
     */
    optimizeMouseMovesAbsolute(distanceThreshold: number): void;

    /**
     * Toggles the internal state that allows or blocks mouse movement optimization.
     * @returns The new optimization state (`true` if enabled, `false` if disabled).
     */
    toggleOptimization(): boolean;

    /**
     * Executes all queued input events as a single batch operation using the native OS API.
     * Clears the queue after execution.
     */
    flush(): void;

    /**
     * Registers a callback to receive internal log messages from the C++ native backend.
     * @param callback - The function to call with log messages.
     */
    setLogger(callback: (msg: string) => void): void;
}

export interface INativeAddon {
    InputBridge: new () => IInputBridge;
}

const rootDir = path.resolve(__dirname, '..');
const native = nodeGypBuild(rootDir) as INativeAddon;

export const InputBridge = native.InputBridge;
export default native;
