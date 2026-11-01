# Build entry point for hyprpm (see hyprpm.toml).
#
# HEADER TREES
# hyprpm runs these steps with PKG_CONFIG_PATH pointed at its own headersRoot,
# so `pkg-config --cflags hyprland` resolves to the header tree matching the
# running compositor. Building by hand without that resolves to /usr/include
# instead. Either is fine on its own — what must never happen is BOTH on one
# command line, which produces duplicate-definition errors. Letting pkg-config
# decide keeps it to one tree; do not add -I paths for Hyprland by hand.

# LUA HEADERS
# main.cpp includes <lua.hpp> for the hl.plugin.HyprWindowShade.* callbacks.
# hyprland.pc does not list lua in Requires, so its --cflags never carries the
# include path, and distros that ship the headers in a VERSIONED directory
# (Debian/Ubuntu: /usr/include/lua5.5) then fail the build outright on
# "lua.hpp: No such file or directory". Ask pkg-config for it separately.
#
# The version must be the one the compositor already has mapped, because we
# deliberately do not link liblua — the symbols resolve out of the host process
# at dlopen (see main.cpp). Hyprland 0.57 links liblua5.5, so prefer that name
# and only fall back to the unversioned alias, which on some distros points at
# an older series.
LUA_PKG := $(firstword $(foreach p,lua5.5 lua-5.5 lua55 lua,\
             $(shell pkg-config --exists $(p) && echo $(p))))

TARGET  := HyprWindowShade.so
SRC     := $(wildcard *.cpp)

# -fno-gnu-unique is REQUIRED, not an optimisation. Inline-function statics and
# template statics default to STB_GNU_UNIQUE binding, and glibc marks any DSO
# defining a unique symbol as NODELETE — dlclose then never unmaps it, so
# `hyprctl plugin load` silently hands back the previously loaded build. This
# header tree emits ~146 such symbols without the flag.
#
# -O3 is deliberate: hkGLDrawTex/hkUseShader run per textured surface per frame.
CXXFLAGS += -shared -fPIC -O3 -std=c++23 -fno-gnu-unique
CXXFLAGS += $(shell pkg-config --cflags hyprland)
CXXFLAGS += $(shell pkg-config --cflags $(LUA_PKG))
LDLIBS   += -lGLESv2 -lEGL -lGL

all: $(TARGET)

ifeq ($(LUA_PKG),)
$(error No lua pkg-config module found (tried lua5.5, lua-5.5, lua55, lua). \
Install the Lua 5.5 development headers — e.g. liblua5.5-dev)
endif

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $@ $(LDLIBS)
	@# Guard the two things that pin this .so in memory and break live-reload.
	@# If either reappears, a reload silently re-runs the OLD build.
	@if [ "$$(readelf -sW $@ | grep -c UNIQUE)" -ne 0 ]; then \
		echo "ERROR: STB_GNU_UNIQUE symbols present — is -fno-gnu-unique set?"; rm -f $@; exit 1; fi
	@if [ "$$(readelf -sW $@ | grep -c __cxa_thread_atexit)" -ne 0 ]; then \
		echo "ERROR: thread_local with a non-trivial destructor pins this .so against dlclose."; rm -f $@; exit 1; fi

clean:
	rm -f $(TARGET)

.PHONY: all clean
