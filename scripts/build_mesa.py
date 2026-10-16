#!/usr/bin/env python3
"""Build devkitPro's switch-mesa with this project's patches on top.

The console's GL driver is Mesa (`switch-mesa`, a devkitPro portlib). The
port's frame is CPU-bound in draw calls -- about 11 us of driver work each on
the game's core -- and Mesa's glthread moves that work to another core, but
devkitPro's Switch EGL driver never starts glthread.
`patches/mesa/0001-switch-glthread.patch` makes it able to, opt-in through the
environment (`mesa_glthread=true`), which the port sets from `gl_thread` in
gta3_nx.cfg (source/main.c). The patches come from the torchlight port, where
they were written and measured first.

This script is devkitPro's own recipe - `switch/mesa/PKGBUILD` in
devkitPro/pacman-packages, pinned below - with that patch added:

    1. Mesa 20.1.0-rc3: upstream's commit `VERSION: bump to 20.1.0-rc3`,
       fetched by its hash from devkitPro's own mirror (github.com/devkitPro/mesa)
       - the rc3 tarball the PKGBUILD names is no longer on mesa.freedesktop.org,
       and the tree it held is that commit's. devkitPro's patches applied to it
       give exactly their `switch-20.1.0-rc3` branch;
    2. devkitPro's `switch-mesa-20.1.0-5.patch`, `gl_XML.py.patch` and
       `glX_XML.py.patch`, at the pinned commit, checked against the PKGBUILD's
       own sha256 sums;
    3. every `patches/mesa/*.patch`;
    4. `meson-cross.sh switch` and `ninja`, as the PKGBUILD does;
    5. installed under `mesa-install/`, where the Makefile picks it up in
       place of the portlib.

It needs a Linux shell with devkitPro installed - WSL, or devkitPro's own
`devkitpro/devkita64` container, which is what CI uses . devkitPro's
Windows MSYS2 shell has no Python, and Mesa generates half its sources with it.

    sudo dkp-pacman -S --needed switch-dev switch-libdrm_nouveau switch-zlib \\
        dkp-meson-scripts switch-pkg-config dkp-toolchain-vars
    sudo apt install bison flex ninja-build python3-pip && pip install mako meson

Usage:
    python3 scripts/build_mesa.py [--jobs N] [--keep-source]
"""
import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "build-mesa"           # download cache and source tree
INSTALL = ROOT / "mesa-install"      # what the Makefile looks for
DEVKITPRO = pathlib.Path(os.environ.get("DEVKITPRO", "/opt/devkitpro"))

MESA_VERSION = "20.1.0-rc3"
MESA_GIT = "https://github.com/devkitPro/mesa.git"
# Upstream Mesa, "VERSION: bump to 20.1.0-rc3" - the git hash is the pin.
MESA_COMMIT = "c49fbacd9444da2dbd4bbe7d574808f8f7c29af6"

# devkitPro/pacman-packages, the commit that last touched switch/mesa
# (switch-mesa 20.1.0-5). The sums are the PKGBUILD's own.
DKP_COMMIT = "f103fe88e37180ecd0b7a9173b52b7580a54f71a"
DKP_BASE = f"https://raw.githubusercontent.com/devkitPro/pacman-packages/{DKP_COMMIT}/switch/mesa"
DKP_PATCHES = [
    ("switch-mesa-20.1.0-5.patch", "950f93d3e5b6ae9c5a42c2918623fe9a80d7ee398d92d2e73abec86b62d75916"),
    ("gl_XML.py.patch", "a9bc326195b3fe29709e079466a8b2162a2ac9409f694eaad5494490155e2dd4"),
    ("glX_XML.py.patch", "1475defcdf8600690eaddeee28d8f01310635c1273fb541f668e8789714d36ca"),
]


def fetch(url: str, dest: pathlib.Path, sha256: str) -> pathlib.Path:
    """Download once into the cache, and refuse anything but the pinned bytes."""
    if not dest.exists():
        print(f"fetching {url}")
        with urllib.request.urlopen(url) as response, open(dest.with_suffix(".part"), "wb") as out:
            shutil.copyfileobj(response, out)
        dest.with_suffix(".part").rename(dest)
    digest = hashlib.sha256(dest.read_bytes()).hexdigest()
    if digest != sha256:
        dest.unlink()
        raise SystemExit(f"{dest.name}: sha256 {digest}, expected {sha256}")
    return dest


