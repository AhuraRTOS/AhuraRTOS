"""
***************************************************************************************************
 * @file        install_rpi_online.py
 * @author      Nima Askari
 * @github      https://github.com/AhuraRTOS/AhuraRTOS
 * @version     1.0.0
 * @date        2026
 * @brief       Install AhuraRTOS into a Raspberry Pi Pico SDK project - one command.
 **************************************************************************************************

Run this from the root of your Pico SDK project - the directory holding the
top-level CMakeLists.txt and pico_sdk_import.cmake. It detects the chip from
PICO_BOARD / PICO_PLATFORM, selects the matching SoC package, copies the
application-owned files, adds the CMake block and wires up os_init() / os_start().
It prints the exact diff first and asks before writing anything.

    curl -fsSL https://raw.githubusercontent.com/AhuraRTOS/AhuraRTOS/main/tools/install_rpi_online.py | python3 -

Options go after the `-`: --dry-run shows the diff and writes nothing, --yes skips the prompt,
--update fetches the current kernel over the one already installed, --uninstall takes it back out,
--source DIR installs from a checkout you already have.

HOW IT WORKS

This file is a bootstrap, and deliberately small. Its whole job is to end up holding a real
AhuraRTOS checkout on disk, in one of four ways, and then to load the installer out of it:

    1. --source DIR             a checkout you point at
    2. beside this script       when run from inside a checkout, tools/ next to ahura.h
    3. the project's AhuraRTOS/ when the kernel is already installed and --update was not asked
    4. download it from GitHub

Steps 1 to 3 cost no network at all, which is what makes re-running free.

The installer itself - diffing, the >>> AhuraRTOS BEGIN blocks, rollback - lives in
tools/_ahura_install.py, and the raspberrypi-specific half in tools/_platforms/raspberrypi.py. Both are
loaded from the checkout found above, so there is exactly one copy of each in the repository no
matter how many platforms it supports.

Requires Python 3.8 or newer and nothing else - standard library only.

@copyright (c) 2026 Ahura Project Contributors
            See LICENSE in the project root for the full license text.
"""

import argparse
import contextlib
import importlib.util
import io
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

PLATFORM = "raspberrypi"
ONLINE = True

REPO = "AhuraRTOS/AhuraRTOS"
TARBALL = "https://codeload.github.com/{repo}/tar.gz/{ref}"

#: Names the engine must expose. A bootstrap and an engine always ship together, so a mismatch
#: means two versions were mixed - say so rather than failing later with an AttributeError.
ENGINE_API = ("Fatal", "run", "looks_like_ahura")
PLATFORM_API = ("NAME", "PROG", "DESCRIPTION", "detect", "add_arguments",
                "Project", "plan", "plan_uninstall")


class Fatal(Exception):
    """An error with a message already written for the user."""


def looks_like_ahura(path: Path) -> bool:
    """Whether `path` is the root of an AhuraRTOS checkout.

    ahura.h and kernel/ together, because either one alone is something a project might
    plausibly have of its own.
    """
    return (path / "ahura.h").is_file() and (path / "kernel").is_dir()


def has_installer(path: Path) -> bool:
    """Whether that checkout is new enough to carry the split installer."""
    return (path / "tools" / "_ahura_install.py").is_file() \
        and (path / "tools" / "_platforms" / (PLATFORM + ".py")).is_file()


def download(ref: str, into: str) -> Path:
    """Fetch the repository tarball and extract it under `into`."""
    url = TARBALL.format(repo=REPO, ref=ref)
    print("  downloading {} ...".format(url))
    try:
        with urllib.request.urlopen(url, timeout=60) as response:
            payload = response.read()
    except (urllib.error.URLError, OSError) as exc:
        raise Fatal(
            "could not download AhuraRTOS: {}\n"
            "  Clone it yourself and point the script at the clone instead:\n"
            "      git clone https://github.com/{}.git\n"
            "      python {} --source AhuraRTOS".format(exc, REPO, Path(sys.argv[0]).name))

    with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as tar:
        # A GitHub tarball has no absolute or parent paths, no symlinks and no hardlinks, but
        # this is archive extraction: check rather than trust. The link check matters for the
        # fallback below - filter="data" rejects escaping links itself, but on Python < 3.12
        # there is no filter, and a link member whose linkname points outside the directory is
        # the one way a member with a perfectly innocent name still writes outside `into`.
        for member in tar.getmembers():
            if member.name.startswith("/") or ".." in Path(member.name).parts:
                raise Fatal("refusing to extract unsafe path from tarball: " + member.name)
            if member.issym() or member.islnk():
                raise Fatal("refusing to extract link member from tarball: {} -> {}"
                            .format(member.name, member.linkname))
        try:
            tar.extractall(into, filter="data")   # filter= is 3.12+
        except TypeError:
            tar.extractall(into)

    roots = [p for p in Path(into).iterdir() if p.is_dir()]
    if len(roots) != 1 or not looks_like_ahura(roots[0]):
        raise Fatal("downloaded archive does not look like the AhuraRTOS repository")
    return roots[0]


