#include <napi.h>

#include <exception>
#include <memory>
#include <string>
#include "platform_input.hpp"

class InputBridge : public Napi::ObjectWrap<InputBridge> {
    public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "InputBridge", {
            InstanceMethod("moveMouse", &InputBridge::MoveMouse),
            InstanceMethod("mouseClick", &InputBridge::MouseClick),
            InstanceMethod("keyPress", &InputBridge::KeyPress),
            InstanceMethod("scrollMouse", &InputBridge::ScrollMouse),
            InstanceMethod("setLogger", &InputBridge::SetLogger)
            });

        auto* constructor = new Napi::FunctionReference();
        *constructor = Napi::Persistent(func);
        env.SetInstanceData(constructor);

        exports.Set("InputBridge", func);
        return exports;
    }

    explicit InputBridge(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<InputBridge>(info),
        m_backend(CreatePlatformInput()) {
    }

    private:
    std::unique_ptr<IPlatformInput> m_backend;
    Napi::FunctionReference m_logger;

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

        m_backend->SetLogCallback([this](const std::string& msg) {
            this->Log(msg);
            });

        return info.Env().Undefined();
    }

    Napi::Value MoveMouse(const Napi::CallbackInfo& info) {
        if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
            Napi::TypeError::New(info.Env(), "Expected x and y as numbers").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        int32_t x = info[0].As<Napi::Number>().Int32Value();
        int32_t y = info[1].As<Napi::Number>().Int32Value();
        m_backend->MoveMouse(x, y);
        return info.Env().Undefined();
    }

    Napi::Value MouseClick(const Napi::CallbackInfo& info) {
        if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsBoolean()) {
            Napi::TypeError::New(info.Env(), "Expected button as number and down as boolean").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        int32_t button = info[0].As<Napi::Number>().Int32Value();
        bool down = info[1].As<Napi::Boolean>().Value();
        m_backend->MouseClick(button, down);
        return info.Env().Undefined();
    }

    Napi::Value KeyPress(const Napi::CallbackInfo& info) {
        if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsBoolean()) {
            Napi::TypeError::New(info.Env(), "Expected keyCode as number and down as boolean").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        int32_t keyCode = info[0].As<Napi::Number>().Int32Value();
        bool down = info[1].As<Napi::Boolean>().Value();
        m_backend->KeyPress(keyCode, down);
        return info.Env().Undefined();
    }

    Napi::Value ScrollMouse(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsNumber()) {
            Napi::TypeError::New(info.Env(), "Expected delta as number").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        int32_t delta = info[0].As<Napi::Number>().Int32Value();
        m_backend->ScrollMouse(delta);
        return info.Env().Undefined();
    }
};

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    return InputBridge::Init(env, exports);
}

NODE_API_MODULE(input_bridge_addon, InitAll)
