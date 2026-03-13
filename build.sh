#!/bin/bash
# build.sh

# We keep the standard of commenting the workflow for future reference.

# --- COLOR DEFINITIONS ---
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color (resets terminal text back to default)

echo -e "${GREEN}Starting build for HyprWindowShade...${NC}"

# 1. Compile the plugin using the Makefile
echo "[Build] Running make..."
make all

# 2. Halt the script if compilation fails, outputting in bold red
if [ $? -ne 0 ]; then
    echo -e "${RED}[Error] Compilation failed. Aborting load.${NC}"
    exit 1
fi

echo -e "${GREEN}Build successful!${NC}"

# 3. Define the absolute path to the compiled shared object
# Using the local share directory for proper Hyprland standardization
PLUGIN_DIR="$HOME/.local/share/hyprland/plugins"
PLUGIN_PATH="$PLUGIN_DIR/HyprWindowShade.so"

# Create the directory just in case it doesn't exist yet
mkdir -p "$PLUGIN_DIR"

# 4. Move the compiled object to the standardized Hyprland plugin folder
echo -n "Moving HyprWindowShade.so to $PLUGIN_DIR..."
mv "$(pwd)/HyprWindowShade.so" "$PLUGIN_PATH"
echo -e "${GREEN}Complete!${NC}"

# 5. Unload the plugin if it is already running (suppresses errors if it isn't loaded)
# Thanks to our explicit unhooking in PLUGIN_EXIT, this is completely safe.
echo "[Plugin] Unloading previous version..."
hyprctl plugin unload "$PLUGIN_PATH" > /dev/null 2>&1

# 6. Load the fresh plugin into the compositor
echo "[Plugin] Loading new version..."
hyprctl plugin load "$PLUGIN_PATH"

# 7. Print the helpful user output for configuration reference
echo ""
echo "To load the plugin manually, run:"
echo "  hyprctl plugin load $PLUGIN_PATH"
echo ""
echo "To ensure it loads on startup, add this to your hyprland.conf:"
echo "  exec-once = hyprctl plugin load $PLUGIN_PATH"
echo ""

echo -e "${GREEN}[Success] HyprWindowShade is now live!${NC}"