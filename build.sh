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

echo "[Build] Running make..."
# v0.56: Use ONLY the hyprpm cache header tree to avoid duplicate-definition errors.
# Do not mix /usr/include/hyprland/src with /var/cache/hyprpm — they will conflict.
g++ -shared -fPIC -O3 -std=c++23 *.cpp -o HyprWindowShade.so \
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

mkdir -p "$PLUGIN_DIR"
echo -n "Moving HyprWindowShade.so to $PLUGIN_DIR..."
rm -f "$PLUGIN_PATH"
mv "$(pwd)/HyprWindowShade.so" "$PLUGIN_PATH"
echo -e "${GREEN}Complete!${NC}"

echo "[Plugin] Loading new version..."
hyprctl plugin load "$PLUGIN_PATH"

echo -e "${GREEN}[Success] HyprWindowShade is now live!${NC}"