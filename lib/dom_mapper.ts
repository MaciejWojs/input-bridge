import * as os from 'node:os';

const FLAG_RAW_WINDOWS = 0x10000000;
const FLAG_RAW_LINUX   = 0x20000000;

// Zmapowanie fizycznego klawisza (DOM event.code) z przeglądarki/Electrona 
// bezpośrednio na uniwersalny sprzętowy skankod dla Linuxa (Evdev)
const DomToLinuxEvdev: Record<string, number> = {
    // Litery
    'KeyA': 30, 'KeyB': 48, 'KeyC': 46, 'KeyD': 32, 'KeyE': 18, 'KeyF': 33, 
    'KeyG': 34, 'KeyH': 35, 'KeyI': 23, 'KeyJ': 36, 'KeyK': 37, 'KeyL': 38, 
    'KeyM': 50, 'KeyN': 49, 'KeyO': 24, 'KeyP': 25, 'KeyQ': 16, 'KeyR': 19, 
    'KeyS': 31, 'KeyT': 20, 'KeyU': 22, 'KeyV': 47, 'KeyW': 17, 'KeyX': 45, 
    'KeyY': 21, 'KeyZ': 44,

    // Cyfry
    'Digit1': 2, 'Digit2': 3, 'Digit3': 4, 'Digit4': 5, 'Digit5': 6, 
    'Digit6': 7, 'Digit7': 8, 'Digit8': 9, 'Digit9': 10, 'Digit0': 11,

    // Znaki specjalne
    'Minus': 12, 'Equal': 13, 'BracketLeft': 26, 'BracketRight': 27, 
    'Semicolon': 39, 'Quote': 40, 'Backquote': 41, 'Backslash': 43, 
    'Comma': 51, 'Period': 52, 'Slash': 53,

    // Klawisze sterujące
    'Escape': 1, 'Enter': 28, 'Tab': 15, 'Space': 57, 'Backspace': 14, 
    'CapsLock': 58,

    // Modyfikatory
    'ShiftLeft': 42, 'ShiftRight': 54, 
    'ControlLeft': 29, 'ControlRight': 97, 
    'AltLeft': 56, 'AltRight': 100, 
    'MetaLeft': 125, 'MetaRight': 126, // Win / Super
    'OSLeft': 125, 'OSRight': 126,

    // Nawigacja i strzałki
    'ArrowUp': 103, 'ArrowDown': 108, 'ArrowLeft': 105, 'ArrowRight': 106,
    'Insert': 110, 'Delete': 111, 'Home': 102, 'End': 107, 
    'PageUp': 104, 'PageDown': 109,

    // Numpad
    'NumLock': 69, 
    'NumpadDivide': 98, 'NumpadMultiply': 55, 'NumpadSubtract': 74, 
    'NumpadAdd': 78, 'NumpadEnter': 96, 'NumpadDecimal': 83,
    'Numpad0': 82, 'Numpad1': 79, 'Numpad2': 80, 'Numpad3': 81, 
    'Numpad4': 75, 'Numpad5': 76, 'Numpad6': 77, 'Numpad7': 71, 
    'Numpad8': 72, 'Numpad9': 73,

    // Funkcyjne
    'F1': 59, 'F2': 60, 'F3': 61, 'F4': 62, 'F5': 63, 'F6': 64, 
    'F7': 65, 'F8': 66, 'F9': 67, 'F10': 68, 'F11': 87, 'F12': 88
};

