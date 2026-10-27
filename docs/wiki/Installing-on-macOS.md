# Installing on macOS

Pixelion Captions for macOS ships as a code-signed `.pkg` installer that places the plugin bundle (`pixelion-captions.plugin`) in the OBS plugins folder for you. It is a **universal binary** (Apple silicon + Intel) for **macOS 13.7 (Ventura) and later**.

> **Close OBS first.** Close OBS Studio completely before installing or updating the plugin.

---

## 1. Download

Download the macOS installer from the [Releases page](https://github.com/pixelsolutions/pixelion-captions/releases):

```
pixelion-captions-<version>-macos-universal.pkg
```

Choose the `.pkg`, not the source archive.

![The downloaded .pkg in Downloads](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/wiki/images/macos_pkg_01.png)

---

## 2. Run the installer

Double-click the `.pkg` to open the macOS Installer, then click **Continue** through the introduction.

![Installer welcome / introduction](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/wiki/images/macos_pkg_02.png)

On the **Installation Type** step, the installer does a standard per-user install into your home folder (`~/Library/Application Support/obs-studio/plugins/`), so only your account gets the plugin and no admin password is needed. To install for all users instead, click **Change Install Location** and pick the system-wide destination. Click **Install**.

![Installation Type - standard per-user install](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/wiki/images/macos_pkg_03.png)

When it finishes you'll see a success screen. Click **Close**.

![The installation was successful](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/wiki/images/macos_pkg_04.png)

---

## 3. Open the dock in OBS

Launch OBS Studio (31.1.1 or later). Once the plugin loads it appears in the OBS source/plugin list. Open the dock from **Docks > Pixelion Captions**, then sign in to start captions.

![OBS Docks menu with Pixelion Captions selected and the dock open on macOS](https://raw.githubusercontent.com/pixelsolutions/pixelion-captions/main/docs/wiki/images/macos_pkg_05.png)

---

## Updating

Close OBS, run the newer `.pkg` the same way as the first install (it replaces the bundle in place), then relaunch OBS. Your settings and saved transcripts are kept.

---

## Uninstalling

The `.pkg` does not install an uninstaller, so remove the plugin by hand. Close OBS, then delete the bundle from wherever the installer placed it (per-user by default):

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
| Sign-in doesn't persist | If sign-in does not stick, sign in again from the dock. |

---

## Supported versions

| macOS | Architecture | OBS Studio |
|---|---|---|
| 13.7 Ventura | Apple silicon / Intel | 31.1.1+ |
| 14.x Sonoma | Apple silicon | 31.1.1+ |
| 15.x Sequoia | Apple silicon | 31.1.1+ |

---

## Need help?

- Ask questions on the [Pixelion Captions thread on the OBS forum](https://obsproject.com/forum/threads/pixelion-captions.196676/).
- [Submit an issue](https://github.com/pixelsolutions/pixelion-captions/issues) for a bug or request.
- Enjoying the plugin? [Submit a review](https://obsproject.com/forum/resources/pixelion-captions.2795/rate).
