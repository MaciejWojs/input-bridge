import path from 'node:path';
import { fileURLToPath } from 'node:url';
import nodeGypBuild from 'node-gyp-build';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

export interface IInputBridge {
    moveMouse(x: number, y: number): void;
    mouseClick(button: number, down: boolean): void;
    keyPress(keyCode: number, down: boolean): void;
    scrollMouse(delta: number): void;
    setLogger(callback: (msg: string) => void): void;
}

export interface INativeAddon {
    InputBridge: new () => IInputBridge;
}

const rootDir = path.resolve(__dirname, '..');
const native = nodeGypBuild(rootDir) as INativeAddon;

export const InputBridge = native.InputBridge;
export default native;
