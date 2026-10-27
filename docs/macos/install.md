# Installing Pixelion Captions on macOS

Feature: macos-platform-support, Task 6.4 (Requirements 6.7, 6.8)

Pixelion Captions for macOS ships as a code-signed `.plugin` bundle
(`pixelion-captions.plugin`) with bundle id `ai.pixelion.captions`, built as a
universal binary (arm64 + x86_64) for macOS 13.7 and later.

> **Notarization:** a build is notarized only when it comes from a version tag
> push, or from a manual run of the **Dispatch** workflow with **notarize = true**
> (`gh workflow run dispatch.yaml -f job=build -f notarize=true`). A branch push or
> a plain dispatch (notarize unchecked) is signed but **not** notarized, so
> Gatekeeper blocks it on first launch — see
> [First launch and Gatekeeper](#first-launch-and-gatekeeper) below. A notarized
> build opens with no Gatekeeper prompt.

## Install directory

Copy `pixelion-captions.plugin` into one of:

- Per-user (recommended):
  `~/Library/Application Support/obs-studio/plugins/`
- System-wide (all users on the Mac):
  `/Library/Application Support/obs-studio/plugins/`

Create the `plugins` directory if it does not exist, then restart OBS Studio
31.1.1 or later. The plugin appears in the OBS source/plugin list once loaded.

## First launch and Gatekeeper

A **notarized** release build loads with no Gatekeeper prompt; skip this section.

A **signed-but-not-notarized** build (branch/dispatch build) is blocked by
Gatekeeper on first load, because macOS only clears it silently once Apple's notary
service has stapled a ticket. To allow a non-notarized build:

- Remove the quarantine attribute, then restart OBS:

  ```bash
  xattr -dr com.apple.quarantine \
    ~/Library/Application\ Support/obs-studio/plugins/pixelion-captions.plugin
  ```

  (use the `/Library/...` path instead for a system-wide install)

- Or, after OBS reports the block, open **System Settings > Privacy & Security**,
  scroll to the message about `pixelion-captions.plugin`, and click **Open Anyway**,
  then relaunch OBS.

This is only needed for non-notarized builds. Prefer a notarized release for normal
distribution so end users never see this.

## First-launch permission prompts

On first use you may see:

1. **Keychain access** - when your Pixelion sign-in is first saved and when it is
   read back. Allow it so your sign-in persists across OBS restarts. Only the
   refresh credential is stored; access tokens and session secrets stay in
   memory only.
2. **Default browser launch** - sign-in opens your default web browser to the
   Pixelion hosted sign-in page. Complete sign-in there and return to OBS.

The plugin does not request microphone or camera access: it reads the OBS
program audio mix, not a system input device.

## License and source code (GPL-2.0-or-later)

Pixelion Captions is licensed under GPL-2.0-or-later. The complete corresponding
source for each release is published alongside the binary as a `sources` archive
(produced by the release workflow's `sources` package variant). You may obtain,
inspect, modify, and rebuild the source under the terms of the GPL; see the
`LICENSE` file bundled with the plugin.
