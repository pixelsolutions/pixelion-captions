## Pixelion Captions 0.1.1

This release improves how the caption box behaves on screen so it works like real captions, and refreshes the default caption look.

![Captions shown live in an OBS scene](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/wiki/images/obs_6.jpg)

![The Pixelion Captions dock](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/wiki/images/obs_pixelion.png)

### Changes
- The caption box now appears only while transcription is arriving. When you press Start it no longer shows the "Your captions will appear here" placeholder, and the box stays hidden until the first caption comes in.
- After you stop speaking, the current caption stays visible for about two seconds and then the box hides itself, instead of leaving the last line on screen.
- After a pause, the next thing you say starts on a clean box rather than continuing after the previous sentence. Your saved transcript still keeps the full text.
- When you press Stop, the box clears and returns to the preview placeholder.
- The default caption font size is now larger and the line spacing is tighter, for a cleaner look out of the box. Resetting the caption style also updates the font size selector to the new default.

### Install
1. Close OBS Studio.
2. Download the Windows x64 ZIP below (the plugin package, not the source ZIP).
3. Extract it and copy the pixelion-captions folder into C:\ProgramData\obs-studio\plugins, keeping its bin and data folders together.
4. Open OBS Studio, open the Docks menu, and select Pixelion Captions.

See the README for full usage instructions and requirements. Tested with OBS Studio 32.0.1 on Windows x64.
