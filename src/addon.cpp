#include <napi.h>

#include <exception>
#include <memory>
#include <string>
#include "platform_input.hpp"
#include <array>
#include <cstdint>
#include <iostream>

// Compile-time FNV-1a 64-bit hash and hex formatter to produce a stable build token.
constexpr uint64_t fnv1a64(const char* s, size_t n) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint8_t>(s[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

template<size_t N>
constexpr std::array<char, 17> hex16(uint64_t v) {
    std::array<char, 17> out = {};
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        out[15 - i] = hex[v & 0xF];
        v >>= 4;
    }
    out[16] = '\0';
    return out;
}

// Token source combines compile-time strings so the token changes when source/time/file changes.
#define REV_SOURCE __FILE__ " " __DATE__ " " __TIME__
constexpr uint64_t revision_hash = fnv1a64(REV_SOURCE, sizeof(REV_SOURCE) - 1);
constexpr auto revision_token = hex16<16>(revision_hash);

#if defined(_WIN32)
#include "win/platform_input_win.cpp"
typedef PlatformInputWin PlatformInputImpl;
#elif defined(__linux__)
#ifdef USE_X11_BACKEND
#include "linux/platform_input_x11.cpp"
typedef X11PlatformInput PlatformInputImpl;
#else
#include "linux/linux_platform_factory.hpp"
#endif
#else
#include "platform_input_stub.cpp"
typedef PlatformInputStub PlatformInputImpl;
#endif

#if defined(_WIN32)
#include "cursor/cursor_win.cpp"
#elif defined(__linux__)
#include "cursor/cursor_linux_dispatch.cpp"
#else
#include "cursor/cursor_stub.cpp"
#endif
#include "cursor/cursor_exports.cpp"

std::unique_ptr<IPlatformInput> CreatePlatformInput() {
#if defined(__linux__) && !defined(USE_X11_BACKEND)
    return CreateLinuxPlatformInputFromFactory();
#else
    return std::make_unique<PlatformInputImpl>();
#endif
}

static Napi::Object MonitorInfoToJs(Napi::Env env, const MonitorInfo& monitor) {
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("index", Napi::Number::New(env, monitor.index));
    obj.Set("id", Napi::String::New(env, monitor.id));
    obj.Set("name", Napi::String::New(env, monitor.name));
    obj.Set("x", Napi::Number::New(env, monitor.x));
    obj.Set("y", Napi::Number::New(env, monitor.y));
    obj.Set("width", Napi::Number::New(env, monitor.width));
    obj.Set("height", Napi::Number::New(env, monitor.height));
    obj.Set("primary", Napi::Boolean::New(env, monitor.primary));
    return obj;
}

class InputBridge : public Napi::ObjectWrap<InputBridge> {
    public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "InputBridge", {
            InstanceMethod("init", &InputBridge::InitAsync),
            InstanceMethod("getMonitors", &InputBridge::GetMonitors),
            InstanceMethod("setMonitors", &InputBridge::SetMonitors),
            InstanceMethod("setCurrentMonitor", &InputBridge::SetCurrentMonitor),
            InstanceMethod("moveMouseRelative", &InputBridge::MoveMouseRelative),
            InstanceMethod("moveMouseAbsolute", &InputBridge::MoveMouseAbsolute),
            InstanceMethod("mouseClick", &InputBridge::MouseClick),
            InstanceMethod("keyPress", &InputBridge::KeyPress),
            InstanceMethod("scrollMouse", &InputBridge::ScrollMouse),
            InstanceMethod("typeString", &InputBridge::TypeString),
            InstanceMethod("optimizeMouseMovesRelative", &InputBridge::OptimizeMouseMovesRelative),
            InstanceMethod("optimizeMouseMovesAbsolute", &InputBridge::OptimizeMouseMovesAbsolute),
            InstanceMethod("toggleOptimization", &InputBridge::ToggleOptimization),
            InstanceMethod("flush", &InputBridge::Flush),
            InstanceMethod("setLogger", &InputBridge::SetLogger),
            InstanceMethod("onClipboard", &InputBridge::OnClipboard),
            InstanceMethod("offClipboard", &InputBridge::OffClipboard),
            InstanceMethod("onInput", &InputBridge::OnInput),
            InstanceMethod("offInput", &InputBridge::OffInput),
            InstanceMethod("startInputDetection", &InputBridge::StartInputDetection),
            InstanceMethod("stopInputDetection", &InputBridge::StopInputDetection),
            InstanceMethod("optimizeInputDetection", &InputBridge::OptimizeInputDetection),
            InstanceMethod("setClipboardText", &InputBridge::SetClipboardText),
            InstanceMethod("getClipboardText", &InputBridge::GetClipboardText),
            InstanceMethod("setClipboardFiles", &InputBridge::SetClipboardFiles),
            InstanceMethod("getClipboardFiles", &InputBridge::GetClipboardFiles),
            InstanceMethod("setClipboardFilesRemote", &InputBridge::SetClipboardFilesRemote),
            InstanceMethod("getClipboardFilesRemote", &InputBridge::GetClipboardFilesRemote),
            InstanceMethod("setInputMode", &InputBridge::SetInputMode),
            InstanceMethod("getInputMode", &InputBridge::GetInputMode),
            InstanceMethod("setBackendMethods", &InputBridge::SetBackendMethods),
            InstanceMethod("getBackendMethods", &InputBridge::GetBackendMethods),
            InstanceMethod("connectToEIS", &InputBridge::ConnectToEIS),
            InstanceMethod("disconnectEIS", &InputBridge::DisconnectEIS),
            InstanceMethod("isEISConnected", &InputBridge::IsEISConnected),
            InstanceMethod("getPortalSessionHandle", &InputBridge::GetPortalSessionHandle),
            InstanceMethod("openPipeWireRemoteFd", &InputBridge::OpenPipeWireRemoteFd)
            });

        auto* constructor = new Napi::FunctionReference();
        *constructor = Napi::Persistent(func);
        env.SetInstanceData(constructor);

        exports.Set("InputBridge", func);
        return exports;
    }

    // Clipboard: setClipboardText(text: string): boolean
    Napi::Value SetClipboardText(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(info.Env(), "Expected text as string").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        std::string text = info[0].As<Napi::String>().Utf8Value();
        bool ok = m_queue.GetPlatform()->SetClipboardText(text);
        return Napi::Boolean::New(info.Env(), ok);
    }

    // Clipboard: getClipboardText(): string | null
    Napi::Value GetClipboardText(const Napi::CallbackInfo& info) {
        auto result = m_queue.GetPlatform()->GetClipboardText();
        if (!result) return info.Env().Null();
        return Napi::String::New(info.Env(), *result);
    }

    // Clipboard: setClipboardFiles(paths: string[]): boolean
    Napi::Value SetClipboardFiles(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsArray()) {
            Napi::TypeError::New(info.Env(), "Expected array of file paths").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        Napi::Array arr = info[0].As<Napi::Array>();
        std::vector<std::string> paths;
        for (uint32_t i = 0; i < arr.Length(); ++i) {
            Napi::Value v = arr[i];
            if (!v.IsString()) continue;
            paths.push_back(v.As<Napi::String>().Utf8Value());
        }
        bool ok = m_queue.GetPlatform()->SetClipboardFiles(paths);
        return Napi::Boolean::New(info.Env(), ok);
    }

    // Clipboard: getClipboardFiles(): string[] | null
    Napi::Value GetClipboardFiles(const Napi::CallbackInfo& info) {
        auto result = m_queue.GetPlatform()->GetClipboardFiles();
        if (!result) return info.Env().Null();
        Napi::Array arr = Napi::Array::New(info.Env(), result->size());
        for (size_t i = 0; i < result->size(); ++i) {
            arr[i] = Napi::String::New(info.Env(), (*result)[i]);
        }
        return arr;
    }

    // Clipboard: setClipboardFilesRemote(files: { fileName: string, data: Buffer | Uint8Array }[]): boolean
    Napi::Value SetClipboardFilesRemote(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsArray()) {
            Napi::TypeError::New(info.Env(), "Expected array of { fileName: string, data: Buffer | Uint8Array }").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        Napi::Array arr = info[0].As<Napi::Array>();
        std::vector<ClipboardRemoteFileEntry> entries;
        entries.reserve(arr.Length());
        for (uint32_t i = 0; i < arr.Length(); ++i) {
            Napi::Value v = arr[i];
            if (!v.IsObject()) {
                continue;
            }
            Napi::Object o = v.As<Napi::Object>();
            if (!o.Has("fileName") || !o.Get("fileName").IsString()) {
                continue;
            }
            if (!o.Has("data")) {
                continue;
            }
            std::string fileName = o.Get("fileName").As<Napi::String>().Utf8Value();
            Napi::Value dataVal = o.Get("data");
            std::vector<uint8_t> bytes;
            if (dataVal.IsBuffer()) {
                Napi::Buffer<uint8_t> buf = dataVal.As<Napi::Buffer<uint8_t>>();
                bytes.assign(buf.Data(), buf.Data() + buf.Length());
            } else if (dataVal.IsTypedArray()) {
                Napi::TypedArray ta = dataVal.As<Napi::TypedArray>();
                if (ta.TypedArrayType() != napi_uint8_array) {
                    Napi::TypeError::New(info.Env(), "Each entry.data must be Buffer or Uint8Array").ThrowAsJavaScriptException();
                    return info.Env().Undefined();
                }
                Napi::Uint8Array ua = ta.As<Napi::Uint8Array>();
                bytes.assign(ua.Data(), ua.Data() + ua.ByteLength());
            } else {
                Napi::TypeError::New(info.Env(), "Each entry.data must be Buffer or Uint8Array").ThrowAsJavaScriptException();
                return info.Env().Undefined();
            }
            entries.push_back(ClipboardRemoteFileEntry{std::move(fileName), std::move(bytes)});
        }
        if (entries.empty()) {
            return Napi::Boolean::New(info.Env(), false);
        }
        bool ok = m_queue.GetPlatform()->SetClipboardFilesRemote(entries);
        return Napi::Boolean::New(info.Env(), ok);
    }

    // Clipboard: getClipboardFilesRemote(): string[] | null
    Napi::Value GetClipboardFilesRemote(const Napi::CallbackInfo& info) {
        auto result = m_queue.GetPlatform()->GetClipboardFilesRemote();
        if (!result) return info.Env().Null();
        Napi::Array arr = Napi::Array::New(info.Env(), result->size());
        for (size_t i = 0; i < result->size(); ++i) {
            arr[i] = Napi::String::New(info.Env(), (*result)[i]);
        }
        return arr;
    }

    Napi::Value SetInputMode(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(info.Env(), "Expected mode as string ('notify' or 'eis')").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }

        std::string mode = info[0].As<Napi::String>().Utf8Value();
        std::string error_msg;
        bool ok = m_queue.GetPlatform()->SetInputMode(mode, error_msg);
        if (!ok && !error_msg.empty()) {
            Napi::Error::New(info.Env(), error_msg).ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }

        return Napi::Boolean::New(info.Env(), ok);
    }

    Napi::Value GetInputMode(const Napi::CallbackInfo& info) {
        return Napi::String::New(info.Env(), m_queue.GetPlatform()->GetInputMode());
    }

    Napi::Value SetBackendMethods(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsObject()) {
            Napi::TypeError::New(info.Env(), "Expected options object").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }

        Napi::Object options = info[0].As<Napi::Object>();
        BackendMethods methods;

        if (options.Has("keyboardMethod")) {
            Napi::Value keyboardMethodValue = options.Get("keyboardMethod");
            if (!keyboardMethodValue.IsString()) {
                Napi::TypeError::New(info.Env(), "Expected keyboardMethod as 'eis' or 'fallback'").ThrowAsJavaScriptException();
                return info.Env().Undefined();
            }

            const std::string keyboardMethod = keyboardMethodValue.As<Napi::String>().Utf8Value();
            if (keyboardMethod == "eis") {
                methods.keyboardMethod = KeyboardMethod::EIS;
            } else if (keyboardMethod == "fallback") {
                methods.keyboardMethod = KeyboardMethod::Fallback;
            } else {
                Napi::Error::New(info.Env(), "Unsupported keyboardMethod. Use 'eis' or 'fallback'.").ThrowAsJavaScriptException();
                return info.Env().Undefined();
            }
        }

        if (options.Has("allowNotifyKeyboard")) {
            Napi::Value allowNotifyKeyboardValue = options.Get("allowNotifyKeyboard");
            if (!allowNotifyKeyboardValue.IsBoolean()) {
                Napi::TypeError::New(info.Env(), "Expected allowNotifyKeyboard as boolean").ThrowAsJavaScriptException();
                return info.Env().Undefined();
            }
            methods.allowNotifyKeyboard = allowNotifyKeyboardValue.As<Napi::Boolean>().Value();
        }

        if (options.Has("allowNotifyPointer")) {
            Napi::Value allowNotifyPointerValue = options.Get("allowNotifyPointer");
            if (!allowNotifyPointerValue.IsBoolean()) {
                Napi::TypeError::New(info.Env(), "Expected allowNotifyPointer as boolean").ThrowAsJavaScriptException();
                return info.Env().Undefined();
            }
            methods.allowNotifyPointer = allowNotifyPointerValue.As<Napi::Boolean>().Value();
        }

        std::string error_msg;
        bool ok = m_queue.GetPlatform()->SetBackendMethods(methods, error_msg);
        if (!ok && !error_msg.empty()) {
            Napi::Error::New(info.Env(), error_msg).ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }

        return Napi::Boolean::New(info.Env(), ok);
    }

    Napi::Value GetBackendMethods(const Napi::CallbackInfo& info) {
        (void)info;
        const BackendMethods methods = m_queue.GetPlatform()->GetBackendMethods();
        Napi::Object options = Napi::Object::New(info.Env());
        options.Set("keyboardMethod", methods.keyboardMethod == KeyboardMethod::EIS ? "eis" : "fallback");
        options.Set("allowNotifyKeyboard", Napi::Boolean::New(info.Env(), methods.allowNotifyKeyboard));
        options.Set("allowNotifyPointer", Napi::Boolean::New(info.Env(), methods.allowNotifyPointer));
        return options;
    }

    Napi::Value ConnectToEIS(const Napi::CallbackInfo& info) {
        std::string error_msg;
        bool ok = m_queue.GetPlatform()->ConnectToEIS(error_msg);
        if (!ok && !error_msg.empty()) {
            Napi::Error::New(info.Env(), error_msg).ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        return Napi::Boolean::New(info.Env(), ok);
    }

    Napi::Value DisconnectEIS(const Napi::CallbackInfo& info) {
        m_queue.GetPlatform()->DisconnectEIS();
        return info.Env().Undefined();
    }

    Napi::Value IsEISConnected(const Napi::CallbackInfo& info) {
        return Napi::Boolean::New(info.Env(), m_queue.GetPlatform()->IsEISConnected());
    }

    Napi::Value GetPortalSessionHandle(const Napi::CallbackInfo& info) {
        auto session = m_queue.GetPlatform()->GetPortalSessionHandle();
        if (!session) return info.Env().Null();
        return Napi::String::New(info.Env(), *session);
    }

    Napi::Value OpenPipeWireRemoteFd(const Napi::CallbackInfo& info) {
        std::string error_msg;
        auto fd = m_queue.GetPlatform()->OpenPipeWireRemoteFd(error_msg);
        if (!fd) {
            if (!error_msg.empty()) {
                if (error_msg == "PipeWire remote fd is available only with portal backend.") {
                    return info.Env().Null();
                }
                Napi::Error::New(info.Env(), error_msg).ThrowAsJavaScriptException();
                return info.Env().Undefined();
            }
            return info.Env().Null();
        }
        return Napi::Number::New(info.Env(), *fd);
    }

    class InitWorker : public Napi::AsyncWorker {
        public:
        InitWorker(Napi::Promise::Deferred deferred, IPlatformInput* platform)
            : Napi::AsyncWorker(deferred.Env()), m_deferred(deferred), m_platform(platform), m_success(false) {}

        ~InitWorker() {}

        void Execute() override {
            if (m_platform) {
                m_success = m_platform->Initialize(m_errorMsg);
            } else {
                m_errorMsg = "Platform input instance is null";
                m_success = false;
            }
        }

        void OnOK() override {
            if (m_success) {
                m_deferred.Resolve(Env().Undefined());
            } else {
                m_deferred.Reject(Napi::Error::New(Env(), m_errorMsg).Value());
            }
        }

        void OnError(const Napi::Error& e) override {
            m_deferred.Reject(e.Value());
        }

        private:
        Napi::Promise::Deferred m_deferred;
        IPlatformInput* m_platform;
        bool m_success;
        std::string m_errorMsg;
    };

    struct ClipboardEventData {
        std::string type;
        std::vector<std::string> files;
        std::string text;
        int64_t timestampMs = 0;
    };

    struct InputEventData {
        std::string type;
        std::vector<std::string> _pad_files;
        std::string _pad_text;
        int32_t x = 0;
        int32_t y = 0;
        int32_t button = 0;
        bool down = false;
        int32_t keyCode = 0;
        uint32_t charCode = 0;
        int32_t delta = 0;
        std::string domCode = "";
    };

    explicit InputBridge(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<InputBridge>(info),
        m_queue(CreatePlatformInput()) {
    }

    ~InputBridge() {
        if (m_queue.GetPlatform()) {
            m_queue.GetPlatform()->SetClipboardChangeCallback({});
            m_queue.GetPlatform()->SetInputEventCallback({});
        }
        if (m_clipboardTsfn) {
            m_clipboardTsfn.Release();
        }
        if (m_inputTsfn) {
            m_inputTsfn.Release();
        }
    }

    private:
    static InputRoute DetectInputMode(bool isTextEvent, bool hasShortcutModifiers) {
        if (isTextEvent) {
            return InputRoute::Unicode;
        }

        if (hasShortcutModifiers) {
            return InputRoute::Keyboard;
        }

        return InputRoute::Keyboard;
    }

    InputQueue m_queue;
    Napi::FunctionReference m_logger;
    Napi::ThreadSafeFunction m_clipboardTsfn;
    Napi::ThreadSafeFunction m_inputTsfn;

    Napi::Value OnClipboard(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsFunction()) {
            Napi::TypeError::New(info.Env(), "Expected a callback function").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }

        if (m_clipboardTsfn) {
            m_clipboardTsfn.Release();
            m_clipboardTsfn = Napi::ThreadSafeFunction();
        }

        m_clipboardTsfn = Napi::ThreadSafeFunction::New(
            info.Env(),
            info[0].As<Napi::Function>(),
            "ClipboardEvent",
            0,
            1
        );

        if (m_queue.GetPlatform()) {
            m_queue.GetPlatform()->SetClipboardChangeCallback([this](const std::string& type, const std::vector<std::string>& files, const std::string& text, int64_t timestampMs) {
                if (!m_clipboardTsfn) return;

                auto* eventData = new ClipboardEventData{ type, files, text, timestampMs };
                napi_status status = m_clipboardTsfn.BlockingCall(eventData, [](Napi::Env env, Napi::Function jsCallback, ClipboardEventData* data) {
                    Napi::Object event = Napi::Object::New(env);
                    event.Set("type", data->type);
                    event.Set("timestamp", Napi::Number::New(env, static_cast<double>(data->timestampMs)));
                    if (data->type == "files") {
                        Napi::Array arr = Napi::Array::New(env, data->files.size());
                        for (size_t i = 0; i < data->files.size(); ++i) {
                            arr[i] = Napi::String::New(env, data->files[i]);
                        }
                        event.Set("data", arr);
                    } else {
                        event.Set("data", Napi::String::New(env, data->text));
                    }
                    jsCallback.Call({ event });
                    delete data;
                    });
                if (status != napi_ok) {
                    delete eventData;
                }
                });
        }

        return info.Env().Undefined();
    }

    Napi::Value OffClipboard(const Napi::CallbackInfo& info) {
        if (m_queue.GetPlatform()) {
            m_queue.GetPlatform()->SetClipboardChangeCallback({});
        }
        if (m_clipboardTsfn) {
            m_clipboardTsfn.Release();
            m_clipboardTsfn = Napi::ThreadSafeFunction();
        }
        return info.Env().Undefined();
    }

    Napi::Value OnInput(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsFunction()) {
            Napi::TypeError::New(info.Env(), "Expected a callback function").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }

        if (m_inputTsfn) {
            m_inputTsfn.Release();
            m_inputTsfn = Napi::ThreadSafeFunction();
        }

        m_inputTsfn = Napi::ThreadSafeFunction::New(
            info.Env(),
            info[0].As<Napi::Function>(),
            "InputEvent",
            0,
            1
        );

        if (m_queue.GetPlatform()) {
            m_queue.GetPlatform()->SetInputEventCallback([this](const InputEvent& ev) {
                if (!m_inputTsfn) return;

                auto* data = new InputEventData();

                if (auto p = std::get_if<::MouseMoveRelative>(&ev)) {
                    data->type = "mouse_move_relative";
                    data->x = p->x; data->y = p->y;
                } else if (auto p = std::get_if<::MouseMoveAbsolute>(&ev)) {
                    data->type = "mouse_move_absolute";
                    data->x = p->x; data->y = p->y;
                } else if (auto p = std::get_if<::MouseClick>(&ev)) {
                    data->type = "mouse_click";
                    data->button = p->button; data->down = p->down;
                } else if (auto p = std::get_if<::KeyPress>(&ev)) {
                    data->type = "key_press";
                    data->keyCode = p->keyCode; data->down = p->down;
                    // Note: We don't have domCode prop yet in InputEventData, let's add it.
                    data->domCode = p->domCode;
                } else if (auto p = std::get_if<::MouseScroll>(&ev)) {
                    data->type = "mouse_scroll";
                    data->delta = p->delta;
                } else if (auto p = std::get_if<::TypeCharacter>(&ev)) {
                    data->type = "type_character";
                    data->charCode = p->charCode;
                }

                napi_status status = m_inputTsfn.BlockingCall(data, [](Napi::Env env, Napi::Function jsCallback, InputEventData* data) {
                    Napi::Object ev = Napi::Object::New(env);
                    ev.Set("type", data->type);
                    if (data->type == "mouse_move_relative" || data->type == "mouse_move_absolute") {
                        ev.Set("x", Napi::Number::New(env, data->x));
                        ev.Set("y", Napi::Number::New(env, data->y));
                    } else if (data->type == "mouse_click") {
                        ev.Set("button", Napi::Number::New(env, data->button));
                        ev.Set("down", Napi::Boolean::New(env, data->down));
                    } else if (data->type == "key_press") {
                        ev.Set("keyCode", Napi::Number::New(env, data->keyCode));
                        ev.Set("down", Napi::Boolean::New(env, data->down));
                        if (!data->domCode.empty()) {
                            ev.Set("domCode", Napi::String::New(env, data->domCode));
                        }
                    } else if (data->type == "mouse_scroll") {
                        ev.Set("delta", Napi::Number::New(env, data->delta));
                    } else if (data->type == "type_character") {
                        ev.Set("charCode", Napi::Number::New(env, data->charCode));
                    }
                    jsCallback.Call({ ev });
                    delete data;
                    });

                if (status != napi_ok) {
                    delete data;
                }
                });
        }

        return info.Env().Undefined();
    }

    Napi::Value OffInput(const Napi::CallbackInfo& info) {
        if (m_queue.GetPlatform()) {
            m_queue.GetPlatform()->SetInputEventCallback({});
        }
        if (m_inputTsfn) {
            m_inputTsfn.Release();
            m_inputTsfn = Napi::ThreadSafeFunction();
        }
        return info.Env().Undefined();
    }

    Napi::Value StartInputDetection(const Napi::CallbackInfo& info) {
        if (m_queue.GetPlatform()) {
            bool ok = m_queue.GetPlatform()->StartInputDetection();
            return Napi::Boolean::New(info.Env(), ok);
        }
        return Napi::Boolean::New(info.Env(), false);
    }

    Napi::Value StopInputDetection(const Napi::CallbackInfo& info) {
        if (m_queue.GetPlatform()) {
            m_queue.GetPlatform()->StopInputDetection();
        }
        return info.Env().Undefined();
    }

    Napi::Value OptimizeInputDetection(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsNumber()) {
            Napi::TypeError::New(info.Env(), "Expected distanceThreshold as number").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        int threshold = info[0].As<Napi::Number>().Int32Value();
        if (m_queue.GetPlatform()) {
            m_queue.GetPlatform()->SetDetectionOptimizationThreshold(threshold);
        }
        return info.Env().Undefined();
    }

    Napi::Value InitAsync(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);

        InitWorker* worker = new InitWorker(deferred, m_queue.GetPlatform());
        worker->Queue();

        return deferred.Promise();
    }

    Napi::Value GetMonitors(const Napi::CallbackInfo& info) {
        (void)info;
        Napi::Env env = info.Env();
        const std::vector<MonitorInfo> monitors = m_queue.GetPlatform()->GetMonitors();
        Napi::Array result = Napi::Array::New(env, monitors.size());
        for (size_t i = 0; i < monitors.size(); ++i) {
            result[i] = MonitorInfoToJs(env, monitors[i]);
        }
        return result;
    }

    Napi::Value SetMonitors(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsArray()) {
            Napi::TypeError::New(info.Env(), "Expected array of MonitorMetadata").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }

        Napi::Array arr = info[0].As<Napi::Array>();
        std::vector<MonitorInfo> monitors;
        monitors.reserve(arr.Length());

        for (uint32_t i = 0; i < arr.Length(); ++i) {
            Napi::Value v = arr[i];
            if (!v.IsObject()) continue;

            Napi::Object obj = v.As<Napi::Object>();
            MonitorInfo m;
            m.index = obj.Has("index") ? obj.Get("index").As<Napi::Number>().Int32Value() : 0;
            m.id = obj.Has("id") ? obj.Get("id").As<Napi::String>().Utf8Value() : "";
            m.name = obj.Has("name") ? obj.Get("name").As<Napi::String>().Utf8Value() : "";
            m.x = obj.Has("x") ? obj.Get("x").As<Napi::Number>().Int32Value() : 0;
            m.y = obj.Has("y") ? obj.Get("y").As<Napi::Number>().Int32Value() : 0;
            m.width = obj.Has("width") ? obj.Get("width").As<Napi::Number>().Int32Value() : 0;
            m.height = obj.Has("height") ? obj.Get("height").As<Napi::Number>().Int32Value() : 0;
            m.primary = obj.Has("primary") ? obj.Get("primary").As<Napi::Boolean>().Value() : false;
            monitors.push_back(std::move(m));
        }

        m_queue.GetPlatform()->SetMonitors(monitors);
        return info.Env().Undefined();
    }

    Napi::Value SetCurrentMonitor(const Napi::CallbackInfo& info) {
        if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
            Napi::TypeError::New(info.Env(), "Expected monitor index, width, and height as numbers").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }

        int32_t index = info[0].As<Napi::Number>().Int32Value();
        int32_t width = info[1].As<Napi::Number>().Int32Value();
        int32_t height = info[2].As<Napi::Number>().Int32Value();
        bool ok = m_queue.GetPlatform()->SetCurrentMonitor(index, width, height);
        return Napi::Boolean::New(info.Env(), ok);
    }

    void Log(const std::string& msg) {
        if (!m_logger.IsEmpty()) {
            m_logger.Call({ Napi::String::New(m_logger.Env(), msg) });
        }
    }

    Napi::Value SetLogger(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsFunction()) {
            Napi::TypeError::New(info.Env(), "Expected a logger function").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        m_logger = Napi::Persistent(info[0].As<Napi::Function>());

        if (m_queue.GetPlatform()) {
            m_queue.GetPlatform()->SetLogCallback([this](const std::string& msg) {
                this->Log(msg);
                });
        }

        return info.Env().Undefined();
    }

    Napi::Value MoveMouseRelative(const Napi::CallbackInfo& info) {
        if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
            Napi::TypeError::New(info.Env(), "Expected x and y as numbers").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        m_queue.QueueMouseMoveRelative(
            info[0].As<Napi::Number>().Int32Value(),
            info[1].As<Napi::Number>().Int32Value()
        );
        return info.Env().Undefined();
    }

    Napi::Value MoveMouseAbsolute(const Napi::CallbackInfo& info) {
        if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
            Napi::TypeError::New(info.Env(), "Expected x and y as numbers").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        m_queue.QueueMouseMoveAbsolute(
            info[0].As<Napi::Number>().Int32Value(),
            info[1].As<Napi::Number>().Int32Value()
        );
        return info.Env().Undefined();
    }

    Napi::Value MouseClick(const Napi::CallbackInfo& info) {
        if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsBoolean()) {
            Napi::TypeError::New(info.Env(), "Expected button as number and down as boolean").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        m_queue.QueueMouseClick(
            info[0].As<Napi::Number>().Int32Value(),
            info[1].As<Napi::Boolean>().Value()
        );
        return info.Env().Undefined();
    }

    Napi::Value KeyPress(const Napi::CallbackInfo& info) {
        if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsBoolean()) {
            Napi::TypeError::New(info.Env(), "Expected keyCode as number and down as boolean").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        (void)DetectInputMode(false, false);
        m_queue.QueueKeyPress(
            info[0].As<Napi::Number>().Int32Value(),
            info[1].As<Napi::Boolean>().Value()
        );
        return info.Env().Undefined();
    }

    Napi::Value ScrollMouse(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsNumber()) {
            Napi::TypeError::New(info.Env(), "Expected delta as number").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        m_queue.QueueScrollMouse(info[0].As<Napi::Number>().Int32Value());
        return info.Env().Undefined();
    }

    Napi::Value TypeString(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(info.Env(), "Expected string").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }

        (void)DetectInputMode(true, false);
        std::u16string str = info[0].As<Napi::String>().Utf16Value();
        for (size_t i = 0; i < str.length(); ) {
            char16_t c = str[i];
            uint32_t codepoint = c;
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < str.length()) { // high surrogate
                char16_t low = str[i + 1];
                if (low >= 0xDC00 && low <= 0xDFFF) { // low surrogate
                    codepoint = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                } else {
                    i++;
                }
            } else {
                i++;
            }
            m_queue.QueueTypeCharacter(codepoint);
        }
        return info.Env().Undefined();
    }

    Napi::Value OptimizeMouseMovesRelative(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsNumber()) {
            Napi::TypeError::New(info.Env(), "Expected distanceThreshold as number").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        m_queue.OptimizeMouseMovesRelative(info[0].As<Napi::Number>().Int32Value());
        return info.Env().Undefined();
    }

    Napi::Value OptimizeMouseMovesAbsolute(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsNumber()) {
            Napi::TypeError::New(info.Env(), "Expected distanceThreshold as number").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        m_queue.OptimizeMouseMovesAbsolute(info[0].As<Napi::Number>().Int32Value());
        return info.Env().Undefined();
    }

    Napi::Value ToggleOptimization(const Napi::CallbackInfo& info) {
        bool newState = m_queue.ToggleOptimization();
        return Napi::Boolean::New(info.Env(), newState);
    }

    Napi::Value Flush(const Napi::CallbackInfo& info) {
        m_queue.Flush();
        return info.Env().Undefined();
    }
};

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    // Print compile-time generated revision token on module load
    std::cout << "[InputBridge] revision: " << revision_token.data() << std::endl;
    InputBridge::Init(env, exports);
    cursor_exports::Register(env, exports);
    return exports;
}

NODE_API_MODULE(input_bridge_addon, InitAll)
