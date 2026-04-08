#include <napi.h>

#include <exception>
#include <memory>
#include <string>
#include "platform_input.hpp"

class InputBridge : public Napi::ObjectWrap<InputBridge> {
    public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "InputBridge", {
            InstanceMethod("moveMouseRelative", &InputBridge::MoveMouseRelative),
            InstanceMethod("moveMouseAbsolute", &InputBridge::MoveMouseAbsolute),
            InstanceMethod("mouseClick", &InputBridge::MouseClick),
            InstanceMethod("keyPress", &InputBridge::KeyPress),
            InstanceMethod("scrollMouse", &InputBridge::ScrollMouse),
            InstanceMethod("optimizeMouseMovesRelative", &InputBridge::OptimizeMouseMovesRelative),
            InstanceMethod("optimizeMouseMovesAbsolute", &InputBridge::OptimizeMouseMovesAbsolute),
            InstanceMethod("toggleOptimization", &InputBridge::ToggleOptimization),
            InstanceMethod("flush", &InputBridge::Flush),
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
        m_queue(CreatePlatformInput()) {
    }

    private:
    InputQueue m_queue;
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
    return InputBridge::Init(env, exports);
}

NODE_API_MODULE(input_bridge_addon, InitAll)
