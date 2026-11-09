## Pixelion Captions 0.2.0

This release adds a guest mode with a free sample, a clearer account section, and add-on credits for Pro users. It also fixes feedback submission from the dock.

### Changes
- Welcome screen: when you open the dock, choose Create free account, Sign in, or Continue as guest.
- Guest mode: try live captions with a free allowance of 10 minutes, transcription only. The dock shows how many minutes you have left. When they run out, captions stop, your transcript stays saved on your computer, and Create account is offered. Guest mode is not kept after you restart OBS.
- Account card: always visible at the top of the dock, with a plan badge (Guest, Free or Pro) and a progress bar for your plan minutes. The usage line shows minutes used of your allowance.
- Free plan: Upgrade to Pro is always visible. A warning appears in the account card when you reach 80 percent of your allowance.
- Pro plan: when you have add-on credits, a second bar shows the minutes available from your plan plus credits. Add Credits is a link under Additional credits.
- Session limit: the longest single session now follows the limit reported for your plan.
- Settings: links to your Pixelion account page and to manage your Stripe subscription. Manage subscription opens the Stripe billing portal for your own account.
- Feedback: Send Feedback now submits to the Pixelion API, so reports arrive in the same place as feedback from the web and the desktop app.

### Install
1. Close OBS Studio.
2. Download the package for your platform below. The source ZIP is not needed to run the plugin.
   - Windows: run pixelion-captions-0.2.0-windows-x64-installer.exe. The plugin ZIP is available if you prefer a manual install. Step-by-step guide: [Installing on Windows](https://github.com/pixelsolutions/pixelion-captions/wiki/Installing-on-Windows)
   - macOS: open pixelion-captions-0.2.0-macos-universal.pkg and follow the steps. Step-by-step guide: [Installing on macOS](https://github.com/pixelsolutions/pixelion-captions/wiki/Installing-on-macOS)
3. Open OBS Studio, open the Docks menu, and select Pixelion Captions.

Guest mode needs no account. A free Pixelion.ai account is needed for translation, Pixelion Library saving and add-on credits. Translation requires a Pixelion Pro plan.

See the README and the install guides in the wiki for full usage instructions and requirements. Tested with OBS Studio 32.2.2.
