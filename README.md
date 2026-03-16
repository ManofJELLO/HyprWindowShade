## HyprWindowShade

There is a plugin Hypr-DarkWindows that already allows shaders on windows and is listed in on the Hyprland website. That author seems to actively maintain it, which is not something I really intend to do here. I'll update it for my own use and maybe time to time, but as was famously said "Don't quote me boy. I ain't said shit"

This is a plugin designed to allow you to apply shaders to individual windows based on windowRules in the Hyprland.conf. The shaders are specifically made to be compatible with HyprShade from the AUR. This was my downfall with trying to use DarkWindows with some of the shaders that I had already written. If a shader works for HyprShade it should work here, but what do I know. You can also  use Time in the shader for glitch like effects.    

You are also able to apply shaders to layers though not through the LayerRule system, as I understand Layers have a limited list of available rules. Instead there is a toggle keybind and you can exec-once at startup. For a layer like Rofi, if you make an exec-once at start up, it will apply the shader on Rofi everytime it is called. 

This is some not stress tested stuff and could break when Hyprland updates or just not work for your system. I've only tested this on AMD graphics on Arch for my personal system.   

Good Luck have fun, don't say I didn't warn ya.  

## How to use the script  

unzip the files to a directory  
In your terminal go to the directory with the files  
enter the below commands:  
chmod +x build.sh  
./build.sh  

Update your hyprland.conf with below lines:  
exec-once = hyprctl plugin load /home/USERNAME/.local/share/hyprland/plugins/HyprWindowShade.so  


## HOW to use this plugin

## If you want to apply to a layer at startup below is an example:  
exec-once = hyprctl dispatch layershader mpvpaper /home/manofjello/.config/hypr/shaders/pixelate.glsl  

## KeyBind layer example for toggle:  
bind = $mainMod, B, togglelayershader, mpvpaper /home/manofjello/.config/hypr/shaders/pixelate.glsl  

## Turn the pixelate shader ON for mpvpaper
bind = $mainMod, B, layershader, mpvpaper /home/manofjello/.config/hypr/shaders/pixelate.glsl

## Turn the shader OFF (clear it)
bind = $mainMod SHIFT, B, layershader, mpvpaper clear  

Update:  
You can now use six different tags in your hyprland.conf:  
  
tag +shader:/path.glsl (Applies all the time, regardless of focus)  
tag +shader_fullscreen:/path.glsl (Default behavior is to disable shaders on fullscreen apps, this tag applies the shader on a fullscreen app)  
tag +shader_active:/path.glsl (Applies ONLY when you click the window)  
tag +shader_inactive:/path.glsl (Applies ONLY when you click away)  
tag +shader_floating:/path.glsl (Applies ONLY to floating windows)  
tag +shader_tiled:/path.glsl (Applies ONLY to tiled windows)  
Behavior Note:  
Floating rules are prioritized over the Active rules. This means if you have both a floating rule AND an active rule, the floating shader will takeprecedence while the window is floating.  
  
## Example windowRule for reading_mode.glsl shader:
windowrule = match:class kitty, tag +shader:/home/manofjello/.config/hypr/shaders/reading_mode.glsl

## if you want a dynamic keybind to toggle the effect for every single open program
bind = $mainMod, K, toggleclassshader, google-chrome /home/manofjello/.config/hypr/shaders/reading_mode.glsl
## Toggle the shader on the currently focused window
bind = $mainMod, W, togglewindowshader, /home/manofjello/.config/hypr/shaders/pixelate.glsl
## If you want a program to always have the shader applied automatically on startup
exec-once = hyprctl dispatch classshader kitty /home/manofjello/.config/hypr/shaders/pixelate.glsl
  
## keybind to reload the shaders if you made changes to their .glsl file to adjust the effect
## or you can write "hyprctl dispatch reloadshaders" in terminal
bind = $mainMod, R, reloadshaders 

This was a vibe coding experiment with base Gemini Pro. I didn't write any of this code other than spending hours fighting the clankers. The AI makes a lot of assumptions for naming and function calls, overly believes the compiler suggestions and doesn't ask for help or more information when it should. Honestly though, I learned C++ 25 years ago and haven't touched it since and I was able to write a working plugin without writing a single line of code.  
