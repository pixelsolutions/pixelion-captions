# Wiki source

These markdown files are the source for the project's GitHub Wiki. They live in the
repo so they are reviewable and version-controlled; the live wiki is a separate git
repository (`pixelion-captions.wiki.git`).

## Pages

| File | Wiki page |
|------|-----------|
| `Home.md` | Home (landing page) |
| `Installing-on-Windows.md` | Installing on Windows |
| `Installing-on-macOS.md` | Installing on macOS |
| `_Sidebar.md` | Navigation sidebar (GitHub renders this automatically) |

GitHub wikis map a page title to a file by replacing spaces with hyphens, so
`Installing-on-Windows.md` becomes the page "Installing on Windows" and the link
`[Installing on Windows](Installing-on-Windows)` resolves correctly.

## Screenshots

Screenshots are committed in the main repo and referenced from the wiki pages via
`raw.githubusercontent.com/.../main/...` URLs, so the images render in the wiki
without copying files into the separate wiki repo.

**Windows installer screenshots** live in `docs/wiki/images/` and are already
wired into `Installing-on-Windows.md`:

| File | Step |
|------|------|
| `mswin_exe_01.jpg` | License agreement |
| `mswin_exe_02.jpg` | Select Destination Location (detected OBS folder) |
| `mswin_exe_03.jpg` | Ready to Install |
| `mswin_exe_04.jpg` | Completing the Setup Wizard |
| `mswin_exe_05.jpg` | OBS Docks menu + the dock open |

The dock screenshot on the Home/macOS pages reuses the published
`docs/screenshots/obs_pixelion.png`.

**macOS screenshots** are not captured yet. `Installing-on-macOS.md` still has
commented-out `<!-- SCREENSHOT: ... -->` markers for the ones to add (Releases
download, plugins folder, Docks menu, Keychain prompt). When ready, drop them in a
repo-tracked folder and reference them the same way (raw URL), then uncomment the
matching `![...]()` line.

> Note: the raw URLs point at the `main` branch, so images resolve once this branch
> is merged. Before merge, preview from the branch that holds the assets.

## Publishing to the GitHub Wiki

The wiki repo must be initialized first: on GitHub, open the repo's **Wiki** tab and
create any page once (this creates `pixelion-captions.wiki.git`). Then:

```bash
git clone https://github.com/pixelsolutions/pixelion-captions.wiki.git
# copy Home.md, Installing-on-Windows.md, Installing-on-macOS.md, _Sidebar.md
# and the images/ folder into the clone
cd pixelion-captions.wiki
git add .
git commit -m "Add Windows and macOS installation pages"
git push
```

Alternatively, paste each page's contents into the matching page in the GitHub wiki
web editor and attach the screenshots there.
