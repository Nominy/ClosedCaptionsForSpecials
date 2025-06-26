# ClosedCaptionsForSpecials

## installation:
0. dependencies:
- [PD3 UE4SS](https://modworkshop.net/mod/47771)
- [PD3 Logic Mod Loader](https://modworkshop.net/mod/44049)
- `AllowModsMod`(which should be bundled with PD3 UE4SS)
1. acquire the built mod(mod is actually 3 mods: `.pak` mod which must be cooked in Unreal Engine 4 editor and packed, C++ mod which must be compiled with UE4SS mod build pipeline, and Lua which is delivered as is) - **you can just use the Github Releases for packaged release**. 
2. copy the folder inside the `\Mods\` to `...\steamapps\common\PAYDAY3\PAYDAY3\Binaries\Win64\Mods\`
3. enable the mod in `\Mods\mods.txt`(add `MyAwesomeMod : 1` to the end of mod declarations or whatever the folder name it would be)
EXAMPLE(make sure `AllowModsMod: 1` is also up!):
```
CheatManagerEnablerMod : 1
ActorDumperMod : 0
ConsoleCommandsMod : 1
ConsoleEnablerMod : 1
SplitScreenMod : 0
LineTraceMod : 1
BPModLoaderMod : 1
BPML_GenericFunctions : 1
jsbLuaProfilerMod : 0
AllowModsMod: 1
MyAwesomeMod : 1




; Built-in keybinds, do not move up!
Keybinds : 1
```
3. copy the `.pak` mod in `\~mods\` to `...\steamapps\common\PAYDAY3\PAYDAY3\Content\Paks\~mods`
