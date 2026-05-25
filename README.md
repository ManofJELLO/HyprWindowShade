# HyprWindowShade

A Hyprland plugin that applies fragment shaders to individual windows (or layers) based on `hyprland.conf` window rules. Shaders are HyprShade-compatible — if it works in HyprShade, it should work here.

You can also use a `time` uniform for glitch-style animated effects.

> **`.lua` config users:** Plugin dispatchers aren't surfaced to the Lua config layer on Hyprland 0.55, so the plugin also exposes every action as a Lua function under `hl.plugin.HyprWindowShade.*`. See [Lua config](#lua-config) below for usage.

> This has not been stress-tested. It may break when Hyprland updates or simply not work on your system. Only tested on AMD graphics on Arch. Good luck, have fun, don't say I didn't warn ya.

---

## Requirements

- **Hyprland 0.55** (the plugin is built against this version's internal API).
- Either a `.conf` config (use dispatchers) or a `.lua` config (use the `hl.plugin.HyprWindowShade.*` functions — see [Lua config](#lua-config)).
- **GLSL ES 3.20** fragment shaders. The plugin uses Hyprland's `TEXVERTSRC320` vertex shader, so your fragment shader should start with `#version 320 es` and declare `in vec2 v_texcoord;`, `out vec4 fragColor;`, and `uniform sampler2D tex;` (same interface HyprShade uses).
- Tested on AMD graphics, Arch Linux. Other setups may work but are unverified.

---

## Install

Unzip the files to a directory, `cd` into it, then:

```sh
chmod +x build.sh
./build.sh
```

Then add this to your `hyprland.conf` (replace `USERNAME`):

```
exec-once = hyprctl plugin load /home/USERNAME/.local/share/hyprland/plugins/HyprWindowShade.so
```

---

## Window shaders

Apply a shader to a window via a `tag` on a `windowrule`. Six tags are supported:

| Tag | Behavior |
|---|---|
| `+shader:/path.glsl` | Always applied, regardless of focus |
| `+shader_fullscreen:/path.glsl` | Applies on fullscreen apps (default is to disable shaders when fullscreen) |
| `+shader_active:/path.glsl` | Applies only when the window is focused |
| `+shader_inactive:/path.glsl` | Applies only when the window is not focused |
| `+shader_floating:/path.glsl` | Applies only when floating |
| `+shader_tiled:/path.glsl` | Applies only when tiled |

> **Priority:** floating rules take precedence over active rules. If a window has both, the floating shader wins while the window is floating.

### Example

```
windowrule = match:class kitty, tag +shader:/home/USERNAME/.config/hypr/shaders/reading_mode.glsl
```

### Keybind examples

```
# Toggle a shader on every window matching a class
bind = $mainMod, K, toggleclassshader, google-chrome /home/USERNAME/.config/hypr/shaders/reading_mode.glsl

# Toggle a shader on the currently focused window
bind = $mainMod, W, togglewindowshader, /home/USERNAME/.config/hypr/shaders/pixelate.glsl

# Always apply a shader to a class at startup
exec-once = hyprctl dispatch classshader kitty /home/USERNAME/.config/hypr/shaders/pixelate.glsl

# Reload all shader source files (after editing a .glsl)
bind = $mainMod, R, reloadshaders
# or from a terminal:
#   hyprctl dispatch reloadshaders
```

---

## Layer shaders

Layers have a limited rule set, so layer shaders are controlled via dispatchers and `exec-once` rather than `layerrule`. For something like `rofi`, an `exec-once` at startup will re-apply the shader every time the layer appears.

```
# Apply at startup
exec-once = hyprctl dispatch layershader mpvpaper /home/USERNAME/.config/hypr/shaders/pixelate.glsl

# Toggle keybind
bind = $mainMod, B, togglelayershader, mpvpaper /home/USERNAME/.config/hypr/shaders/pixelate.glsl

# Force ON
bind = $mainMod, B, layershader, mpvpaper /home/USERNAME/.config/hypr/shaders/pixelate.glsl

# Force OFF (clear)
bind = $mainMod SHIFT, B, layershader, mpvpaper clear
```

---

## Dispatchers reference

All dispatchers are registered via `HyprlandAPI::addDispatcherV2` and can also be invoked from a shell with `hyprctl dispatch <name> <args>`.

| Dispatcher | Arguments | Effect |
|---|---|---|
| `classshader` | `<class> <path\|clear\|none>` | Force a shader on every window matching the class. `clear`/`none` removes it. |
| `toggleclassshader` | `<class> <path>` | Toggle the class shader on/off. |
| `togglewindowshader` | `<path>` | Toggle a shader on the currently focused window only. Pass `clear`/`none` to remove. |
| `layershader` | `<layer-namespace> <path\|clear\|none>` | Force a shader on a layer namespace (e.g. `rofi`, `mpvpaper`). |
| `togglelayershader` | `<layer-namespace> <path>` | Toggle a layer shader on/off. |
| `reloadshaders` | — | Drop the compiled shader cache and re-read every `.glsl` from disk. Shows a green toast on success. Usually not needed — see the troubleshooting note about auto-reload. |

> **Args with whitespace.** The first argument supports double-quoting, so a class name with a space works: `hyprctl dispatch classshader "Some Class" /path/to.glsl`. (Lua callers don't need this — each argument is its own string.)

---

## Lua config

Hyprland 0.55 doesn't surface plugin dispatchers to `.lua` configs, so the plugin also registers each action as a Lua function under `hl.plugin.HyprWindowShade.*`. Bind them by wrapping the call in a `function() ... end` (per vaxry's guidance for plugin actions on the Lua config path).

### Functions

Each one matches the dispatcher of the same name, but takes proper Lua arguments instead of a single space-separated string:

| Function | Arguments | Effect |
|---|---|---|
| `hl.plugin.HyprWindowShade.classshader(class, path)` | strings; `path` can be `"clear"`/`"none"` | Force a shader on every window matching `class`. |
| `hl.plugin.HyprWindowShade.toggleclassshader(class, path)` | strings | Toggle the class shader on/off. |
| `hl.plugin.HyprWindowShade.togglewindowshader(path)` | string; or `"clear"`/`"none"` | Toggle a shader on the currently focused window. |
| `hl.plugin.HyprWindowShade.layershader(ns, path)` | strings; `path` can be `"clear"`/`"none"` | Force a shader on a layer namespace. |
| `hl.plugin.HyprWindowShade.togglelayershader(ns, path)` | strings | Toggle a layer shader on/off. |
| `hl.plugin.HyprWindowShade.reloadshaders()` | — | Drop the compiled shader cache and re-read every `.glsl` from disk. |

### Calling from your binds

Plugin actions can't be referenced by name in a Lua bind — wrap them in a closure, which is the pattern vaxry recommends for any plugin function on the Lua config path:

```lua
-- Toggle a shader on the focused window
function() hl.plugin.HyprWindowShade.togglewindowshader("/home/USERNAME/.config/hypr/shaders/pixelate.glsl") end

-- Toggle a shader on every window of a class
function() hl.plugin.HyprWindowShade.toggleclassshader("google-chrome", "/home/USERNAME/.config/hypr/shaders/reading_mode.glsl") end

-- Reload all shader sources after editing a .glsl
function() hl.plugin.HyprWindowShade.reloadshaders() end
```

Drop those closures into whatever bind helper your Lua config uses — the closure body is the only plugin-specific piece. You can also call the functions directly at config-load time (no closure), e.g. to apply a startup layer shader:

```lua
hl.plugin.HyprWindowShade.layershader("mpvpaper", "/home/USERNAME/.config/hypr/shaders/pixelate.glsl")
```

### Tips and notes

- Functions appear under `hl.plugin.HyprWindowShade.*` only after the plugin is loaded. If `hyprctl plugin load ...` runs in your config, make sure it happens before any code that calls these functions.
- The functions take the same `"clear"` / `"none"` sentinels that the dispatchers do for removing a shader.
- `.conf`-style dispatchers are still registered too — you can mix them with the Lua functions if you have both kinds of configs, though there's no reason to in a Lua-only setup.

---

## Writing a shader

The plugin auto-wraps your shader: it renames your `void main()` to `void user_main()` and appends a `main()` that calls it and multiplies `fragColor` by `plugin_alpha`. You only need to write a normal HyprShade-style fragment shader.

Minimal example using three uniforms — focused windows ripple slightly, unfocused windows are dimmed:

```glsl
#version 320 es
precision highp float;

in vec2 v_texcoord;
out vec4 fragColor;
uniform sampler2D tex;

uniform float time;
uniform vec2  surface_size;
uniform float is_active;

void main() {
    // Horizontal ripple, only when focused.
    float wave = sin(v_texcoord.y * 40.0 + time * 4.0) * 0.005 * is_active;
    vec4 col = texture(tex, v_texcoord + vec2(wave, 0.0));

    // Dim inactive windows.
    col.rgb *= mix(0.6, 1.0, is_active);

    fragColor = col;
}
```

> Continuous redraws are only scheduled when your shader actually declares the `time` uniform. Static effects cost nothing extra.

---

## Available shader uniforms

Declare any of these in your fragment shader and the plugin will populate them every frame. Uniforms you don't declare are skipped (cached `-1` location), so there's no cost to leaving them out.

| Uniform | GLSL type | Source |
|---|---|---|
| `time` | `float` | seconds since plugin start (monotonic) |
| `plugin_alpha` | `float` | `window->alphaTotal()` for the window being drawn |
| `resolution` | `vec2` | active monitor pixel size |
| `surface_size` | `vec2` | window size |
| `mouse` | `vec2` | pointer position in compositor coords |
| `is_active` | `float` | 1.0 if focused, else 0.0 |
| `is_floating` | `float` | 1.0 if floating, else 0.0 |
| `is_fullscreen` | `float` | 1.0 if fullscreen, else 0.0 |

To add a new uniform: register its location in `ShaderEngine.cpp` (the `glGetUniformLocation` block, ~line 78) and push the value from `Hooks.cpp` in `hkUseShader` (~line 120).

---

## Troubleshooting

- **Shader compile errors.** A failed compile shows a red Hyprland notification for 15 seconds with the first ~200 characters of the GLSL error log. The plugin remembers the failure's mtime and won't re-toast every frame — it just sits silent until the file changes on disk, then automatically retries the compile.
- **Edits to a `.glsl` file aren't taking effect.** Edits are picked up automatically on the next draw — the cache is keyed by file mtime, so saving the file is enough. `hyprctl dispatch reloadshaders` is still available as a force-reload, but you shouldn't need it for ordinary edits.
- **Plugin doesn't seem to be loaded.** Run `hyprctl plugins list` to confirm `HyprWindowShade` is present. If it isn't, check the path in your `exec-once` line and rebuild with `./build.sh`.
- **Dispatchers do nothing on a `.lua` config.** Hyprland 0.55 doesn't surface plugin dispatchers to Lua configs — use the `hl.plugin.HyprWindowShade.*` functions instead (see [Lua config](#lua-config)).
- **`attempt to index a nil value (field 'HyprWindowShade')`** in Lua. The plugin isn't loaded yet when the config evaluates this line. Make sure the plugin's `hyprctl plugin load ...` runs first, or wrap your binds in a deferred call.
- **Shader doesn't show on a fullscreen window.** The default is to disable shaders on fullscreen. Use the `+shader_fullscreen:` tag or add it alongside your existing tag.
- **Floating rule and active rule both set.** The floating rule wins while the window is floating.

---

This was a vibe coding experiment with base Gemini Pro. I didn't write any of this code other than spending hours fighting the clankers. The AI makes a lot of assumptions for naming and function calls, overly believes the compiler suggestions, and doesn't ask for help or more information when it should. Honestly though, I learned C++ 25 years ago and haven't touched it since, and I was able to write a working plugin without writing a single line of code.
