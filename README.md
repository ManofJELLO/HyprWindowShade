### How to Build and Use  
Preparation:  

Save the C++ code we've been working on as main.cpp.  

Save the above Makefile in the same directory.  

Ensure you have the Hyprland headers installed (usually hyprland-devel or hyprland-headers depending on your distro).  

Compile:  

Open your terminal in that folder and run:  

Bash  
make  
This will generate a HyprWindowShade.so file.  

Load the Plugin:  

You can load it dynamically via the terminal to test it:  

Bash  
hyprctl plugin load $(pwd)/HyprWindowShade.so  
Configure Window Rules:  

Add a rule to your hyprland.conf using the prefix we defined:  

Bash  
# Format: windowrule = plugin:shader:<path_to_glsl>, <window_regex>  
windowrule = plugin:shader:/home/youruser/shaders/pixelate.glsl, ^(kitty)$  

If it complains about missing headers, ensure pkg-config --cflags hyprland points to the correct directory.  
