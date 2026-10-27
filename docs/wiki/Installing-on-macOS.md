# Installing on macOS

Pixelion Captions for macOS ships as a signed and notarized plugin bundle (`pixelion-captions.plugin`), built as a **universal binary** (Apple silicon + Intel) for **macOS 13.7 (Ventura) and later**. Because it is notarized, it installs and loads with Gatekeeper enabled, no security overrides needed.

> **Close OBS first.** Close OBS Studio completely before installing or updating the plugin.

---

## 1. Download

Download the macOS plugin from the [Releases page](https://github.com/pixelsolutions/pixelion-captions/releases):

```
pixelion-captions-<version>-macos-universal.pkg   (or the .plugin bundle)
```

Choose the plugin package, not the source archive.

<!-- SCREENSHOT: Releases page with the macOS asset highlighted -->
<!-- ![Download the macOS plugin from Releases](images/mac-01-releases-download.png) -->

---

## 2. Install the plugin

Copy `pixelion-captions.plugin` into one of the OBS plugin folders:

- **Per-user (recommended)** — just your account:
  ```
  ~/Library/Application Support/obs-studio/plugins/
  ```
- **System-wide** — all users on the Mac:
  ```
  /Library/Application Support/obs-studio/plugins/
  ```

If the `plugins` folder does not exist, create it first.

> Tip: in Finder, use **Go > Go to Folder** (Shift+Cmd+G) and paste `~/Library/Application Support/obs-studio/plugins/` to open the per-user location.

<!-- SCREENSHOT: pixelion-captions.plugin placed in the per-user plugins folder -->
<!-- ![The .plugin bundle in the per-user OBS plugins folder](images/mac-02-plugins-folder.png) -->

---

## 3. Restart OBS and open the dock

Launch OBS Studio (31.1.1 or later). Once the plugin loads it appears in the OBS source/plugin list. Open the dock from **Docks > Pixelion Captions**.

<!-- SCREENSHOT: OBS Docks menu with "Pixelion Captions" selected on macOS -->
<!-- ![Opening the Pixelion Captions dock on macOS](images/mac-03-docks-menu.png) -->

![The Pixelion Captions dock](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/screenshots/obs_pixelion.png)

---

## First-launch permission prompts

On first use you may see:

1. **Keychain access** — when your Pixelion sign-in is first saved and when it is read back. Allow it so your sign-in persists across OBS restarts. Only the refresh credential is stored; access tokens and session secrets stay in memory.
2. **Default browser launch** — sign-in opens your default web browser to the Pixelion hosted sign-in page. Complete sign-in there and return to OBS.

The plugin does **not** request microphone or camera access. It reads the OBS program audio mix, not a system input device.

<!-- SCREENSHOT (optional): Keychain access prompt -->
<!-- ![Keychain access prompt on first sign-in](images/mac-04-keychain.png) -->

---

## Updating

Close OBS, replace the `pixelion-captions.plugin` bundle in your plugins folder with the new one, then relaunch OBS. Your settings and saved transcripts are kept.

---

## Uninstalling

Close OBS, then delete the bundle from wherever you installed it:

```
~/Library/Application Support/obs-studio/plugins/pixelion-captions.plugin
```
or
```
/Library/Application Support/obs-studio/plugins/pixelion-captions.plugin
```

To also clear saved settings, remove:

```
~/Library/Application Support/obs-studio/plugin_config/pixelion-captions
```

Your transcripts and the Keychain sign-in entry are left untouched unless you remove them yourself (sign-in lives in **Keychain Access** under your login keychain).

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Plugin doesn't appear in OBS | Confirm the `.plugin` is directly inside a `plugins` folder (not nested), and that you fully restarted OBS. Check the OBS log (**Help > Log Files**). |
| Gatekeeper blocks it | The bundle is notarized, so this should not happen on a supported macOS. If it does, confirm you downloaded the official release and did not alter the bundle. |
| Sign-in doesn't persist | Allow the Keychain prompt. If you denied it, delete the `pixelion-captions` entry in Keychain Access and sign in again. |

---

## Supported versions

| macOS | Architecture | OBS Studio |
|---|---|---|
| 13.7 Ventura | Apple silicon / Intel | 31.1.1+ |
| 14.x Sonoma | Apple silicon | 31.1.1+ |
| 15.x Sequoia | Apple silicon | 31.1.1+ |
