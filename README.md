# HyprWindowShade

A Hyprland plugin that applies fragment shaders to individual windows (or layers) based on window rules. Shaders are HyprShade-compatible — if it works in HyprShade, it should work here. A `time` uniform is available for glitch-style animated effects, and windows and layers can play one-shot shaders as they open and close.

Configuration is shown in Hyprland's Lua config format (`hyprland.lua`). The old `hyprland.conf` format still parses in 0.56 but Hyprland itself warns that support is removed in 0.57 — everything `.conf`-specific lives in [Legacy: hyprland.conf](#legacy-hyprlandconf).

> This has not been stress-tested. It may break when Hyprland updates or simply not work on your system. Only tested on AMD graphics on Arch. Good luck, have fun, don't say I didn't warn ya.

---

## Contents

- [Quick start](#quick-start)
- [Requirements](#requirements) · [Install](#install)
- [Window shaders](#window-shaders) — [stacking](#stacking) · [fullscreen](#fullscreen) · [fallback rules](#fallback-rules)
- [Layer shaders](#layer-shaders)
- [Open and close animations](#open-and-close-animations)
- [Writing a shader](#writing-a-shader)
- [Reference](#reference) — [Lua API](#lua-api) · [hyprctl dispatch](#hyprctl-dispatch) · [shader uniforms](#shader-uniforms)
- [How close animations work](#how-close-animations-work)
- [Troubleshooting](#troubleshooting)
- [Legacy: hyprland.conf](#legacy-hyprlandconf)
- [Extending the plugin](#extending-the-plugin)

---

## Quick start

Save this as `~/.config/hypr/shaders/dim.glsl`:

```glsl
#version 320 es
precision highp float;

in vec2 v_texcoord;
out vec4 fragColor;
uniform sampler2D tex;
uniform float is_active;

void main() {
    fragColor = texture(tex, v_texcoord);
    fragColor.rgb *= mix(0.6, 1.0, is_active);   // dim while unfocused
}
```

Build and load the plugin ([Install](#install)), then add one rule to `hyprland.lua`:

```lua
hl.window_rule({
    name  = "kitty-dim",
    match = { class = "kitty" },
    tag   = "+shader:/home/USERNAME/.config/hypr/shaders/dim.glsl",
})
```

Reload the config. Every kitty window is now dimmed while unfocused and full brightness while focused. Edit `dim.glsl` and save — the change takes effect on the next frame, no reload needed.

---

## Requirements

- **Hyprland 0.56** (the plugin is built against this version's internal API).
- A **Lua config** (`~/.config/hypr/hyprland.lua`). A `.conf` config still works in 0.56 — see [Legacy: hyprland.conf](#legacy-hyprlandconf) — but Hyprland drops it in 0.57.
- **GLSL ES 3.20** fragment shaders. The plugin uses Hyprland's `TEXVERTSRC320` vertex shader, so your fragment shader should start with `#version 320 es` and declare `in vec2 v_texcoord;`, `out vec4 fragColor;`, and `uniform sampler2D tex;` (same interface HyprShade uses).

---

## Install

Unzip the files to a directory, `cd` into it, then:

```sh
chmod +x build.sh
./build.sh
```

Then load the plugin from `hyprland.lua` (replace `USERNAME`):

```lua
-- Loaded at parse time, so hl.plugin.HyprWindowShade.* is resolvable by the
-- time any bind or startup call below it runs.
hl.plugin.load("/home/USERNAME/.local/share/hyprland/plugins/HyprWindowShade.so")
```

A plugin built against a different Hyprland commit is rejected on ABI grounds, and an
uncaught error there aborts the *rest* of your config — binds, rules and all. Wrapping the
load in `pcall` costs you only shaders when it fails:

```lua
local ok, err = pcall(hl.plugin.load,
    "/home/USERNAME/.local/share/hyprland/plugins/HyprWindowShade.so")
if not ok then
    hl.on("hyprland.start", function()
        hl.notification.create({
            text    = "[HyprWindowShade] failed to load: " .. tostring(err),
            timeout = 8000,
            color   = "rgb(ff5555)",
        })
    end)
end
```

---

## Window shaders

Apply a shader to a window with a `tag` on a window rule. Ten tags are supported:

| Tag | Behavior |
|---|---|
| `+shader:/path.glsl` | Always applied, regardless of focus |
| `+shader_fullscreen:/path.glsl` | The one shader that applies while the window is fullscreen |
| `+shader_active:/path.glsl` | Applies only when the window is focused |
| `+shader_inactive:/path.glsl` | Applies only when the window is not focused |
| `+shader_floating:/path.glsl` | Applies only when floating |
| `+shader_tiled:/path.glsl` | Applies only when tiled |
| `+shader_open:/path.glsl` | Plays once when the window opens, on top of the window's normal shader |
| `+shader_close:/path.glsl` | Plays once as the window closes |
| `+shader_replace:1` | Opts this window out of [stacking](#stacking) |
| `+shader_fullscreen_stack:1` | Keeps this window's shaders while it is [fullscreen](#fullscreen) |

Any of the eight shader tags also accepts a `_default` suffix
(`+shader_close_default:`) marking it as a [fallback](#fallback-rules) that yields
to a rule setting the same tag without the suffix.

The leading `+` means "apply this tag" — the same prefix Hyprland's tag system uses everywhere. A window can carry as many of these as you like; see [stacking](#stacking) for how they combine.

A window's shaders also cover its subsurfaces and its popups — menus, dropdowns, tooltips — including while a popup plays its closing fade. A popup belonging to a *layer surface* instead of a window inherits that [layer's shader](#layer-shaders).

### Example rule

```lua
hl.window_rule({
    name  = "kitty-reading-mode",
    match = { class = "kitty" },
    tag   = "+shader:/home/USERNAME/.config/hypr/shaders/reading_mode.glsl",
})
```

### Stacking

Every tag that matches contributes a layer, and the layers are rendered on top of
each other. A window can wear a permanent look, a focus-dependent effect, and an
open animation at the same time:

```lua
hl.window_rule({
    match = { class = "google-chrome" },
    tag   = "+shader:/home/USERNAME/.config/hypr/shaders/reading_mode.glsl",
})
hl.window_rule({
    match = { class = "google-chrome" },
    tag   = "+shader_inactive:/home/USERNAME/.config/hypr/shaders/crt.glsl",
})
```

Focused, Chrome renders through `reading_mode`. Unfocused, it renders through
`reading_mode` **and then** `crt` — the CRT effect operates on the reading-mode
image rather than throwing it away. The same applies to open and close
animations, so an animation no longer snaps to a different-looking window when it
finishes.

Layers are applied bottom-to-top in this order:

| Order | Layer |
|---|---|
| 1 | `+shader:` |
| 2 | `+shader_floating:` or `+shader_tiled:` |
| 3 | `+shader_active:` or `+shader_inactive:` |
| 4 | `+shader_fullscreen:` |
| 5 | `+shader_open:` / `+shader_close:` |

Nothing about the shaders themselves has to change for this to work. Each layer
is a separate GL program that reads the layer below it through its own `tex`
sampler, exactly as if that layer's output were the window — so shaders written
by different people compose without being edited or renamed.

Two notes on what stacking changed:

- **Fullscreen is the exception.** Going fullscreen drops a window's shaders by
  default — see [fullscreen](#fullscreen) below.
- **`togglewindowshader` still replaces.** A manually toggled shader is an
  explicit override, so it takes the place of the rule layers rather than joining
  them. Animations still play on top of it.

To get the old behavior back on a given window — the first matching tag wins and
the rest are ignored — tag it `+shader_replace:1`.

#### Cost

Each layer beyond the first costs one offscreen pass over the window's texture,
per frame that window is drawn. Two or three layers on a handful of windows is
not something you will notice; a `time`-driven shader stacked under another one
on every window on screen is, since `time` forces a continuous redraw. If you
only want the effect while a window is unfocused, tag it `+shader_inactive:`
rather than `+shader:` and it costs nothing while you are using the window.

### Fullscreen

A fullscreen window gets **no shaders at all** by default. Someone who put a paper
or CRT effect on a browser almost certainly does not want it over a fullscreen
video, and a game is the last place a permanent post-process effect belongs — so
fullscreen is opt-in rather than opt-out.

Two ways to opt in:

| Want | Tag |
|---|---|
| One specific shader, only while fullscreen | `+shader_fullscreen:/path.glsl` |
| The window's normal stack, kept while fullscreen | `+shader_fullscreen_stack:1` |

```lua
-- mpv keeps its colour grade fullscreen, nothing else does
hl.window_rule({
    match = { class = "mpv" },
    tag   = "+shader_fullscreen_stack:1",
})
```

With `+shader_fullscreen_stack:1` the [stack](#stacking) resolves exactly as it
does windowed, with `+shader_fullscreen:` added as the top layer if present.
Without it, `+shader_fullscreen:` is the *only* layer that applies, and if that
tag is absent the window renders unshaded.

`+shader_fullscreen_stack:` has no `_default` form, for the same reason
`+shader_replace:` doesn't.

### Fallback rules

Stacking combines *different* tags. Two rules setting the **same** tag on one
window is a different problem: Hyprland keeps a window's tags in an
alphabetically sorted set, so a catch-all rule and a per-app rule that both set
`+shader_close:` resolve by whichever shader *path* sorts later. Renaming a file
can flip which one wins.

Append `_default` to any of the eight shader tags to mark it as a fallback. A
fallback applies only when the same tag without the suffix is absent from that
window, whatever the paths happen to be called:

```lua
-- every window closes with smoke...
hl.window_rule({
    match = { class = ".*" },
    tag   = "+shader_close_default:/home/USERNAME/.config/hypr/shaders/smoke_close.glsl@1.0",
})

-- ...except kitty, which closes with matrix
hl.window_rule({
    match = { class = "^(kitty)$" },
    tag   = "+shader_close:/home/USERNAME/.config/hypr/shaders/matrix_close.glsl@0.8",
})
```

This works for `+shader_default:`, `+shader_active_default:`,
`+shader_inactive_default:`, `+shader_floating_default:`, `+shader_tiled_default:`,
`+shader_fullscreen_default:`, `+shader_open_default:` and `+shader_close_default:`.
`+shader_replace:` has no `_default` form — a bool can't tell "unset" apart from
"explicitly off", so a default could never be overridden back off.

Two rules setting the same tag at the *same* level are still resolved by path
order. One rule per tag per window, plus a `_default` for the catch-all.

### Keybind examples

Plugin functions can't be referenced by name in a Lua bind — wrap them in a closure, which
is the pattern vaxry recommends for any plugin function on the Lua config path. Full
argument list in the [Lua API](#lua-api) reference.

```lua
local shaders = {
    pixelate    = "/home/USERNAME/.config/hypr/shaders/pixelate.glsl",
    readingMode = "/home/USERNAME/.config/hypr/shaders/reading_mode.glsl",
}

-- Toggle a shader on the currently focused window
hl.bind("SUPER + W", function()
    hl.plugin.HyprWindowShade.togglewindowshader(shaders.pixelate)
end)

-- Toggle a shader on every window matching a class
hl.bind("SUPER + K", function()
    hl.plugin.HyprWindowShade.toggleclassshader("google-chrome", shaders.readingMode)
end)

-- Reload all shader source files (after editing a .glsl)
hl.bind("SUPER + R", function()
    hl.plugin.HyprWindowShade.reloadshaders()
end)

-- Always apply a shader to a class at startup
hl.on("hyprland.start", function()
    hl.plugin.HyprWindowShade.classshader("kitty", shaders.pixelate)
end)
```

Looking the plugin table up *inside* the closure rather than at config-parse time also
means a bind still exists (and can report the problem) if the plugin failed to load:

```lua
local shade = function(fn, ...)
    local args = { ... }
    return function()
        local ns = hl.plugin.HyprWindowShade
        if not ns then
            hl.notification.create({ text = "[HyprWindowShade] plugin not loaded",
                                     timeout = 3000, color = "rgb(ff5555)" })
            return
        end
        ns[fn](table.unpack(args))
    end
end

hl.bind("SUPER + W", shade("togglewindowshader", shaders.pixelate))
```

---

## Layer shaders

> Popups belonging to a layer surface — a bar's tooltip or dropdown menu — are shaded with that layer's shader too. They do not pick up the layer's open animation or rounding, which belong to the layer's own box.

Layers have a limited rule set — no tags — so layer shaders are set by **namespace** through
the plugin's own functions rather than through `hl.layer_rule`. For something like `rofi`, a
call at startup re-applies the shader every time the layer appears.

```lua
-- Apply at startup
hl.on("hyprland.start", function()
    hl.plugin.HyprWindowShade.layershader("mpvpaper", shaders.pixelate)
end)

-- Toggle keybind
hl.bind("SUPER + B", function()
    hl.plugin.HyprWindowShade.togglelayershader("mpvpaper", shaders.pixelate)
end)

-- Force ON
hl.bind("SUPER + B", function()
    hl.plugin.HyprWindowShade.layershader("mpvpaper", shaders.pixelate)
end)

-- Force OFF (clear)
hl.bind("SUPER SHIFT + B", function()
    hl.plugin.HyprWindowShade.layershader("mpvpaper", "clear")
end)
```

Find a namespace with `hyprctl layers`.

---

## Open and close animations

A shader tagged with `+shader_open:` or `+shader_close:` runs **once**, driven by the
[`progress`](#shader-uniforms) uniform rather than looping on `time`. It renders on top of
whatever the window's other shader rules resolve to (see [stacking](#stacking)), so when
it finishes the window's normal appearance is already there underneath it and there is no
visible switch.

### The shader declares its own duration

How long the effect should run is a property of the effect, not of the keybind or the
window rule, so the shader says it directly — but a rule can override it for a one-off.
Three sources, highest priority first:

| Source | Syntax | Where it goes |
|---|---|---|
| Rule / call `@sec` | `…/dissolve.glsl@0.6` | Appended to the path in a window rule `tag`, or in `layeropenanim` / `layercloseanim`. |
| Shader `// @duration` | `// @duration 0.35` | Anywhere in the `.glsl` file. The effect's own natural length. |
| Built-in default | — | **0.3s**, used when neither of the above says anything. |

```lua
hl.window_rule({
    name  = "kitty-open-dissolve",
    match = { class = "kitty" },
    tag   = "+shader_open:/path/dissolve.glsl@0.6",
})
```

Both `@sec` and `// @duration` are capped at **5 seconds**, and anything longer is
clamped rather than rejected — `@10` gives you a 5s animation, not the 0.3s default. The
cap is there so a typo — `30` for `3` — can't leave a closing window's snapshot sitting on
screen for half a minute.

`// @duration` is a comment rather than a real GLSL construct on purpose: GLSL ES forbids
initializers on uniforms, so there's no in-language way to declare a value the plugin can
read *before* the shader ever runs, and it needs the number on the CPU side to know when
the animation is over.

If a shader declares the `progress` uniform but no `// @duration`, the plugin falls back
to 0.3s *and* toasts once to tell you — a shader written as an animation that never says
how long it should run is almost always an oversight. Like compile errors, the warning is
keyed to the file's mtime: one notification per edit, not one per frame, and it clears
itself as soon as you save a fix. Shaders that don't use `progress` never warn, and an
explicit `@sec` on the rule suppresses it too, since that's unambiguous.

### Example: dissolve on open

```glsl
#version 320 es
precision highp float;
// @duration 0.4

in  vec2 v_texcoord;
out vec4 fragColor;
uniform sampler2D tex;
uniform float progress;   // 0 -> 1 over the animation
uniform float seed;       // differs per window

void main() {
    // Dissolve in from noise, scaled so each window breaks up differently.
    float n = fract(sin(dot(v_texcoord + seed, vec2(12.9898, 78.233))) * 43758.5453);
    fragColor = texture(tex, v_texcoord);
    fragColor *= step(n, progress);
}
```

Use the same shape for a close shader, but end at **fully transparent** when
`progress` reaches 1.0 — see [How close animations work](#how-close-animations-work).

### Layer surfaces (rofi/wofi, notifications, bars)

Layer surfaces have no rule tags in Hyprland 0.56, so their animations are configured by
**namespace** like the rest of the layer API:

```lua
hl.plugin.HyprWindowShade.layeropenanim("rofi",  "/path/to/open.glsl")
hl.plugin.HyprWindowShade.layercloseanim("rofi", "/path/to/close.glsl")
```

Pass `"clear"` or `"none"` as the path to remove one. The `@sec` duration override works
here too (`"/path/to/open.glsl@0.2"`), with the same precedence as window rules.

Find a namespace with `hyprctl layers`. Common ones: `rofi`, `wofi`, `notifications` (or
`mako` / `swaync`), `waybar`, `hyprlock`.

Everything else behaves exactly as it does for windows — same [`progress` and `seed`](#shader-uniforms)
uniforms, same `// @duration`, same snapshot-based close path.

---

## Writing a shader

The plugin auto-wraps your shader: it renames your `void main()` to `void user_main()` and appends a `main()` that calls it, multiplies `fragColor` by `plugin_alpha`, and re-applies the window's corner rounding. You only need to write a normal HyprShade-style fragment shader.

The wrapper also clamps your colour to what its alpha permits (`fragColor.rgb = min(fragColor.rgb, vec3(fragColor.a))`). Surface colours are premultiplied and the compositor blends with `src.rgb + dst * (1 - src.a)`, so a fragment carrying more colour than its alpha is *added* to what is behind it rather than covering it — which is why a shader returning full-intensity colour at a low alpha paints a pale slab over popups and shadows. The clamp only ever touches the colour channel, so shaders that drive an effect through alpha — every [open/close animation](#open-and-close-animations) — are unaffected, and a shader that already premultiplies correctly is left exactly as it was.

The clamp is a safety net, not a substitute for premultiplying properly: it bounds the colour rather than scaling it, so for partial alpha it lands close to the right answer but not on it. Premultiply in your own shader and the wrapper becomes a no-op. It also overrides the one legitimate reason to emit colour above alpha — deliberate additive glow — which is rare enough to be worth the trade.

Rounding is part of the wrapper because Hyprland rounds corners *inside* the fragment program your shader replaces. Without it, any shader that writes its own alpha — `fragColor = vec4(color, 1.0)` is the common shape — would fill in the rounded corners and leave the window square. The wrapper `discard`s fragments outside the rounded outline rather than only zeroing alpha, since an opaque window is drawn with blending off and a zero alpha there would simply be ignored. It applies only to a window's own surface, and only when your shader declares `in vec2 v_texcoord;` — a shader working purely off `gl_FragCoord` keeps the plain wrapper and the old square-corner behavior.

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

## Reference

### Lua API

Hyprland 0.55+ doesn't surface plugin dispatchers to `.lua` configs, so the plugin registers
every action as a Lua function under `hl.plugin.HyprWindowShade.*`. Each argument is its own
Lua string.

| Function | Arguments | Effect |
|---|---|---|
| `classshader` | `(class, path\|"clear"\|"none")` | Force a shader on every window matching the class. `clear`/`none` removes it. |
| `toggleclassshader` | `(class, path)` | Toggle the class shader on/off. |
| `togglewindowshader` | `(path)` | Toggle a shader on the currently focused window only. Pass `"clear"`/`"none"` to remove. |
| `layershader` | `(namespace, path\|"clear"\|"none")` | Force a shader on a layer namespace (e.g. `rofi`, `mpvpaper`). |
| `togglelayershader` | `(namespace, path)` | Toggle a layer shader on/off. |
| `layeropenanim` | `(namespace, path[@sec]\|"clear"\|"none")` | One-shot animation when a layer of that namespace appears. |
| `layercloseanim` | `(namespace, path[@sec]\|"clear"\|"none")` | One-shot animation as a layer of that namespace goes away. |
| `reloadshaders` | — | Drop the compiled shader cache and re-read every `.glsl` from disk. Shows a green toast on success. Usually not needed — see the troubleshooting note about auto-reload. |

Two notes:

- Functions appear under `hl.plugin.HyprWindowShade.*` only after the plugin is loaded. Make
  sure `hl.plugin.load(...)` runs before any code that calls them, and prefer looking the
  table up inside a bind closure (see [Keybind examples](#keybind-examples)).
- Class names or namespaces containing spaces need no special treatment here — each argument
  is a separate Lua string.

### hyprctl dispatch

Every function above is also registered as a native dispatcher via
`HyprlandAPI::addDispatcherV2`, invokable from any shell:

```sh
hyprctl dispatch layershader mpvpaper /home/USERNAME/.config/hypr/shaders/pixelate.glsl
hyprctl dispatch reloadshaders
```

The dispatcher names and semantics match the [Lua API](#lua-api) table exactly, except that
arguments arrive as one space-separated string. Handy for testing a shader without touching
your config. The first argument supports double-quoting, so a class name with a space works:
`hyprctl dispatch classshader "Some Class" /path/to.glsl`. A dispatcher that can't parse its
arguments reports a usage error rather than silently doing nothing, so
`hyprctl dispatch layershader rofi` tells you the path is missing.

> Dispatchers fire from `hyprctl` and from `.conf` binds, but **not** from Lua binds — that's
> why the Lua functions exist.

### Shader uniforms

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
| `progress` | `float` | 0.0 → 1.0 across an open/close animation; 1.0 otherwise |
| `seed` | `float` | stable per-window random value in 0..1 |

Names beginning with `plugin_` are reserved by the wrapper. Besides `plugin_alpha` it
injects `plugin_box_size`, `plugin_round` and `plugin_round_power` to re-apply corner
rounding — don't declare those names yourself, and don't write to `fragColor` expecting
them to be absent.

---

## How close animations work

<details>
<summary>Design notes — why every close path animates, and the one rule your close shader has to follow.</summary>

Close animations ride Hyprland's own fadeout. When a window closes, Hyprland
snapshots it into a framebuffer and renders that snapshot for the duration of the fadeout
animation. The plugin tags that snapshot with the window's `shader_close:` shader and
shades it on the way out. Layer surfaces close through the identical mechanism
(`CLayerFadeout` mirrors `CWindowFadeout`), so both share this path. So:

- The close request is never delayed — window close semantics are completely unchanged.
- **Every** close path animates: keybind, X button, `killactive`, the app quitting itself.
- An app that refuses to close simply never fades out, so nothing animates. Correct by
  construction, with no revert timer to get wrong.

Two consequences worth knowing:

- **Your close shader should reach full transparency at `progress == 1.0`.** The plugin
  replaces Hyprland's texture shader, so Hyprland's own fade-out alpha isn't applied to
  the snapshot — your shader has sole control of how it disappears. If it ends opaque,
  the snapshot pops rather than fades.
- Hyprland normally drops the fadeout when *its* animation finishes, which can be sooner
  than your shader wants. The plugin holds it open for exactly the declared duration, so
  you don't have to match your config's fade-out animation. If Hyprland's fadeout
  animation is disabled entirely, no snapshot is created and close animations won't run.

</details>

---

## Troubleshooting

- **Shader compile errors.** A failed compile shows a red Hyprland notification for 15 seconds with the first ~200 characters of the GLSL error log. The plugin remembers the failure's mtime and won't re-toast every frame — it just sits silent until the file changes on disk, then automatically retries the compile.
- **Edits to a `.glsl` file aren't taking effect.** Edits are picked up automatically on the next draw — the cache is keyed by file mtime, so saving the file is enough. `reloadshaders()` is still available as a force-reload, but you shouldn't need it for ordinary edits.
- **Plugin doesn't seem to be loaded.** Run `hyprctl plugins list` to confirm `HyprWindowShade` is present. If it isn't, check the path in your `hl.plugin.load(...)` line and rebuild with `./build.sh`.
- **`attempt to index a nil value (field 'HyprWindowShade')`.** The plugin isn't loaded yet when the config evaluates this line. Make sure `hl.plugin.load(...)` runs first, or move the call inside a bind closure / `hl.on("hyprland.start", ...)`.
- **Dispatchers do nothing from a Lua bind.** Hyprland 0.55+ doesn't surface plugin dispatchers to Lua configs — use the `hl.plugin.HyprWindowShade.*` functions instead (see [Lua API](#lua-api)). `hyprctl dispatch` from a shell still works.
- **Shader doesn't show on a fullscreen window.** That is the default: fullscreen drops a window's shaders so games and videos are left alone. Add `+shader_fullscreen:/path.glsl` for a shader that applies only while fullscreen, or `+shader_fullscreen_stack:1` to keep the window's normal stack. See [fullscreen](#fullscreen).
- **A catch-all rule is overriding a per-app rule.** Hyprland keeps tags in an alphabetically sorted set, so two rules setting the same tag on one window resolve by whichever *path* sorts later, not by which rule is more specific. Stacking doesn't help — both tags write the same layer. Mark the catch-all as a [fallback](#fallback-rules) with the `_default` suffix.
- **A shader stopped applying after adding another.** Stacking runs each layer through the one below it, so a layer that ignores `tex` and writes a solid color will hide everything beneath it. That is the shader's doing, not the plugin's.
- **A menu, tooltip or popup over a shaded window shows as a pale or dark rectangular slab.** Popups and subsurfaces are shaded along with the window they belong to, and surface colors are **premultiplied**: RGB is already scaled by alpha, so a fragment must never emit more color than its alpha allows. Compositing is `result = src.rgb + dst * (1 - src.a)`, which means a fragment with alpha 0 and non-zero RGB is *added* to whatever is behind it. A popup's transparent margin — the part its shadow shows through — then picks up the shader's color as a visible haze in the shape of the popup's box.

  Both common mistakes cause it. `fragColor = vec4(color, 1.0)` throws the shape away entirely. `fragColor = vec4(color, src.a)` keeps the shape but forgets the premultiplication, so transparent pixels still emit full-intensity color. The correct form scales the color by the alpha it is paired with:

  ```glsl
  vec4 src = texture(tex, v_texcoord);
  vec3 col = /* your effect, from src.rgb */;
  fragColor = vec4(col * src.a, src.a);   // premultiplied
  ```

  A shader that also displaces its coordinates must sample alpha at the coordinate it actually read, not at `v_texcoord`. Corner rounding is restored by the wrapper regardless, but the margin cannot be — a shader is entitled to set alpha, and that is how the [dissolve animations](#example-dissolve-on-open) work.

---

## Legacy: hyprland.conf

Hyprland 0.56 still parses `hyprland.conf`, but it prints *"You are using the .conf config
format, support for which will be removed in Hyprland 0.57."* on startup. Everything below
works today on a `.conf` config and will stop working when you upgrade past 0.56 — the Lua
equivalent for each is linked inline.

**The one real functional gap:** plugin dispatchers *do* fire from `.conf` binds, which is
why the `.conf` path uses them everywhere instead of the [Lua API](#lua-api).

### Loading the plugin

```
exec-once = hyprctl plugin load /home/USERNAME/.local/share/hyprland/plugins/HyprWindowShade.so
```

### Window shader rules

Same eight [tags](#window-shaders), written as a `windowrule` tag:

```
windowrule = match:class kitty, tag +shader:/home/USERNAME/.config/hypr/shaders/reading_mode.glsl
windowrule = match:class kitty, tag +shader_open:/path/dissolve.glsl@0.6
```

### Keybinds and startup

These call the plugin's [dispatchers](#hyprctl-dispatch) — same names, same arguments, but
as one space-separated string.

```
# Toggle a shader on every window matching a class
bind = $mainMod, K, toggleclassshader, google-chrome /home/USERNAME/.config/hypr/shaders/reading_mode.glsl

# Toggle a shader on the currently focused window
bind = $mainMod, W, togglewindowshader, /home/USERNAME/.config/hypr/shaders/pixelate.glsl

# Always apply a shader to a class at startup
exec-once = hyprctl dispatch classshader kitty /home/USERNAME/.config/hypr/shaders/pixelate.glsl

# Reload all shader source files (after editing a .glsl)
bind = $mainMod, R, reloadshaders
```

### Layer shaders and layer animations

```
# Apply at startup
exec-once = hyprctl dispatch layershader mpvpaper /home/USERNAME/.config/hypr/shaders/pixelate.glsl

# Toggle keybind
bind = $mainMod, B, togglelayershader, mpvpaper /home/USERNAME/.config/hypr/shaders/pixelate.glsl

# Force ON
bind = $mainMod, B, layershader, mpvpaper /home/USERNAME/.config/hypr/shaders/pixelate.glsl

# Force OFF (clear)
bind = $mainMod SHIFT, B, layershader, mpvpaper clear

# Open/close animations, by namespace
exec-once = hyprctl dispatch layeropenanim  rofi /path/to/open.glsl
exec-once = hyprctl dispatch layercloseanim rofi /path/to/close.glsl
```

---

## Extending the plugin

Popups are matched to their owner in `hkGLDrawTex`. The renderer's `currentWindow` is only
published while it is walking a window's own tree, so it comes back empty for a popup being
snapshotted for its close fade — which used to capture that snapshot unshaded. The owner is
therefore resolved directly, via `CPopup::getT1Owner()` for a window and `CPopup::layerOwner()`
for a layer surface. The layer owner is kept in its own variable rather than reusing `pLS`,
so a popup inherits the layer's shader without also inheriting its open animation or its
corner rounding.

The offscreen stages used by [stacking](#stacking) render through the projection Hyprland
set up for the monitor — it is built in `begin()` and cannot be swapped out mid-pass, so
`runIntermediateStages` expresses its blit in that projection's coordinate space rather
than building its own matrix. Changing this is the first thing to check if a stacked window
ever renders at the wrong size or position.

To add a new uniform: add its `GLint ...Loc` field to `CompiledShader` in `Globals.hpp`,
register the location in the `glGetUniformLocation` block at the end of
`getOrCompileShader` (`ShaderEngine.cpp`), and push the value from the uniform-injection
block in `hkUseShader` (`Hooks.cpp`).

When adding a new *action*, register it both as a dispatcher (`addDispatcherV2`) and as a
Lua function (`addLuaFunction`), delegating to a shared helper in the `shadeActions::`
namespace — see the parallel registration blocks in `main.cpp`.

To add a new *conditional shader tag*: add the field to `WindowShaderState` (`Globals.hpp`),
parse it in the tag loop in `applyShaderRulesSafe` (`Hooks.cpp`), and add it to the layer
order in `collectBaseLayers`. If the total number of possible layers grows past five, bump
`MAX_SHADER_STAGES` — the stack arrays are fixed-size and sized from it. Also add it to
`resolveShaderPath`, which is the first-match-wins ladder kept for `+shader_replace:1`, and
to the merge block at the end of `applyShaderRulesSafe` so the tag's `_default` form works.
The `_default` suffix itself needs no per-tag handling: it is stripped off the key before
the tag is matched.
