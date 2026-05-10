#include "cursor_manager.hpp"

#include <napi.h>

namespace cursor_exports {

Napi::Value GetCursorTypeJs(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), GetSystemCursorCssName());
}

void Register(Napi::Env env, Napi::Object exports) {
    exports.Set(
        Napi::String::New(env, "getCursorType"),
        Napi::Function::New(env, GetCursorTypeJs, "getCursorType")
    );
}

} // namespace cursor_exports
