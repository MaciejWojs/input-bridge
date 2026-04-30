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
#include "linux/platform_input_linux.cpp"
typedef PlatformInputLinux PlatformInputImpl;
#endif
#else
#include "platform_input_stub.cpp"
typedef PlatformInputStub PlatformInputImpl;
#endif

std::unique_ptr<IPlatformInput> CreatePlatformInput() {
    return std::make_unique<PlatformInputImpl>();
}

class InputBridge : public Napi::ObjectWrap<InputBridge> {
    public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "InputBridge", {
            InstanceMethod("init", &InputBridge::InitAsync),
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
            InstanceMethod("setClipboardText", &InputBridge::SetClipboardText),
            InstanceMethod("getClipboardText", &InputBridge::GetClipboardText),
            InstanceMethod("setClipboardFiles", &InputBridge::SetClipboardFiles),
            InstanceMethod("getClipboardFiles", &InputBridge::GetClipboardFiles),
            InstanceMethod("setClipboardFilesRemote", &InputBridge::SetClipboardFilesRemote),
            InstanceMethod("getClipboardFilesRemote", &InputBridge::GetClipboardFilesRemote)
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

    // Clipboard: setClipboardFilesRemote(paths: string[]): boolean
    Napi::Value SetClipboardFilesRemote(const Napi::CallbackInfo& info) {
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
        bool ok = m_queue.GetPlatform()->SetClipboardFilesRemote(paths);
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
    };

    explicit InputBridge(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<InputBridge>(info),
        m_queue(CreatePlatformInput()) {
    }

    ~InputBridge() {
        if (m_queue.GetPlatform()) {
            m_queue.GetPlatform()->SetClipboardChangeCallback({});
        }
        if (m_clipboardTsfn) {
            m_clipboardTsfn.Release();
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
            m_queue.GetPlatform()->SetClipboardChangeCallback([this](const std::string& type, const std::vector<std::string>& files, const std::string& text) {
                if (!m_clipboardTsfn) return;

                auto* eventData = new ClipboardEventData{ type, files, text };
                napi_status status = m_clipboardTsfn.BlockingCall(eventData, [](Napi::Env env, Napi::Function jsCallback, ClipboardEventData* data) {
                    Napi::Object event = Napi::Object::New(env);
                    event.Set("type", data->type);
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

    Napi::Value InitAsync(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);

        InitWorker* worker = new InitWorker(deferred, m_queue.GetPlatform());
        worker->Queue();

        return deferred.Promise();
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
    return InputBridge::Init(env, exports);
}

NODE_API_MODULE(input_bridge_addon, InitAll)
