# Installing on Windows

Pixelion Captions runs on **Windows x64** with OBS Studio (tested with 32.0.1). There are two ways to install it:

- **[Installer (recommended)](#installer-recommended)** — a guided `.exe` that finds OBS, copies the files, and registers an entry in Add/Remove Programs.
- **[Manual ZIP](#manual-zip)** — extract and copy the files yourself. Use this if you prefer not to run an installer or need a portable setup.

Both deliver the same plugin. The installer is easier and gives you a clean uninstall; the ZIP gives you full control over where files land.

> **Close OBS first.** OBS locks the plugin files while it runs, so close it completely before installing, updating, or uninstalling.

---

## Installer (recommended)

### 1. Download

Download the Windows installer from the [Releases page](https://github.com/pixelsolutions/pixelion-captions/releases):

```
pixelion-captions-<version>-windows-x64-installer.exe
```

Choose the installer, not the source ZIP.

### 2. Run the installer

Double-click the downloaded `.exe`.

The installer is **not code-signed yet**, so Windows SmartScreen shows a blue "Windows protected your PC" screen with an "unknown publisher" notice. This is expected. Click **More info**, then **Run anyway**. Then approve the User Account Control (admin) prompt — admin rights are needed because the plugin installs into the OBS Studio program folder.

### 3. Accept the license

Pixelion Captions is licensed under GPL-2.0-or-later. Select **I accept the agreement** and click **Next**.

![Installer license agreement page](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/wiki/images/mswin_exe_01.jpg)

### 4. Confirm the OBS folder

The installer detects your OBS Studio installation automatically (usually `C:\Program Files\obs-studio`) and offers it as the install location. If it cannot find OBS, or you installed OBS somewhere custom, click **Browse** and pick your OBS folder. The installer warns you if the folder does not contain `bin\64bit\obs64.exe`. Click **Next**.

![Select Destination Location showing the detected OBS folder](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/wiki/images/mswin_exe_02.jpg)

### 5. Install

Review the summary and click **Install**.

![Ready to Install summary page](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/wiki/images/mswin_exe_03.jpg)

The installer places:

| File | Location |
|---|---|
| Plugin | `<OBS>\obs-plugins\64bit\pixelion-captions.dll` |
| Data (locale, icon) | `<OBS>\data\obs-plugins\pixelion-captions\` |

### 6. Finish

Click **Finish** to close the wizard.

![Completing the Pixelion Captions Setup Wizard](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/wiki/images/mswin_exe_04.jpg)

### 7. Open the dock in OBS

Open OBS Studio. From the top menu choose **Docks > Pixelion Captions**. The dock appears on the right, and you can sign in to start captions.

![OBS Docks menu with Pixelion Captions selected and the dock open](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/wiki/images/mswin_exe_05.jpg)

---

## Manual ZIP

Use this if you prefer not to run the installer.

1. Close OBS Studio.
2. Download the Windows x64 **plugin ZIP** (not the source ZIP) from [Releases](https://github.com/pixelsolutions/pixelion-captions/releases):
   `pixelion-captions-<version>-windows-x64.zip`
3. Extract the ZIP. Copy the `pixelion-captions` folder into:
   ```
   C:\ProgramData\obs-studio\plugins
   ```
   Keep its `bin` and `data` folders together. The result should be:
   ```
   C:\ProgramData\obs-studio\plugins\pixelion-captions\bin\64bit\pixelion-captions.dll
   C:\ProgramData\obs-studio\plugins\pixelion-captions\data\...
   ```
4. Open OBS Studio and choose **Docks > Pixelion Captions**.

<!-- SCREENSHOT (optional): extracted folder copied into C:\ProgramData\obs-studio\plugins -->
<!-- ![Plugin folder placed under ProgramData](images/win-05-manual-zip.png) -->

---

## Updating

Close OBS before updating.

- **Installer:** run the newer installer over the top. It upgrades in place and keeps your settings and saved transcripts.
- **Manual ZIP:** delete the old `pixelion-captions` folder under `C:\ProgramData\obs-studio\plugins` and copy in the new one. Your settings and transcripts (stored separately) are kept.

---

## Uninstalling

Close OBS first.

**If you used the installer:** open **Settings > Apps** (or Control Panel > Programs), find **Pixelion Captions (OBS plugin)**, and uninstall it. This removes the plugin files.

By design, the uninstaller **keeps your settings and sign-in** so a reinstall is seamless. For a complete removal that also clears your saved settings and the Plugin Manager entry, see the [full removal steps](#full-removal-clean-slate) below.

**If you used the manual ZIP:** delete the `pixelion-captions` folder from `C:\ProgramData\obs-studio\plugins`.

### Full removal (clean slate)

To remove everything, including saved settings and the OBS Plugin Manager registration, delete these as well (with OBS closed):

| What | Path |
|---|---|
| Saved settings and sign-in | `%APPDATA%\obs-studio\plugin_config\pixelion-captions` |
| Plugin Manager entry | remove the `pixelion-captions` entry from `%APPDATA%\obs-studio\plugin_manager\modules.json` |

Your transcripts (in `Documents\Pixelion Live Captions`) and saved account credentials (Windows Credential Manager) are left untouched unless you remove them yourself. If a scene still references a caption source, OBS shows it as unavailable; remove it inside OBS.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Installer won't run / "unknown publisher" | Expected — the installer is unsigned. Click **More info > Run anyway** on the SmartScreen prompt. |
| "This does not look like an OBS Studio installation" | You pointed the installer at the wrong folder. Browse to your OBS folder (the one containing `bin\64bit\obs64.exe`). |
| Installer says OBS is running | Close OBS completely (check the system tray) and retry. |
| Plugin not in the Docks menu | Confirm OBS is x64, the files are in the right place, and you fully restarted OBS. Check the OBS log at `%APPDATA%\obs-studio\logs` for a load error. |
| Still listed in Plugin Manager after removing files | Clear the `pixelion-captions` entry from `modules.json` (see Full removal). The entry survives file deletion. |

For OBS logs: in OBS, **Help > Log Files > View Current Log**, or browse `%APPDATA%\obs-studio\logs`.
