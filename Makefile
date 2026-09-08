# Build entry point for hyprpm (see hyprpm.toml).
#
# HEADER TREES
# hyprpm runs these steps with PKG_CONFIG_PATH pointed at its own headersRoot,
# so `pkg-config --cflags hyprland` resolves to the header tree matching the
# running compositor. Building by hand without that resolves to /usr/include
# instead. Either is fine on its own — what must never happen is BOTH on one
# command line, which produces duplicate-definition errors. Letting pkg-config
# decide keeps it to one tree; do not add -I paths for Hyprland by hand.

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
LDLIBS   += -lGLESv2 -lEGL -lGL

all: $(TARGET)

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
