#!/bin/bash
# build.sh - v0.55 build script for HyprWindowShade

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

PLUGIN_DIR="$HOME/.local/share/hyprland/plugins"
PLUGIN_PATH="$PLUGIN_DIR/HyprWindowShade.so"

echo -e "${GREEN}Starting build for HyprWindowShade...${NC}"

echo "[Plugin] Unloading previous version from memory..."
hyprctl plugin unload "$PLUGIN_PATH"
sleep 2

echo "[Build] Running make..."
# v0.55: Use ONLY the hyprpm cache header tree to avoid duplicate-definition errors.
# Do not mix /usr/include/hyprland/src with /var/cache/hyprpm — they will conflict.
g++ -shared -fPIC -O3 -std=c++23 *.cpp -o HyprWindowShade.so \
    -I/var/cache/hyprpm/manofjello/headersRoot/include \
    -I/var/cache/hyprpm/manofjello/headersRoot/include/hyprland \
    -I/var/cache/hyprpm/manofjello/headersRoot/include/hyprland/src \
    -I/var/cache/hyprpm/manofjello/headersRoot/include/hyprland/protocols \
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