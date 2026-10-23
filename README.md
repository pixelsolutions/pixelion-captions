# Pixelion Captions

Pixelion Captions brings live transcription and translation to OBS Studio. It listens to the OBS program mix and displays captions in a source you can style and place in your scene. The dock includes language selection, session controls, transcript history, and optional Pixelion Library sync.

You need a Pixelion account and an internet connection to start a session. Audio is sent to Pixelion for processing after you agree to the audio processing notice. Translation availability depends on your plan.

The plugin currently targets Windows x64. macOS support is planned but not available yet.

## Install

Download the latest release and install it in OBS. Open **Docks → Pixelion Captions**, sign in, choose Transcribe or Translate, select your languages, and press **Start**. Add a **Pixelion Captions** source to your scene to show the captions.

## Project layout

Shared code and utilities are in `src/shared`. The Windows implementation is in `src/platform/windows`. The `src/platform/macos` folder is reserved for future macOS support.

## License

Pixelion Captions is licensed under GPL-2.0-or-later; see [LICENSE](LICENSE). Each binary release will include a matching source ZIP.

## Support

Report bugs on the [GitHub issue tracker](https://github.com/pixelsolutions/pixelion-captions/issues). Please leave out account tokens, private audio, and transcript contents.