@contextlib.contextmanager
def checkout(source: str, ref: str, project: Path, update: bool):
    """Yield a directory that is an AhuraRTOS checkout carrying the installer."""
    if source:
        given = Path(source).expanduser().resolve()
        if not looks_like_ahura(given):
            raise Fatal("--source {} is not an AhuraRTOS checkout "
                        "(no ahura.h in it)".format(source))
        yield given
        return

    # Running from inside a checkout - tools/<this file> beside ahura.h and kernel/.
    #
    # __file__ is not a reliable signal on its own: piped in as `python -` it is set to the
    # literal string "<stdin>", which resolves against the working directory and would point two
    # levels above the project. Requiring it to be an existing file rules that out.
    here = globals().get("__file__")
    if here and Path(here).is_file():
        local = Path(here).resolve().parent.parent
        if looks_like_ahura(local) and has_installer(local):
            yield local
            return

    # The kernel is already in the project. Re-running then costs no network at all, which is
    # what makes running this twice free. --update is how you ask for the current version.
    installed = project / "AhuraRTOS"
    if not update and looks_like_ahura(installed) and has_installer(installed):
        yield installed
        return

    with tempfile.TemporaryDirectory() as tmp:
        yield download(ref, tmp)


def load(path: Path, name: str, required, inject=None):
    """Import a module by path, check it is the one this bootstrap was written against.

    Loaded by path rather than by name so the module always comes from the SAME checkout the
    kernel is being installed from - a stale copy elsewhere on the machine cannot be picked up
    instead. The module name given here is not "__main__", so importing it does not run any
    entry point of its own.
    """
    if not path.is_file():
        raise Fatal(
            "{} is missing from the checkout.\n"
            "  The installer is split across tools/_ahura_install.py and tools/_platforms/,\n"
            "  so the checkout has to be complete. Re-download it.".format(path))

    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise Fatal("could not load {}".format(path))
    module = importlib.util.module_from_spec(spec)
    if inject:
        # The engine's public names, so a platform descriptor can use Fatal, SourceFile,
        # relative() and the rest without importing a module it was not loaded as.
        module.__dict__.update(inject)
    sys.modules[spec.name] = module

    # Import writes tools/__pycache__/ next to the module by default, which would leave a build
    # artefact inside the user's checkout - and, when that checkout is the tree being copied into
    # the project, ship it into their source control. Turned off across the import only, then put
    # back, since the setting is global and this script does not own it.
    bytecode = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    except Exception as exc:                                  # noqa: BLE001
        raise Fatal("could not load {}: {}".format(path, exc))
    finally:
        sys.dont_write_bytecode = bytecode

    missing = [n for n in required if not hasattr(module, n)]
    if missing:
        raise Fatal(
            "{} is not the version this file was written against (missing: {}).\n"
            "  Both come from the same repository - use the pair that shipped together\n"
            "  rather than mixing versions.".format(path, ", ".join(missing)))
    return module


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)

    # Only what choosing a checkout needs. The engine owns the real parser, and parses the same
    # argv again - so --help, unknown flags and every platform option are answered there, once.
    pre = argparse.ArgumentParser(add_help=False)
    pre.add_argument("--source")
    pre.add_argument("--ref", default="main")
    pre.add_argument("--project", default=".")
    pre.add_argument("--update", action="store_true")
    known, _ = pre.parse_known_args(argv)
    project = Path(known.project).expanduser().resolve()

    try:
        with checkout(known.source, known.ref, project, known.update) as repo:
            engine = load(repo / "tools" / "_ahura_install.py",
                          "ahura_install_engine", ENGINE_API)
            platform = load(repo / "tools" / "_platforms" / (PLATFORM + ".py"),
                            "ahura_platform_" + PLATFORM, PLATFORM_API,
                            inject={k: v for k, v in vars(engine).items()
                                    if not k.startswith("_")})
            return engine.run(platform, repo, argv)
    except Fatal as exc:
        print("\nerror: {}".format(exc), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ncancelled", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
