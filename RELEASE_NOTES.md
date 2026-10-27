## Pixelion Captions 0.1.2

This release adds macOS support and a Windows installer, so you no longer need to unpack a ZIP by hand.

![Windows installer setup wizard](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/wiki/images/mswin_exe_01.jpg)

![macOS .pkg installer in the Downloads folder](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/wiki/images/macos_pkg_01.png)

### Changes
- macOS support: Pixelion Captions now runs on macOS 13.7 (Ventura) and later as a universal build for Apple silicon and Intel.
- New Windows installer: the setup .exe finds your OBS Studio install, copies the plugin, and adds an uninstaller. It upgrades in place and keeps your settings and saved transcripts. The ZIP is still available.
- New install guides with screenshots for Windows and macOS in the wiki, and an updated README.

### Install
1. Close OBS Studio.
2. Download the package for your platform below. The source ZIP is not needed to run the plugin.
   - Windows: run pixelion-captions-0.1.2-windows-x64-installer.exe. The plugin ZIP is available if you prefer a manual install. Step-by-step guide: [Installing on Windows](https://github.com/pixelsolutions/pixelion-captions/wiki/Installing-on-Windows)
   - macOS: open pixelion-captions-0.1.2-macos-universal.pkg and follow the steps. Step-by-step guide: [Installing on macOS](https://github.com/pixelsolutions/pixelion-captions/wiki/Installing-on-macOS)
3. Open OBS Studio, open the Docks menu, and select Pixelion Captions.

A free Pixelion.ai account is needed to sign in. Translation requires a Pixelion Pro plan.

See the README and the install guides in the wiki for full usage instructions and requirements. Tested with OBS Studio 32.2.2 on Windows and macOS.
