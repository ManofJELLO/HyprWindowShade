#!/bin/bash
# build.sh - v0.56 build script for HyprWindowShade

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

PLUGIN_DIR="$HOME/.local/share/hyprland/plugins"
PLUGIN_PATH="$PLUGIN_DIR/HyprWindowShade.so"
HEADERS="/var/cache/hyprpm/$USER/headersRoot"

# --build-only compiles and runs every verification, then stops without
# unloading, installing, or loading anything. Use it to compile-check against a
# live session you don't want to disturb — which is most of them, since a plugin
# that misbehaves takes the whole compositor with it.
BUILD_ONLY=0
case "${1:-}" in
    --build-only|--check|-n) BUILD_ONLY=1 ;;
    "") ;;
    *) echo "usage: $0 [--build-only]"; exit 2 ;;
esac

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

# Build FIRST, unload only once we have a verified artifact. The previous
# ordering unloaded up front, so any compile error left the session with no
# plugin at all until the next successful build.
BUILD_TMP="$(pwd)/HyprWindowShade.so.build"

echo "[Build] Running make..."
# Compile flags live in the Makefile, which is also what hyprpm runs. Keeping a
# second copy here meant the dev loop and everyone else's install could drift
# apart silently — so this just calls it, with the same environment hyprpm uses.
#
# PKG_CONFIG_PATH points pkg-config at the hyprpm header tree, matching the
# commit check above. Without it pkg-config resolves to /usr/include/hyprland
# instead; either tree works alone, but mixing them will not compile.
export PKG_CONFIG_PATH="$HEADERS/share/pkgconfig:$PKG_CONFIG_PATH"
if ! make all TARGET="$BUILD_TMP"; then
    echo -e "${RED}[Error] Build failed. Nothing was unloaded; the running plugin is untouched.${NC}"
    rm -f "$BUILD_TMP"
    exit 1
fi
# The Makefile already fails the build on STB_GNU_UNIQUE symbols and on a
# thread_local with a non-trivial destructor — both pin this .so against dlclose
# and make `hyprctl plugin load` silently re-run the previous build. Those guards
# live there rather than here so hyprpm installs get them too.
echo -e "${GREEN}          Clean — no unique symbols, no TLS destructors.${NC}"

# Cheap sanity check only. It catches a non-ELF or a wildly broken link; it does
# NOT prove the file is sound, because readelf happily parses a file with a
# zeroed tail. The real protection is the sync after install; the checksum
# written alongside is there so corruption can be diagnosed after the fact.
if ! readelf -dW "$BUILD_TMP" >/dev/null 2>&1; then
    echo -e "${RED}[Error] Built artifact is not a parseable ELF — refusing to install.${NC}"
    rm -f "$BUILD_TMP"
    exit 1
fi

if [ "$BUILD_ONLY" -eq 1 ]; then
    echo -e "${GREEN}[Build-only] Compiled and verified at $BUILD_TMP${NC}"
    echo "             Nothing was unloaded, installed, or loaded."
    exit 0
fi

echo "[Plugin] Unloading previous version from memory..."
# Unload by whatever path is actually mapped, not just ours. Once the plugin is
# installed through hyprpm the running copy lives in hyprpm's cache, so
# unloading "$PLUGIN_PATH" is a no-op and the load below then collides with an
# already-registered plugin name.
HYPR_PID=$(pidof Hyprland 2>/dev/null | awk '{print $1}')
if [ -n "$HYPR_PID" ]; then
    awk '/HyprWindowShade.*\.so/ {print $NF}' "/proc/$HYPR_PID/maps" 2>/dev/null | sort -u | while read -r p; do
        [ -n "$p" ] && echo "          unloading $p" && hyprctl plugin unload "$p" >/dev/null 2>&1
    done
fi
# Belt and braces for the case where /proc was unreadable.
hyprctl plugin unload "$PLUGIN_PATH" >/dev/null 2>&1
sleep 2

