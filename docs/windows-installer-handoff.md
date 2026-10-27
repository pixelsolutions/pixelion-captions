# Handoff prompt: Windows installer worker

Paste everything below the line into the agent that will work on a Windows machine.

---

You are picking up the Windows installer for the Pixelion Captions OBS plugin
(repo: pixelsolutions/pixelion-captions, public). Work on branch
`release/macos-validation` (it is also the local branch name
`feature/macos-build-validation` on the macOS dev machine). Do not push to
`main`, do not tag, and do not create a GitHub Release without asking the user.

## Current state

- The macOS port is finished and verified: signed, notarized `.pkg`, plugin
  tested in OBS on macOS (login and live transcription work). Do not touch
  macOS files, `src/`, or plugin behavior. This task is Windows packaging only.
- Windows previously shipped only a `.zip` made by
  `.github/scripts/Package-Windows.ps1`.
- A first version of the Inno Setup installer is committed and builds in CI
  (commits `ecfde86` and `21bce0b`). CI run 40420357081 passed and produced the
  artifact `pixelion-captions-0.1.1-windows-x64-<commit>` containing
  `pixelion-captions-0.1.1-windows-x64-installer.exe` (about 3.3 MB) and the
  `.zip`. The installer has NEVER been installed or run on a real machine.

## Files

- `.github/scripts/windows-installer.iss`: Inno Setup 6 script. Defines passed by
  iscc: `AppVersion`, `SourceDir`, `OutputDir`, `OutputName`, optional
  `LicenseFile`.
- `.github/scripts/Package-Windows.ps1`: makes the zip, installs Inno Setup
  6.4.3 via Chocolatey if missing, runs iscc, checks the exit code, and has a
  commented Authenticode extension point (signing is NOT implemented).
- `.github/workflows/build-project.yaml`: the Windows job uploads both the zip
  and the installer exe, and fails if either is missing.
- `.github/workflows/push.yaml`: the `create-release` job already handles
  `windows-x64;zip|exe`, so a version tag will attach the installer.
- Version comes from `buildspec.json` (single shared version for Windows and
  macOS, currently 0.1.1). Never hardcode it.

## Intended installer behavior

- Finds OBS from the registry (`HKLM\SOFTWARE\OBS Studio`, 64-bit view then
  WOW6432Node), falls back to `{commonpf64}\obs-studio`, lets the user change
  it, and warns if `bin\64bit\obs64.exe` is not there.
- Installs `obs-plugins\64bit\*` and `data\obs-plugins\pixelion-captions\*`
  relative to the OBS folder. 64-bit only. Admin required.
- Fixed AppId GUID `DEDBD78D-410C-4108-A391-FB5F214D703E`: do not change it, it
  is what lets a newer installer upgrade in place.
- Blocks while OBS is running (start, before copy, and in the uninstaller).
- Never touches `%APPDATA%\obs-studio\plugin_config\pixelion-captions`.
- Installer is unsigned, so SmartScreen shows "unknown publisher".

## What to do first (manual test on a clean Windows machine)

1. Download the artifact from the CI run page (Actions > run 40420357081 >
   Artifacts) or build a newer one.
2. Run the installer with OBS closed. Check: OBS folder detected, files land in
   the right places, plugin appears in OBS and loads (look at the OBS log under
   `%APPDATA%\obs-studio\logs`), login and live transcription work.
3. Run the same installer again: should upgrade in place with no errors.
4. Run it while OBS is open: should refuse until OBS is closed.
5. Uninstall from Add/Remove Programs: plugin files removed, settings in
   `plugin_config\pixelion-captions` kept.
6. If anything fails, fix `windows-installer.iss` or `Package-Windows.ps1`,
   push to `release/macos-validation`, and check the Windows job in the Actions
   run (every push there triggers a Windows build, free because the repo is
   public).

## Known weak spots

- Running the installer silently (`/SILENT`, `/VERYSILENT`) while OBS is open
  may loop forever on the "close OBS" prompt, because silent message boxes
  return their default. Fix: abort when `WizardSilent` (setup) or
  `UninstallSilent` (uninstall) is true.
- Install paths in the .iss were derived by reading the CMake and packaging
  scripts, not by inspecting a built tree. Confirm against the real zip layout.
- Authenticode signing is not implemented (needs a paid certificate or Azure
  Trusted Signing). The hook is gated on `WINDOWS_SIGN_CERT` and
  `WINDOWS_SIGN_PASSWORD`.

## Rules

- Windows plugin behavior must not change. Keep edits to the three files above.
- Ask the user before pushing to `main`, tagging, or publishing a release.
- Do not create summary or report files beyond what the user asks for.
