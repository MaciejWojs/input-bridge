#include "../platform_input.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef NOSHELLAPI
#include <shellapi.h>
#include <iostream>
#include <string>

#ifndef DROPFILES
typedef struct _DROPFILES {
    DWORD pFiles;
    POINT pt;
    BOOL fNC;
    BOOL fWide;
} DROPFILES;
#endif

class PlatformInputWin : public IPlatformInput {
    private:
    std::vector<INPUT> m_winInputs;

    public:
    PlatformInputWin() {
        // Reserve memory up front to prevent frequent allocations on every flush
        m_winInputs.reserve(1024);
    }

    bool Initialize(std::string& error_msg) override {
        // Windows input injection generally does not require session authorization
        return true;
    }

    void MoveMouseRelative(int32_t x, int32_t y) override {
        Log("PlatformInputWin: MoveMouseRelative x=" + std::to_string(x) + " y=" + std::to_string(y));

        INPUT input = { 0 };
        input.type = INPUT_MOUSE;
        input.mi.dx = x;
        input.mi.dy = y;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &input, sizeof(INPUT));
    }

    void MoveMouseAbsolute(int32_t x, int32_t y) override {
        Log("PlatformInputWin: MoveMouseAbsolute x=" + std::to_string(x) + " y=" + std::to_string(y));

        INPUT input = { 0 };
        input.type = INPUT_MOUSE;
        input.mi.dx = (x * 65535) / (GetSystemMetrics(SM_CXSCREEN) - 1);
        input.mi.dy = (y * 65535) / (GetSystemMetrics(SM_CYSCREEN) - 1);
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        SendInput(1, &input, sizeof(INPUT));
    }

    void MouseClick(int32_t button, bool down) override {
        INPUT input = { 0 };
        input.type = INPUT_MOUSE;

        switch (button) {
        case 0:
            input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case 1:
            input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case 2:
            input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        default: return;
        }
        SendInput(1, &input, sizeof(INPUT));
    }

    void KeyPress(int32_t keyCode, bool down) override {
        INPUT input = { 0 };
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = static_cast<WORD>(keyCode);
        input.ki.wScan = static_cast<WORD>(MapVirtualKey(input.ki.wVk, MAPVK_VK_TO_VSC));

        input.ki.dwFlags = KEYEVENTF_SCANCODE;

        // Extended keys (like arrows, Home, End) require the KEYEVENTF_EXTENDEDKEY flag
        if (keyCode >= 33 && keyCode <= 46) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }

        if (!down) {
            input.ki.dwFlags |= KEYEVENTF_KEYUP;
        }
        SendInput(1, &input, sizeof(INPUT));
    }

    void ScrollMouse(int32_t delta) override {
        Log("PlatformInputWin: ScrollMouse delta=" + std::to_string(delta));

        INPUT input = { 0 };
        input.type = INPUT_MOUSE;
        input.mi.mouseData = delta;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        SendInput(1, &input, sizeof(INPUT));
    }

    void AppendUnicodeInputSequence(uint32_t charCode, std::vector<INPUT>& outputs) {
        auto append = [&](WORD scan, DWORD flags) {
            INPUT input = { 0 };
            input.type = INPUT_KEYBOARD;
            input.ki.wScan = scan;
            input.ki.dwFlags = flags;
            outputs.push_back(input);
            };

        if (charCode <= 0xFFFF) {
            append(static_cast<WORD>(charCode), KEYEVENTF_UNICODE);
            append(static_cast<WORD>(charCode), KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
            return;
        }

        uint32_t scalar = charCode - 0x10000;
        WORD highSurrogate = static_cast<WORD>(0xD800 + ((scalar >> 10) & 0x3FF));
        WORD lowSurrogate = static_cast<WORD>(0xDC00 + (scalar & 0x3FF));

        append(highSurrogate, KEYEVENTF_UNICODE);
        append(highSurrogate, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
        append(lowSurrogate, KEYEVENTF_UNICODE);
        append(lowSurrogate, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
    }

    void TypeCharacter(uint32_t charCode) override {
        Log("PlatformInputWin: TypeCharacter charCode=" + std::to_string(charCode));

        std::vector<INPUT> inputs;
        inputs.reserve(charCode > 0xFFFF ? 4 : 2);
        AppendUnicodeInputSequence(charCode, inputs);
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }

    void ExecuteEvents(const std::vector<InputEvent>& events) override {
        Log("PlatformInputWin: Execute batch of " + std::to_string(events.size()) + " events");

        m_winInputs.clear();

        for (const auto& ev : events) {
            std::vector<INPUT> eventInputs;
            eventInputs.reserve(4);

            std::visit([&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, struct MouseMoveRelative>) {
                    INPUT input = { 0 };
                    input.type = INPUT_MOUSE;
                    input.mi.dx = e.x;
                    input.mi.dy = e.y;
                    input.mi.dwFlags = MOUSEEVENTF_MOVE;
                    eventInputs.push_back(input);
                } else if constexpr (std::is_same_v<T, struct MouseMoveAbsolute>) {
                    INPUT input = { 0 };
                    input.type = INPUT_MOUSE;
                    input.mi.dx = (e.x * 65535) / (GetSystemMetrics(SM_CXSCREEN) - 1);
                    input.mi.dy = (e.y * 65535) / (GetSystemMetrics(SM_CYSCREEN) - 1);
                    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
                    eventInputs.push_back(input);
                } else if constexpr (std::is_same_v<T, struct MouseClick>) {
                    INPUT input = { 0 };
                    input.type = INPUT_MOUSE;
                    if (e.button == 0)      input.mi.dwFlags = e.down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                    else if (e.button == 1) input.mi.dwFlags = e.down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                    else if (e.button == 2) input.mi.dwFlags = e.down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
                    eventInputs.push_back(input);
                } else if constexpr (std::is_same_v<T, struct KeyPress>) {
                    INPUT input = { 0 };
                    input.type = INPUT_KEYBOARD;
                    input.ki.wVk = static_cast<WORD>(e.keyCode);
                    input.ki.wScan = MapVirtualKey(input.ki.wVk, MAPVK_VK_TO_VSC);
                    input.ki.dwFlags = KEYEVENTF_SCANCODE;
                    if (!e.down) {
                        input.ki.dwFlags |= KEYEVENTF_KEYUP;
                    }
                    eventInputs.push_back(input);
                } else if constexpr (std::is_same_v<T, struct MouseScroll>) {
                    INPUT input = { 0 };
                    input.type = INPUT_MOUSE;
                    input.mi.mouseData = e.delta;
                    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
                    eventInputs.push_back(input);
                } else if constexpr (std::is_same_v<T, struct TypeCharacter>) {
                    AppendUnicodeInputSequence(e.charCode, eventInputs);
                }
                }, ev);

            m_winInputs.insert(m_winInputs.end(), eventInputs.begin(), eventInputs.end());
        }

        if (!m_winInputs.empty()) {
            SendInput(static_cast<UINT>(m_winInputs.size()), m_winInputs.data(), sizeof(INPUT));
        }
    }

    // Clipboard: Set text
    bool SetClipboardText(const std::string& text) override {
        if (!OpenClipboard(nullptr)) return false;
        if (!EmptyClipboard()) { CloseClipboard(); return false; }
        size_t size = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!hMem) { CloseClipboard(); return false; }
        wchar_t* wstr = (wchar_t*)GlobalLock(hMem);
        int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wstr, (int)(size / sizeof(wchar_t)));
        GlobalUnlock(hMem);
        if (!SetClipboardData(CF_UNICODETEXT, hMem)) { GlobalFree(hMem); CloseClipboard(); return false; }
        CloseClipboard();
        return true;
    }

    // Clipboard: Get text
    std::optional<std::string> GetClipboardText() override {
        if (!OpenClipboard(nullptr)) return std::nullopt;
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (!hData) { CloseClipboard(); return std::nullopt; }
        wchar_t* wstr = (wchar_t*)GlobalLock(hData);
        if (!wstr) { CloseClipboard(); return std::nullopt; }
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
        std::string result(len - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), len, nullptr, nullptr);
        GlobalUnlock(hData);
        CloseClipboard();
        return result;
    }

    // Clipboard: Set files (CF_HDROP)
    bool SetClipboardFiles(const std::vector<std::string>& filePaths) override {
        if (!OpenClipboard(nullptr)) return false;
        if (!EmptyClipboard()) { CloseClipboard(); return false; }
        // Convert UTF-8 paths to wide strings and double-null-terminated list
        std::wstring filesConcat;
        for (const auto& path : filePaths) {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
            std::wstring wpath(wlen - 1, 0);
            MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
            filesConcat.append(wpath);
            filesConcat.push_back(L'\0');
        }
        filesConcat.push_back(L'\0');
        size_t dropfilesSize = sizeof(DROPFILES) + filesConcat.size() * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dropfilesSize);
        if (!hMem) { CloseClipboard(); return false; }
        DROPFILES* df = (DROPFILES*)GlobalLock(hMem);
        df->pFiles = sizeof(DROPFILES);
        df->pt.x = 0; df->pt.y = 0; df->fNC = FALSE; df->fWide = TRUE;
        memcpy((BYTE*)df + sizeof(DROPFILES), filesConcat.data(), filesConcat.size() * sizeof(wchar_t));
        GlobalUnlock(hMem);
        if (!SetClipboardData(CF_HDROP, hMem)) { GlobalFree(hMem); CloseClipboard(); return false; }
        CloseClipboard();
        return true;
    }

    // Clipboard: Get files (CF_HDROP)
    std::optional<std::vector<std::string>> GetClipboardFiles() override {
        if (!OpenClipboard(nullptr)) return std::nullopt;
        HANDLE hData = GetClipboardData(CF_HDROP);
        if (!hData) { CloseClipboard(); return std::nullopt; }
        HDROP hDrop = (HDROP)hData;
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::string> files;
        for (UINT i = 0; i < count; ++i) {
            UINT len = DragQueryFileW(hDrop, i, nullptr, 0) + 1;
            std::wstring wpath(len, 0);
            DragQueryFileW(hDrop, i, wpath.data(), len);
            int utf8len = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string path(utf8len - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path.data(), utf8len, nullptr, nullptr);
            files.push_back(path);
        }
        CloseClipboard();
        return files;
    }
};
