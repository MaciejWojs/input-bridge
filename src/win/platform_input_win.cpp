#include "../platform_input.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef NOSHELLAPI
#include <shellapi.h>
#include <shlobj.h>
#include <objidl.h>
#include <ole2.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring result(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, result.data(), wlen);
    return result;
}

static std::wstring GetFileName(const std::wstring& fullPath) {
    size_t pos = fullPath.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return fullPath;
    return fullPath.substr(pos + 1);
}

static std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return {};
    std::string result(utf8Len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), utf8Len, nullptr, nullptr);
    return result;
}

static HGLOBAL DuplicateGlobalHandle(HGLOBAL source) {
    if (!source) return nullptr;
    SIZE_T size = GlobalSize(source);
    if (!size) return nullptr;
    HGLOBAL dest = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!dest) return nullptr;
    void* srcPtr = GlobalLock(source);
    void* dstPtr = GlobalLock(dest);
    if (!srcPtr || !dstPtr) {
        if (dstPtr) GlobalUnlock(dest);
        if (srcPtr) GlobalUnlock(source);
        GlobalFree(dest);
        return nullptr;
    }
    memcpy(dstPtr, srcPtr, size);
    GlobalUnlock(dest);
    GlobalUnlock(source);
    return dest;
}

static HGLOBAL CreateDropFilesHandle(const std::vector<std::wstring>& filePaths) {
    std::wstring data;
    for (const auto& path : filePaths) {
        data.append(path);
        data.push_back(L'\0');
    }
    data.push_back(L'\0');
    SIZE_T totalSize = sizeof(DROPFILES) + data.size() * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, totalSize);
    if (!hMem) return nullptr;
    DROPFILES* df = reinterpret_cast<DROPFILES*>(GlobalLock(hMem));
    if (!df) { GlobalFree(hMem); return nullptr; }
    df->pFiles = sizeof(DROPFILES);
    df->pt.x = 0;
    df->pt.y = 0;
    df->fNC = FALSE;
    df->fWide = TRUE;
    memcpy(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES), data.data(), data.size() * sizeof(wchar_t));
    GlobalUnlock(hMem);
    return hMem;
}

static HGLOBAL CreateGlobalFromData(const void* data, SIZE_T size) {
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hMem) return nullptr;
    void* ptr = GlobalLock(hMem);
    if (!ptr) { GlobalFree(hMem); return nullptr; }
    memcpy(ptr, data, size);
    GlobalUnlock(hMem);
    return hMem;
}

static bool ReadFileToBytes(const std::wstring& filePath, std::vector<uint8_t>& out) {
    HANDLE file = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > static_cast<LONGLONG>(SIZE_MAX)) {
        CloseHandle(file);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    bool ok = true;
    if (size.QuadPart > 0) {
        ok = ReadFile(file, out.data(), static_cast<DWORD>(size.QuadPart), &bytesRead, nullptr) != FALSE && bytesRead == static_cast<DWORD>(size.QuadPart);
    }
    CloseHandle(file);
    return ok;
}

class RemoteFileDataObject : public IDataObject {
    private:
    LONG m_ref = 1;
    std::vector<std::wstring> m_fullPaths;
    std::vector<std::wstring> m_fileNames;
    std::vector<std::vector<uint8_t>> m_fileContents;
    HGLOBAL m_descriptorHandle = nullptr;
    std::vector<FORMATETC> m_formats;
    UINT m_cfFileDescriptor = 0;
    UINT m_cfFileContents = 0;
    UINT m_cfPreferredDropEffect = 0;

    HRESULT PrepareDescriptor() {
        size_t count = m_fileNames.size();
        if (count == 0) return E_FAIL;

        size_t descriptorSize = sizeof(FILEGROUPDESCRIPTORW) + (count - 1) * sizeof(FILEDESCRIPTORW);
        m_descriptorHandle = GlobalAlloc(GMEM_MOVEABLE, descriptorSize);
        if (!m_descriptorHandle) return STG_E_MEDIUMFULL;

        FILEGROUPDESCRIPTORW* groupDesc = reinterpret_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(m_descriptorHandle));
        if (!groupDesc) { GlobalFree(m_descriptorHandle); m_descriptorHandle = nullptr; return STG_E_MEDIUMFULL; }

