#!/bin/bash
# build.sh - v0.56 build script for HyprWindowShade

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

PLUGIN_DIR="$HOME/.local/share/hyprland/plugins"
PLUGIN_PATH="$PLUGIN_DIR/HyprWindowShade.so"
HEADERS="/var/cache/hyprpm/$USER/headersRoot"

echo -e "${GREEN}Starting build for HyprWindowShade...${NC}"

# HYPRLAND_API_VERSION is the literal "0.1", so Hyprland will happily load a
# plugin built against the wrong commit and then die on the ABI mismatch.
# Check the header commit against the running compositor before touching anything.
echo "[Check] Verifying hyprpm headers..."
if [ ! -d "$HEADERS/include/hyprland/src" ]; then
    echo -e "${RED}[Error] hyprpm headers not found at $HEADERS${NC}"
    echo "        Run: hyprpm update"
    exit 1
fi

HDR_COMMIT=$(grep -oP '(?<=GIT_COMMIT_HASH    ")[0-9a-f]+' "$HEADERS/include/hyprland/src/version.h")
RUN_COMMIT=$(hyprctl version -j | grep -oP '(?<="commit": ")[0-9a-f]+')

if [ -z "$HDR_COMMIT" ] || [ -z "$RUN_COMMIT" ]; then
    echo -e "${RED}[Error] Could not determine header or compositor commit.${NC}"
    exit 1
fi

if [ "$HDR_COMMIT" != "$RUN_COMMIT" ]; then
    echo -e "${RED}[Error] Header/compositor commit mismatch${NC}"
    echo "        headers:    $HDR_COMMIT"
    echo "        compositor: $RUN_COMMIT"
    echo "        Run: hyprpm update"
    exit 1
fi

echo "[Plugin] Unloading previous version from memory..."
hyprctl plugin unload "$PLUGIN_PATH"
sleep 2

# Confirm the old module actually left the compositor's address space. If glibc
# kept it mapped (see the -fno-gnu-unique note below), the `hyprctl plugin load`
# at the end will name-match the still-loaded object and silently re-run the OLD
# build instead of the one we're about to compile.
PINNED=0
HYPR_PID=$(pidof Hyprland 2>/dev/null | awk '{print $1}')
if [ -n "$HYPR_PID" ] && grep -q 'HyprWindowShade.*\.so' "/proc/$HYPR_PID/maps" 2>/dev/null; then
    PINNED=1
    echo -e "${RED}[Warn] Old module is STILL MAPPED after unload — it is pinned in memory.${NC}"
    echo "       This happens with builds made before -fno-gnu-unique / the"
    echo "       thread_local fix. Loading now would silently re-run the old code."
    echo "       Continuing the build; you'll need to restart Hyprland once."
fi

echo "[Build] Running make..."
# v0.56: Use ONLY the hyprpm cache header tree to avoid duplicate-definition errors.
# Do not mix /usr/include/hyprland/src with /var/cache/hyprpm — they will conflict.
# -fno-gnu-unique is REQUIRED for live-reload, not an optimization. Inline-function
# statics and template statics default to STB_GNU_UNIQUE binding, and glibc marks
# any DSO defining a unique symbol as NODELETE — dlclose then never unmaps it, so
# the `hyprctl plugin load` below silently hands back the previous build. This
# tree emits ~146 such symbols without the flag. See the [Verify] step below.
g++ -shared -fPIC -O3 -std=c++23 -fno-gnu-unique *.cpp -o HyprWindowShade.so \
    -I/var/cache/hyprpm/$USER/headersRoot/include \
    -I/var/cache/hyprpm/$USER/headersRoot/include/hyprland \
    -I/var/cache/hyprpm/$USER/headersRoot/include/hyprland/src \
    -I/var/cache/hyprpm/$USER/headersRoot/include/hyprland/protocols \
    -I/usr/include/cairo \
    -I/usr/include/freetype2 \
    -I/usr/include/libpng16 \
    -I/usr/include/pixman-1 \
    -I/usr/include/libdrm \
    -lGLESv2 -lEGL -lGL

if [ $? -ne 0 ]; then
    echo -e "${RED}[Error] Compilation failed. Aborting load.${NC}"
    exit 1
fi

# Guard the two things that pin this .so in memory and break live-reload. If
# either reappears, `hyprctl plugin load` starts silently running the OLD build
# and you chase phantom bugs, so fail the build loudly instead.
echo "[Verify] Checking the module can actually be unloaded..."
UNIQUE_CNT=$(readelf -sW HyprWindowShade.so | grep -c 'UNIQUE')
TLS_DTORS=$(readelf -sW HyprWindowShade.so | grep -c '__cxa_thread_atexit')

if [ "$UNIQUE_CNT" -ne 0 ]; then
    echo -e "${RED}[Error] $UNIQUE_CNT STB_GNU_UNIQUE symbol(s) — glibc will mark this NODELETE.${NC}"
    echo "        Is -fno-gnu-unique still in the g++ line above?"
    exit 1
fi

if [ "$TLS_DTORS" -ne 0 ]; then
    echo -e "${RED}[Error] __cxa_thread_atexit reference — a thread_local with a non-trivial${NC}"
    echo -e "${RED}        destructor is pinning this .so against dlclose.${NC}"
    echo "        Find it with: readelf -sW HyprWindowShade.so | grep ' TLS '"
    echo "        Fix: make it a plain global (rendering is main-thread-only)."
    exit 1
fi
echo -e "${GREEN}          Clean — no unique symbols, no TLS destructors.${NC}"

mkdir -p "$PLUGIN_DIR"
echo -n "Moving HyprWindowShade.so to $PLUGIN_DIR..."
rm -f "$PLUGIN_PATH"
mv "$(pwd)/HyprWindowShade.so" "$PLUGIN_PATH"
echo -e "${GREEN}Complete!${NC}"

if [ "$PINNED" -eq 1 ]; then
    echo -e "${RED}[Skipped] Not loading — the pinned old module would win.${NC}"
    echo -e "${GREEN}New build installed at $PLUGIN_PATH.${NC}"
    echo "Restart Hyprland once to clear the stale mapping; reloads work normally after that."
    exit 0
fi

echo "[Plugin] Loading new version..."
LOAD_OUT=$(hyprctl plugin load "$PLUGIN_PATH" 2>&1)
echo "$LOAD_OUT"
if echo "$LOAD_OUT" | grep -qiE "could not be loaded|error"; then
    echo -e "${RED}[Error] Plugin load failed.${NC}"
    exit 1
fi

echo -e "${GREEN}[Success] HyprWindowShade is now live!${NC}"