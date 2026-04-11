{
  "targets": [
    {
      "target_name": "input_bridge_addon",
      "sources": [
        "src/addon.cpp"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS"
      ],
      "cflags_cc": [
        "-std=c++20",
        "-fexceptions"
      ],
      "conditions": [
        ["OS=='win'", {
          "sources": [
            "src/win/platform_input_win.cpp"
          ],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "AdditionalOptions": [
                "/EHsc",
                "/std:c++20"
              ]
            }
          },
          "libraries": [
            "user32.lib"
          ]
        }],
        ["OS=='linux'", {
          "sources": [
            "src/linux/platform_input_linux.cpp"
          ],
          "cflags_cc": [
            "<!@(pkg-config --cflags gio-2.0 gio-unix-2.0 glib-2.0 gobject-2.0 xkbcommon)"
          ],
          "libraries": [
            "<!@(pkg-config --libs gio-2.0 gio-unix-2.0 glib-2.0 gobject-2.0 xkbcommon)"
          ],
        }],
        ["OS!='win' and OS!='linux'", {
          "sources": [
            "src/platform_input_stub.cpp"
          ]
        }]
      ]
    }
  ]
}