// Zmapowanie fizycznego klawisza (DOM event.code) z przeglądarki/Electrona 
// na kody Windows Virtual Key (VK). Używane z pominięciem tłumaczenia przez RAW flag.
const DomToWindowsVK: Record<string, number> = {
    // Litery
    'KeyA': 0x41, 'KeyB': 0x42, 'KeyC': 0x43, 'KeyD': 0x44, 'KeyE': 0x45, 'KeyF': 0x46, 
    'KeyG': 0x47, 'KeyH': 0x48, 'KeyI': 0x49, 'KeyJ': 0x4A, 'KeyK': 0x4B, 'KeyL': 0x4C, 
    'KeyM': 0x4D, 'KeyN': 0x4E, 'KeyO': 0x4F, 'KeyP': 0x50, 'KeyQ': 0x51, 'KeyR': 0x52, 
    'KeyS': 0x53, 'KeyT': 0x54, 'KeyU': 0x55, 'KeyV': 0x56, 'KeyW': 0x57, 'KeyX': 0x58, 
    'KeyY': 0x59, 'KeyZ': 0x5A,

    // Cyfry
    'Digit1': 0x31, 'Digit2': 0x32, 'Digit3': 0x33, 'Digit4': 0x34, 'Digit5': 0x35, 
    'Digit6': 0x36, 'Digit7': 0x37, 'Digit8': 0x38, 'Digit9': 0x39, 'Digit0': 0x30,

    // Znaki specjalne
    'Minus': 0xBD, 'Equal': 0xBB, 'BracketLeft': 0xDB, 'BracketRight': 0xDD, 
    'Semicolon': 0xBA, 'Quote': 0xDE, 'Backquote': 0xC0, 'Backslash': 0xDC, 
    'Comma': 0xBC, 'Period': 0xBE, 'Slash': 0xBF,

    // Klawisze sterujące
    'Escape': 0x1B, 'Enter': 0x0D, 'Tab': 0x09, 'Space': 0x20, 'Backspace': 0x08, 
    'CapsLock': 0x14,

    // Modyfikatory
    'ShiftLeft': 0xA0, 'ShiftRight': 0xA1, 
    'ControlLeft': 0xA2, 'ControlRight': 0xA3, 
    'AltLeft': 0xA4, 'AltRight': 0xA5, 
    'MetaLeft': 0x5B, 'MetaRight': 0x5C,
    'OSLeft': 0x5B, 'OSRight': 0x5C,

    // Nawigacja i strzałki
    'ArrowUp': 0x26, 'ArrowDown': 0x28, 'ArrowLeft': 0x25, 'ArrowRight': 0x27,
    'Insert': 0x2D, 'Delete': 0x2E, 'Home': 0x24, 'End': 0x23, 
    'PageUp': 0x21, 'PageDown': 0x22,

    // Numpad
    'NumLock': 0x90, 
    'NumpadDivide': 0x6F, 'NumpadMultiply': 0x6A, 'NumpadSubtract': 0x6D, 
    'NumpadAdd': 0x6B, 'NumpadEnter': 0x0D, 'NumpadDecimal': 0x6E,
    'Numpad0': 0x60, 'Numpad1': 0x61, 'Numpad2': 0x62, 'Numpad3': 0x63, 
    'Numpad4': 0x64, 'Numpad5': 0x65, 'Numpad6': 0x66, 'Numpad7': 0x67, 
    'Numpad8': 0x68, 'Numpad9': 0x69,

    // Funkcyjne
    'F1': 0x70, 'F2': 0x71, 'F3': 0x72, 'F4': 0x73, 'F5': 0x74, 'F6': 0x75, 
    'F7': 0x76, 'F8': 0x77, 'F9': 0x78, 'F10': 0x79, 'F11': 0x7A, 'F12': 0x7B
};

/**
 * Konwertuje stringowy identyfikator fizycznego klawisza (np. "KeyA", "ShiftLeft")
 * z `KeyboardEvent.code` przeglądarki na gotowy do wstrzyknięcia do C++
 * surowy kod dla odpowiedniego systemu operacyjnego.
 * Używa sprzętowego flagowania RAW, by ominąć translację natywnego C++.
 */
export function mapDomCodeToNativeTarget(codeStr: string): number | null {
    const isLinux = os.platform() === 'linux';

    if (isLinux) {
        const evdev = DomToLinuxEvdev[codeStr];
        // Jeśli na Linuksie wspieramy RAW - dodajemy maskę
        return evdev !== undefined ? (evdev | FLAG_RAW_LINUX) : null;
    } else {
        // Platformy Windows i fallback
        const vk = DomToWindowsVK[codeStr];
        // Wysyłamy RAW WindowsVK by backend zignorował ew tłumaczenie odwrotne w hookach C++.
        return vk !== undefined ? (vk | FLAG_RAW_WINDOWS) : null;
    }
}