# Confirm the old module actually left the compositor's address space. If glibc
# kept it mapped (see the -fno-gnu-unique note above), the `hyprctl plugin load`
# at the end will name-match the still-loaded object and silently re-run the OLD
# build instead of the one we just compiled.
PINNED=0
HYPR_PID=$(pidof Hyprland 2>/dev/null | awk '{print $1}')
if [ -n "$HYPR_PID" ] && grep -q 'HyprWindowShade.*\.so' "/proc/$HYPR_PID/maps" 2>/dev/null; then
    PINNED=1
    echo -e "${RED}[Warn] Old module is STILL MAPPED after unload — it is pinned in memory.${NC}"
    echo "       This happens with builds made before -fno-gnu-unique / the"
    echo "       thread_local fix. Loading now would silently re-run the old code."
    echo "       Continuing the build; you'll need to restart Hyprland once."
fi

mkdir -p "$PLUGIN_DIR"
echo -n "Installing to $PLUGIN_DIR..."
# Write, then FLUSH TO DISK before reporting success. Without the sync the file
# contents sit in page cache: the rename is journalled but the data blocks are
# not, so an unclean shutdown in the next few seconds leaves a file of the right
# size whose tail is all zeros. That is not a hypothetical — it happened, and a
# zeroed plugin loaded from the config bricks every subsequent boot.
BUILD_SUM=$(md5sum < "$BUILD_TMP")
cp "$BUILD_TMP" "$PLUGIN_PATH.new" || { echo -e "${RED}copy failed${NC}"; rm -f "$BUILD_TMP"; exit 1; }
sync "$PLUGIN_PATH.new"
mv "$PLUGIN_PATH.new" "$PLUGIN_PATH"
sync "$PLUGIN_PATH" 2>/dev/null || sync
rm -f "$BUILD_TMP"
echo -e "${GREEN}Complete!${NC}"

# Re-read what actually landed on disk, rather than trusting the write. Catches
# a truncated or partially-written install before it can reach a boot.
echo "[Verify] Re-reading the installed file..."
if ! readelf -dW "$PLUGIN_PATH" >/dev/null 2>&1; then
    echo -e "${RED}[Error] Installed file is not a parseable ELF.${NC}"
    exit 1
fi
# Checksum, not an ELF parse. Measured: `readelf -d` and `readelf -S` BOTH exit 0
# on a file whose header block is intact and whose entire tail is zeros — which
# is exactly the corruption an unclean shutdown produces. Only a content hash
# actually catches it.
if [ "$(md5sum < "$PLUGIN_PATH")" != "$BUILD_SUM" ]; then
    echo -e "${RED}[Error] Installed file does not match what was built. Refusing to load.${NC}"
    exit 1
fi

# Leave the hash next to the plugin purely as a diagnostic. After any hard crash
# or unclean shutdown you can tell in one command whether the installed plugin is
# intact, without rebuilding to compare:
#     [ "$(md5sum < ~/.local/share/hyprland/plugins/HyprWindowShade.so)" \
#       = "$(cat ~/.local/share/hyprland/plugins/HyprWindowShade.so.md5)" ] && echo intact
# Nothing reads this automatically; the sync above is what prevents the problem.
printf '%s' "$BUILD_SUM" > "$PLUGIN_PATH.md5"
sync "$PLUGIN_PATH.md5" 2>/dev/null || sync
echo -e "${GREEN}          Installed file matches the build; checksum recorded.${NC}"

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

# If this plugin is also installed through hyprpm, the copy in hyprpm's cache is
# what loads at the next login — not the one just built. Say so, rather than
# letting a change appear to survive a reboot when it will not.
HYPRPM_SO="/var/cache/hyprpm/$USER/HyprWindowShade/HyprWindowShade.so"
if [ -f "$HYPRPM_SO" ] && ! cmp -s "$HYPRPM_SO" "$PLUGIN_PATH"; then
    echo -e "${RED}[Note] hyprpm has a DIFFERENT build cached at${NC}"
    echo "       $HYPRPM_SO"
    echo "       That copy is what loads at your next login; this build is live"
    echo "       for the current session only. To make it stick: commit, then"
    echo "       run 'hyprpm update'."
fi