        groupDesc->cItems = static_cast<UINT>(count);
        for (size_t i = 0; i < count; ++i) {
            FILEDESCRIPTORW& fileDesc = groupDesc->fgd[i];
            ZeroMemory(&fileDesc, sizeof(FILEDESCRIPTORW));
            fileDesc.dwFlags = FD_FILESIZE;
            fileDesc.nFileSizeLow = static_cast<DWORD>(m_fileContents[i].size());
            fileDesc.nFileSizeHigh = static_cast<DWORD>((static_cast<uint64_t>(m_fileContents[i].size()) >> 32) & 0xFFFFFFFF);
            const std::wstring& name = m_fileNames[i];
            wcsncpy_s(fileDesc.cFileName, name.c_str(), _TRUNCATE);
        }

        GlobalUnlock(m_descriptorHandle);
        return S_OK;
    }

    void PrepareFormats() {
        m_cfFileDescriptor = RegisterClipboardFormat(CFSTR_FILEDESCRIPTORW);
        m_cfFileContents = RegisterClipboardFormat(CFSTR_FILECONTENTS);
        m_cfPreferredDropEffect = RegisterClipboardFormat(CFSTR_PREFERREDDROPEFFECT);

        m_formats.clear();
        FORMATETC fileDescriptorFmt = { static_cast<CLIPFORMAT>(m_cfFileDescriptor), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        FORMATETC fileContentsFmt = { static_cast<CLIPFORMAT>(m_cfFileContents), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM };
        FORMATETC preferredDropFmt = { static_cast<CLIPFORMAT>(m_cfPreferredDropEffect), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        FORMATETC hdropFmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };

        m_formats.push_back(fileDescriptorFmt);
        m_formats.push_back(fileContentsFmt);
        m_formats.push_back(preferredDropFmt);
        m_formats.push_back(hdropFmt);
    }

    bool IsFormatSupported(FORMATETC* fmt) const {
        if (!fmt) return false;
        if (fmt->dwAspect != DVASPECT_CONTENT) return false;
        if (fmt->cfFormat == static_cast<CLIPFORMAT>(m_cfFileDescriptor) && (fmt->tymed & TYMED_HGLOBAL)) return true;
        if (fmt->cfFormat == static_cast<CLIPFORMAT>(m_cfFileContents) && (fmt->tymed & TYMED_ISTREAM)) return true;
        if (fmt->cfFormat == static_cast<CLIPFORMAT>(m_cfPreferredDropEffect) && (fmt->tymed & TYMED_HGLOBAL)) return true;
        if (fmt->cfFormat == CF_HDROP && (fmt->tymed & TYMED_HGLOBAL)) return true;
        return false;
    }

    public:
    RemoteFileDataObject(std::vector<std::wstring>&& fullPaths, std::vector<std::wstring>&& fileNames, std::vector<std::vector<uint8_t>>&& fileContents)
        : m_fullPaths(std::move(fullPaths)), m_fileNames(std::move(fileNames)), m_fileContents(std::move(fileContents)) {
        PrepareDescriptor();
        PrepareFormats();
    }

    ~RemoteFileDataObject() {
        if (m_descriptorHandle) {
            GlobalFree(m_descriptorHandle);
            m_descriptorHandle = nullptr;
        }
    }

    HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppvObject = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG __stdcall AddRef() override {
        return InterlockedIncrement(&m_ref);
    }

    ULONG __stdcall Release() override {
        ULONG count = InterlockedDecrement(&m_ref);
        if (count == 0) delete this;
        return count;
    }

    HRESULT __stdcall GetData(FORMATETC* pFormatEtc, STGMEDIUM* pMedium) override {
        if (!pFormatEtc || !pMedium) return E_POINTER;
        if (!IsFormatSupported(pFormatEtc)) return DV_E_FORMATETC;

        pMedium->pUnkForRelease = nullptr;
        pMedium->tymed = TYMED_NULL;
        pMedium->hGlobal = nullptr;
        pMedium->pstm = nullptr;

        if (pFormatEtc->cfFormat == static_cast<CLIPFORMAT>(m_cfFileDescriptor)) {
            if (!(pFormatEtc->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
            if (!m_descriptorHandle) return E_FAIL;
            HGLOBAL hCopy = DuplicateGlobalHandle(m_descriptorHandle);
            if (!hCopy) return STG_E_MEDIUMFULL;
            pMedium->tymed = TYMED_HGLOBAL;
            pMedium->hGlobal = hCopy;
            return S_OK;
        }

        if (pFormatEtc->cfFormat == static_cast<CLIPFORMAT>(m_cfFileContents)) {
            if (!(pFormatEtc->tymed & TYMED_ISTREAM)) return DV_E_TYMED;
            LONG index = pFormatEtc->lindex;
            if (index < 0 || static_cast<size_t>(index) >= m_fileContents.size()) return DV_E_LINDEX;
            const std::vector<uint8_t>& bytes = m_fileContents[static_cast<size_t>(index)];
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
            if (!hMem) return STG_E_MEDIUMFULL;
            void* ptr = GlobalLock(hMem);
            if (!ptr) { GlobalFree(hMem); return STG_E_MEDIUMFULL; }
            memcpy(ptr, bytes.data(), bytes.size());
            GlobalUnlock(hMem);
            IStream* stream = nullptr;
            HRESULT hr = CreateStreamOnHGlobal(hMem, TRUE, &stream);
            if (FAILED(hr)) { GlobalFree(hMem); return hr; }
            pMedium->tymed = TYMED_ISTREAM;
            pMedium->pstm = stream;
            return S_OK;
        }

        if (pFormatEtc->cfFormat == static_cast<CLIPFORMAT>(m_cfPreferredDropEffect)) {
            if (!(pFormatEtc->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
            DWORD dropEffect = DROPEFFECT_COPY;
            HGLOBAL hMem = CreateGlobalFromData(&dropEffect, sizeof(dropEffect));
            if (!hMem) return STG_E_MEDIUMFULL;
            pMedium->tymed = TYMED_HGLOBAL;
            pMedium->hGlobal = hMem;
            return S_OK;
        }

        if (pFormatEtc->cfFormat == CF_HDROP) {
            if (!(pFormatEtc->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
            HGLOBAL hMem = CreateDropFilesHandle(m_fullPaths);
            if (!hMem) return STG_E_MEDIUMFULL;
            pMedium->tymed = TYMED_HGLOBAL;
            pMedium->hGlobal = hMem;
            return S_OK;
        }

        return DV_E_FORMATETC;
    }

    HRESULT __stdcall GetDataHere(FORMATETC* /*pFormatEtc*/, STGMEDIUM* /*pMedium*/) override {
        return DV_E_FORMATETC;
    }

    HRESULT __stdcall QueryGetData(FORMATETC* pFormatEtc) override {
        if (!pFormatEtc) return E_POINTER;
        if (!IsFormatSupported(pFormatEtc)) return DV_E_FORMATETC;
        return S_OK;
    }

    HRESULT __stdcall GetCanonicalFormatEtc(FORMATETC* pFormatEtc, FORMATETC* pFormatEtcOut) override {
        if (!pFormatEtcOut) return E_POINTER;
        *pFormatEtcOut = *pFormatEtc;
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT __stdcall SetData(FORMATETC* /*pFormatEtc*/, STGMEDIUM* /*pMedium*/, BOOL /*fRelease*/) override {
        return E_NOTIMPL;
    }

    HRESULT __stdcall EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppEnumFormatEtc) override {
        if (!ppEnumFormatEtc) return E_POINTER;
        if (dwDirection != DATADIR_GET) return E_NOTIMPL;
        return SHCreateStdEnumFmtEtc(static_cast<ULONG>(m_formats.size()), m_formats.data(), ppEnumFormatEtc);
    }

    HRESULT __stdcall DAdvise(FORMATETC* /*pFormatEtc*/, DWORD /*advf*/, IAdviseSink* /*pAdvSink*/, DWORD* /*pdwConnection*/) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT __stdcall DUnadvise(DWORD /*dwConnection*/) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT __stdcall EnumDAdvise(IEnumSTATDATA** /*ppEnumAdvise*/) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
};

class PlatformInputWin : public IPlatformInput {
    private:
    std::vector<INPUT> m_winInputs;
    ClipboardChangeCallback m_clipboardCallback;
    std::thread m_clipboardThread;
    std::atomic<bool> m_clipboardThreadRunning{ false };
    std::mutex m_clipboardMutex;
    std::condition_variable m_clipboardReady;
    HWND m_clipboardWindow = nullptr;

    InputEventCallback m_inputCallback;
    std::jthread m_inputDetectionThread;
    std::atomic<bool> m_inputDetectionRunning{ false };
    std::mutex m_inputDetectionMutex;
    std::condition_variable m_inputDetectionReady;
    DWORD m_inputDetectionThreadId = 0;
    HHOOK m_kbHook = nullptr;
    HHOOK m_msHook = nullptr;

    std::atomic<int> m_detectionDistanceThreshold{ 0 };
    std::atomic<int> m_lastDetectedMouseX{ -1 };
    std::atomic<int> m_lastDetectedMouseY{ -1 };

    static PlatformInputWin* s_instance;

    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode == HC_ACTION && s_instance && s_instance->m_inputCallback) {
            KBDLLHOOKSTRUCT* pkbhs = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            ::KeyPress kp;
            kp.keyCode = static_cast<int32_t>(pkbhs->vkCode);
            kp.down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

            char name[256];
            LONG scancode = pkbhs->scanCode << 16;
            if (pkbhs->flags & LLKHF_EXTENDED) {
                scancode |= 0x01000000;
            }
            if (GetKeyNameTextA(scancode, name, 256) > 0) {
                kp.domCode = std::string(name);
            }

            InputEvent ev = kp;
            s_instance->m_inputCallback(ev);
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode == HC_ACTION && s_instance && s_instance->m_inputCallback) {
            MSLLHOOKSTRUCT* pmshs = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            if (wParam == WM_MOUSEMOVE) {
                int x = static_cast<int32_t>(pmshs->pt.x);
                int y = static_cast<int32_t>(pmshs->pt.y);
                int threshold = s_instance->m_detectionDistanceThreshold.load(std::memory_order_relaxed);
                int lastX = s_instance->m_lastDetectedMouseX.load(std::memory_order_relaxed);
                int lastY = s_instance->m_lastDetectedMouseY.load(std::memory_order_relaxed);

                if (threshold > 0 && lastX != -1) {
                    long long dx = static_cast<long long>(x) - lastX;
                    long long dy = static_cast<long long>(y) - lastY;
                    if ((dx * dx + dy * dy) < static_cast<long long>(threshold * threshold)) {
                        return CallNextHookEx(nullptr, nCode, wParam, lParam);
                    }
                }

                s_instance->m_lastDetectedMouseX.store(x, std::memory_order_relaxed);
                s_instance->m_lastDetectedMouseY.store(y, std::memory_order_relaxed);

                ::MouseMoveAbsolute mm;
                mm.x = x;
                mm.y = y;
                InputEvent ev = mm;
                s_instance->m_inputCallback(ev);
            } else if (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP) {
                ::MouseClick mc;
                mc.button = 0;
                mc.down = (wParam == WM_LBUTTONDOWN);
                InputEvent ev = mc;
                s_instance->m_inputCallback(ev);
            } else if (wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP) {
                ::MouseClick mc;
                mc.button = 1;
                mc.down = (wParam == WM_RBUTTONDOWN);
                InputEvent ev = mc;
                s_instance->m_inputCallback(ev);
            } else if (wParam == WM_MBUTTONDOWN || wParam == WM_MBUTTONUP) {
                ::MouseClick mc;
                mc.button = 2;
                mc.down = (wParam == WM_MBUTTONDOWN);
                InputEvent ev = mc;
                s_instance->m_inputCallback(ev);
            } else if (wParam == WM_MOUSEWHEEL) {
                ::MouseScroll ms;
                ms.delta = static_cast<int32_t>(static_cast<short>(HIWORD(pmshs->mouseData)));
                InputEvent ev = ms;
                s_instance->m_inputCallback(ev);
            }
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    void RunInputDetectionLoop() {
        {
            std::lock_guard<std::mutex> lock(m_inputDetectionMutex);
            m_inputDetectionThreadId = GetCurrentThreadId();

            s_instance = this;
            m_kbHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(nullptr), 0);
            m_msHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(nullptr), 0);

            m_inputDetectionReady.notify_all();
        }

        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        {
            std::lock_guard<std::mutex> lock(m_inputDetectionMutex);
            if (m_kbHook) {
                UnhookWindowsHookEx(m_kbHook);
                m_kbHook = nullptr;
            }
            if (m_msHook) {
                UnhookWindowsHookEx(m_msHook);
                m_msHook = nullptr;
            }
            s_instance = nullptr;
            m_inputDetectionThreadId = 0;
            m_inputDetectionRunning = false;
        }
    }

    static LRESULT CALLBACK ClipboardWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_CREATE) {
            AddClipboardFormatListener(hwnd);
            return 0;
        }

        if (message == WM_DESTROY) {
            RemoveClipboardFormatListener(hwnd);
            PostQuitMessage(0);
            return 0;
        }

        if (message == WM_CLIPBOARDUPDATE) {
            PlatformInputWin* self = reinterpret_cast<PlatformInputWin*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self) {
                self->HandleClipboardUpdate();
            }
            return 0;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void HandleClipboardUpdate() {
        std::vector<std::string> files;
        std::string text;

        if (OpenClipboard(nullptr)) {
            if (IsClipboardFormatAvailable(CF_HDROP)) {
                HANDLE hData = GetClipboardData(CF_HDROP);
                if (hData) {
                    HDROP hDrop = static_cast<HDROP>(hData);
                    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                    for (UINT i = 0; i < count; ++i) {
                        UINT len = DragQueryFileW(hDrop, i, nullptr, 0) + 1;
                        std::wstring path(len, L'\0');
                        DragQueryFileW(hDrop, i, path.data(), len);
                        if (!path.empty() && path.back() == L'\0') {
                            path.pop_back();
                        }
                        files.push_back(WideToUtf8(path));
                    }
                }
            }

            if (files.empty() && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    wchar_t* wstr = static_cast<wchar_t*>(GlobalLock(hData));
                    if (wstr) {
                        text = WideToUtf8(wstr);
                        GlobalUnlock(hData);
                    }
                }
            }
            CloseClipboard();
        }

        ClipboardChangeCallback callbackCopy;
        {
            std::lock_guard<std::mutex> lock(m_clipboardMutex);
            callbackCopy = m_clipboardCallback;
        }

        if (!callbackCopy) return;

        if (!files.empty()) {
            callbackCopy("files", files, std::string());
        } else if (!text.empty()) {
            callbackCopy("text", {}, text);
        }
    }

    void RunClipboardMessageLoop() {
        std::wstring className = L"InputBridgeClipboardListenerWindow_" + std::to_wstring(reinterpret_cast<uintptr_t>(this));
        WNDCLASSEXW wc = { 0 };
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ClipboardWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className.c_str();

        if (!RegisterClassExW(&wc)) {
            std::lock_guard<std::mutex> lock(m_clipboardMutex);
            m_clipboardThreadRunning = false;
            m_clipboardReady.notify_all();
            return;
        }

        HWND hwnd = CreateWindowExW(0, className.c_str(), L"InputBridgeClipboardListener", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!hwnd) {
            UnregisterClassW(className.c_str(), wc.hInstance);
            std::lock_guard<std::mutex> lock(m_clipboardMutex);
            m_clipboardThreadRunning = false;
            m_clipboardReady.notify_all();
            return;
        }

        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        {
            std::lock_guard<std::mutex> lock(m_clipboardMutex);
            m_clipboardWindow = hwnd;
            m_clipboardReady.notify_all();
        }

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        DestroyWindow(hwnd);
        UnregisterClassW(className.c_str(), wc.hInstance);

        {
            std::lock_guard<std::mutex> lock(m_clipboardMutex);
            m_clipboardWindow = nullptr;
            m_clipboardThreadRunning = false;
        }
    }

    void EnsureClipboardThread() {
        bool expected = false;
        if (!m_clipboardThreadRunning.compare_exchange_strong(expected, true)) {
            return;
        }

        m_clipboardThread = std::thread([this]() {
            RunClipboardMessageLoop();
            });

        std::unique_lock<std::mutex> lock(m_clipboardMutex);
        m_clipboardReady.wait(lock, [this] { return m_clipboardWindow != nullptr || !m_clipboardThreadRunning.load(); });
    }

    void StopClipboardThread() {
        HWND hwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_clipboardMutex);
            hwnd = m_clipboardWindow;
        }
        if (hwnd) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        if (m_clipboardThread.joinable()) {
            m_clipboardThread.join();
        }
    }

    public:
    PlatformInputWin() {
        // Reserve memory up front to prevent frequent allocations on every flush
        m_winInputs.reserve(1024);
    }

    bool Initialize(std::string& error_msg) override {
        // Windows input injection generally does not require session authorization
        return true;
    }

    void SetClipboardChangeCallback(ClipboardChangeCallback cb) override {
        {
            std::lock_guard<std::mutex> lock(m_clipboardMutex);
            m_clipboardCallback = std::move(cb);
        }
        if (m_clipboardCallback) {
            EnsureClipboardThread();
        }
    }

    ~PlatformInputWin() override {
        StopClipboardThread();
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

    // Clipboard: Set remote files (CFSTR_FILEDESCRIPTORW / CFSTR_FILECONTENTS)
    bool SetClipboardFilesRemote(const std::vector<std::string>& filePaths) override {
        std::vector<std::wstring> fullPaths;
        std::vector<std::wstring> fileNames;
        std::vector<std::vector<uint8_t>> fileContents;

        for (const auto& path : filePaths) {
            std::wstring widePath = Utf8ToWide(path);
            if (widePath.empty()) return false;
            std::vector<uint8_t> buffer;
            if (!ReadFileToBytes(widePath, buffer)) return false;
            fullPaths.push_back(std::move(widePath));
            fileNames.push_back(GetFileName(fullPaths.back()));
            fileContents.push_back(std::move(buffer));
        }

        HRESULT hr = OleInitialize(nullptr);
        if (FAILED(hr)) return false;

        RemoteFileDataObject* dataObject = new RemoteFileDataObject(std::move(fullPaths), std::move(fileNames), std::move(fileContents));
        if (!dataObject) {
            OleUninitialize();
            return false;
        }

        HRESULT setHr = OleSetClipboard(dataObject);
        if (SUCCEEDED(setHr)) {
            setHr = OleFlushClipboard();
        }
        dataObject->Release();
        OleUninitialize();
        return SUCCEEDED(setHr);
    }

    // Clipboard: Get remote file names from a redirected clipboard object
    std::optional<std::vector<std::string>> GetClipboardFilesRemote() override {
        HRESULT hr = OleInitialize(nullptr);
        if (FAILED(hr)) return std::nullopt;

        IDataObject* dataObject = nullptr;
        hr = OleGetClipboard(&dataObject);
        if (FAILED(hr) || !dataObject) {
            OleUninitialize();
            return std::nullopt;
        }

        UINT cfFileDescriptor = RegisterClipboardFormat(CFSTR_FILEDESCRIPTORW);
        FORMATETC fmt = { static_cast<CLIPFORMAT>(cfFileDescriptor), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medium = { 0 };
        std::optional<std::vector<std::string>> result;

        if (SUCCEEDED(dataObject->GetData(&fmt, &medium))) {
            FILEGROUPDESCRIPTORW* groupDesc = reinterpret_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
            if (groupDesc) {
                result.emplace();
                for (UINT i = 0; i < groupDesc->cItems; ++i) {
                    FILEDESCRIPTORW& fileDesc = groupDesc->fgd[i];
                    std::wstring name(fileDesc.cFileName);
                    int utf8len = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, nullptr, 0, nullptr, nullptr);
                    std::string utf8Name(utf8len - 1, 0);
                    WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, utf8Name.data(), utf8len, nullptr, nullptr);
                    result->push_back(std::move(utf8Name));
                }
                GlobalUnlock(medium.hGlobal);
            }
            ReleaseStgMedium(&medium);
        }

        dataObject->Release();
        OleUninitialize();
        return result;
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

    bool StartInputDetection() override {
        bool expected = false;
        if (!m_inputDetectionRunning.compare_exchange_strong(expected, true)) {
            return true; // Already running
        }

        // Reset positions to ensure the first movement is always captured
        m_lastDetectedMouseX.store(-1, std::memory_order_relaxed);
        m_lastDetectedMouseY.store(-1, std::memory_order_relaxed);

        m_inputDetectionThread = std::jthread([this]() {
            RunInputDetectionLoop();
            });

        std::unique_lock<std::mutex> lock(m_inputDetectionMutex);
        m_inputDetectionReady.wait(lock, [this]() {
            return (m_kbHook != nullptr && m_msHook != nullptr) || !m_inputDetectionRunning.load();
            });

        return m_kbHook != nullptr && m_msHook != nullptr;
    }

    void StopInputDetection() override {
        bool expected = true;
        if (m_inputDetectionRunning.compare_exchange_strong(expected, false)) {
            DWORD threadId = 0;
            {
                std::lock_guard<std::mutex> lock(m_inputDetectionMutex);
                threadId = m_inputDetectionThreadId;
            }
            if (threadId != 0) {
                PostThreadMessage(threadId, WM_QUIT, 0, 0);
            }

            if (m_inputDetectionThread.joinable()) {
                m_inputDetectionThread.join();
            }
        }
    }

    void SetInputEventCallback(InputEventCallback cb) override {
        m_inputCallback = cb;
    }

    void SetDetectionOptimizationThreshold(int distanceThreshold) override {
        m_detectionDistanceThreshold.store(distanceThreshold, std::memory_order_relaxed);
    }
};

PlatformInputWin* PlatformInputWin::s_instance = nullptr;
