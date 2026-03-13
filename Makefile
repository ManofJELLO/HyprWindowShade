# Plugin Name
PLUGIN_NAME=HyprWindowShade

# Source Files
SOURCE_FILES=main.cpp

# Compiler and Flags
CXX=g++
CXXFLAGS=-shared -fPIC --optimize=3 -std=c++23
INCLUDES=-I/usr/include/hyprland -I/usr/include/pixman-1 -I/usr/include/libdrm

# Libraries
LIBS=-lGLEW -lGL

# Build Rule
all:
	$(CXX) $(CXXFLAGS) $(SOURCE_FILES) -o $(PLUGIN_NAME).so $(INCLUDES) $(LIBS)

# Clean Rule
clean:
	rm -f $(PLUGIN_NAME).so