def run(command, cwd, env=None) -> None:
    print("+ " + " ".join(str(c) for c in command))
    if subprocess.run([str(c) for c in command], cwd=cwd, env=env).returncode != 0:
        raise SystemExit(f"failed: {command[0]}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--keep-source", action="store_true",
                    help="reuse an already patched source tree instead of unpacking afresh")
    args = ap.parse_args()

    if os.name == "nt":
        print("build_mesa.py needs Linux (WSL or the devkitpro/devkita64 container); see its docstring.")
        return 1
    cross = DEVKITPRO / "meson-cross.sh"
    if not cross.exists():
        print(f"no {cross} - install dkp-meson-scripts (see this script's docstring)")
        return 1

    cache = OUT / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    dkp = [fetch(f"{DKP_BASE}/{name}", cache / name, digest) for name, digest in DKP_PATCHES]
    ours = sorted((ROOT / "patches" / "mesa").glob("*.patch"))

    source = OUT / f"mesa-{MESA_VERSION}"
    if not (args.keep_source and source.exists()):
        shutil.rmtree(source, ignore_errors=True)
        source.mkdir(parents=True)
        run(["git", "init", "-q"], cwd=source)
        run(["git", "fetch", "-q", "--depth", "1", MESA_GIT, MESA_COMMIT], cwd=source)
        run(["git", "checkout", "-q", "FETCH_HEAD"], cwd=source)
        for patch in dkp + ours:
            run(["patch", "-p1", "-i", patch], cwd=source)

    # As the PKGBUILD: the cross file is written next to the source tree.
    build = source / "build"
    if not (build / "build.ninja").exists():
        run([cross, "switch", "../crossfile.txt", "build", "-Db_ndebug=true"], cwd=source)
    ninja = ["ninja", "-C", "build"] + (["-j", str(args.jobs)] if args.jobs else [])
    run(ninja, cwd=source)

    install = INSTALL
    shutil.rmtree(install, ignore_errors=True)
    run(["ninja", "-C", "build", "install"], cwd=source, env={**os.environ, "DESTDIR": str(install)})

    lib = install / DEVKITPRO.relative_to("/") / "portlibs" / "switch" / "lib"
    missing = [name for name in ("libEGL.a", "libGLESv2.a", "libglapi.a") if not (lib / name).exists()]
    if missing:
        print(f"built, but {lib} has no {', '.join(missing)}")
        return 1
    # What the port relies on, read off the built archives: the flag it detects
    # the patched driver by (source/imports.c), and glthread's entry points,
    # which devkitPro's build leaves out - a patch that silently stopped
    # applying would otherwise build a library the game runs without a thread.
    nm = DEVKITPRO / "devkitA64" / "bin" / "aarch64-none-elf-nm"
    symbols = ["mesa_switch_glthread_active", "switch_st_set_background_context",
               "st_start_thread", "_mesa_glthread_init"]
    if nm.exists():
        # Across the three archives: which of them carries the state tracker is
        # the build's business; the port links all three.
        defined = set()
        for archive in ("libEGL.a", "libGLESv2.a", "libglapi.a"):
            listing = subprocess.run([str(nm), str(lib / archive)], capture_output=True,
                                     text=True).stdout
            for line in listing.splitlines():
                fields = line.split()
                if len(fields) == 3 and fields[1] in "TtDdBbRr":
                    defined.add(fields[2])
        absent = [symbol for symbol in symbols if symbol not in defined]
        if absent:
            print(f"the built archives define no {', '.join(absent)} - the glthread patch did not take")
            return 1
        print(f"glthread symbols present: {', '.join(symbols)}")
    else:
        print(f"no {nm}; the built archives were not checked for the glthread symbols")

    print(f"\npatched switch-mesa in {lib}")
    print("The Makefile links it in place of the portlib from the next build (make clean first).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
