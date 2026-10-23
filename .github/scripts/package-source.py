"""Package the committed source for a matching Pixelion Captions binary release."""

import argparse
import io
import json
from pathlib import Path
import re
import subprocess
import zipfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE_PATHS = (
    ".github", "build-aux", "cmake", "data", "resources", "src",
    ".clang-format", ".gersemirc", ".gitignore", "buildspec.json",
    "CMakeLists.txt", "CMakePresets.json", "LICENSE", "README.md",
)
REQUIRED = (
    "buildspec.json", "CMakeLists.txt", "CMakePresets.json", "LICENSE",
    "README.md", "src/plugin-main.cpp",
    "resources/pixelion.qrc", "resources/pixelion-logo.png",
)


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref", default="HEAD", help="Commit used to build the release binary")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "release")
    args = parser.parse_args()
    revision = git("rev-parse", "--verify", "--end-of-options", args.ref + "^{commit}").decode().strip()
    tracked = set(git("ls-tree", "-r", "--name-only", revision).decode().splitlines())
    missing = [path for path in REQUIRED if path not in tracked]
    if missing:
        parser.error("Release commit is missing required source files: " + ", ".join(missing))
    spec = json.loads(git("show", revision + ":buildspec.json"))
    version = spec["version"]
    if not isinstance(version, str) or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?", version):
        parser.error("Invalid release version in buildspec.json")
    prefix = f"pixelion-captions-{version}-source/"
    paths = [path for path in SOURCE_PATHS if any(p == path or p.startswith(path + "/") for p in tracked)]
    archive = io.BytesIO(git("archive", "--format=zip", "--prefix=" + prefix, revision, "--", *paths))
    with zipfile.ZipFile(archive, "a", compression=zipfile.ZIP_DEFLATED) as source_zip:
        # Fail if export attributes omitted files needed to rebuild the plugin.
        missing = [path for path in REQUIRED if prefix + path not in source_zip.namelist()]
        if missing:
            parser.error("Source archive is missing required files: " + ", ".join(missing))
        source_zip.writestr(prefix + "SOURCE_REVISION.txt", f"Pixelion Captions {version}\nCommit: {revision}\n")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    destination = args.output_dir / f"pixelion-captions-{version}-source.zip"
    destination.write_bytes(archive.getvalue())
    print(destination)


if __name__ == "__main__":
    main()
