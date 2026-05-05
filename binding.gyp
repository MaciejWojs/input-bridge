{
  "targets": [
    {
      "target_name": "input_bridge_addon",
      "variables": {
        "use_x11_backend%": "<!(node -p \"process.env.use_x11_backend || 0\")"
      },
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
        ["OS=='linux' and use_x11_backend==1", {
          "defines": [
            "USE_X11_BACKEND"
          ],
          "libraries": [
            "-lX11",
            "-lXtst",
            "-lXrandr"
          ]
        }],

        ["OS=='linux' and use_x11_backend!=1", {
          "cflags_cc": [
            "<!@(pkg-config --cflags gio-2.0 gio-unix-2.0 glib-2.0 gobject-2.0 xkbcommon)"
          ],
          "libraries": [
            "<!@(pkg-config --libs gio-2.0 gio-unix-2.0 glib-2.0 gobject-2.0 xkbcommon)"
          ]
        }],

        ["OS=='win'", {
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

        ["OS!='win' and OS!='linux'", {
          "defines": [
            "USE_PLATFORM_STUB"
          ]
        }]
      ]
    }
  ]
}
