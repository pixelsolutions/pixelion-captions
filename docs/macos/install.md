# Installing Pixelion Captions on macOS

Feature: macos-platform-support, Task 6.4 (Requirements 6.7, 6.8)

Pixelion Captions for macOS ships as a signed and notarized `.plugin` bundle
(`pixelion-captions.plugin`) with bundle id `ai.pixelion.captions`, built as a
universal binary (arm64 + x86_64) for macOS 13.7 and later.

## Install directory

Copy `pixelion-captions.plugin` into one of:

- Per-user (recommended):
  `~/Library/Application Support/obs-studio/plugins/`
- System-wide (all users on the Mac):
  `/Library/Application Support/obs-studio/plugins/`

Create the `plugins` directory if it does not exist, then restart OBS Studio
31.1.1 or later. The plugin appears in the OBS source/plugin list once loaded.

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
