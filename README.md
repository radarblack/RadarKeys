# RadarKeys — An InfiniteHeaven Module

Allows keybinds to run scripts through button interactions using the IHHook implementation.
It runs on a separate thread, running side by side with other hooks and frameworks.  

## Installation
```
1. Install InfiniteHeaven
2. Place the RadarKeys.dll in the main directory (with the mgsvtpp.exe).
3. Place the RadarKeys_Core.lua in MGSV-TPP directory → ".../mod/modules"
```
## Usage
```
• Default hotkey is F7 to open the RadarKeys menu in-game.

To bind a key:
1. Select the key from the dropdown box at the bottom.
2. In the text box, place the name of the script where the trigger is located.
3. Hit Assign and it will be listed above in the list.

To unbind a key:
1. Guess what button "removes" it from the list?
2. Or, you can just press the "Remove All Bindings".

To use an assigned script:
1. Like, do I really have to explain how to do this...?
2. Like, seriously?
3. No. You're trolling.
```
## Updates
```
[08/26/2026] - First successful build. Working condition. Ready for release, but fixing some UI related concerns before it.
[08/27/2026] - Fixing and improving the error logging system. Unifying the debug report to the spdlog reports for uniform reporting.
[?] - First public release in NexusMods.
```
## Plans ahead
- [x] Address some of the immediate UI concerns (misspells, formatting, and spacing, etc.)
- [x] Cull unnecessary/unused function from the IHHook and VFramework implementations
- [x] Improve error logging and readability in the log files
- [ ] Make a showcase script of how "core" and "trigger" script functions
- [ ] Release the mod on NexusMods (First Official Release)
- [ ] Make some template scripts for a simple and complex script mods
- [ ] Final clean up (review, reassessment, function refactoring to shorten runtime execution if needed)
- [ ] Find a way to read and run certain function within the script without passing through every functions on the script
- [ ] Add in mouse and gamepad support
- [ ] Enhance key assignment feature (eliminating the dropdown box selection and instead a direct input detection through RawInput)
- [ ] Add in "Hold" functionality to the key assignment
- [ ] Add in presets that can enhance gameplay experience
- [ ] \(Optional) Less hopeful, but attempt to bind the mod directly towards the game binding file, instead of using IHHook
- [ ] Consider this project complete with continued updates until EOS.
- And, redesign some of the old mods that may benefit to this module.
