"""
***************************************************************************************************
 * @file        install_rpi_offline.py
 * @author      Nima Askari
 * @github      https://github.com/AhuraRTOS/AhuraRTOS
 * @version     1.0.0
 * @date        2026
 * @brief       Install AhuraRTOS into a Raspberry Pi Pico SDK project - from a local checkout.
 **************************************************************************************************

The offline twin of install_rpi_online.py: the same six steps, from a copy of the
repository already on disk. It imports no networking module at all, so being
offline is a property of the file rather than a promise in a comment.

    python3 AhuraRTOS/tools/install_rpi_offline.py

Options go after the `-`: --dry-run shows the diff and writes nothing, --yes skips the prompt,
--update fetches the current kernel over the one already installed, --uninstall takes it back out,
--source DIR installs from a checkout you already have.

HOW IT WORKS

This file is a bootstrap, and deliberately small. Its whole job is to end up holding a real
AhuraRTOS checkout on disk, in one of four ways, and then to load the installer out of it:

    1. --source DIR             a checkout you point at
    2. beside this script       when run from inside a checkout, tools/ next to ahura.h
    3. the project's AhuraRTOS/ when the kernel is already installed and --update was not asked
    4. nothing - this installer never reaches the network

If none of the first three finds one, it stops and says how to get a copy.

The installer itself - diffing, the >>> AhuraRTOS BEGIN blocks, rollback - lives in
tools/internal/engine.py, and the raspberrypi-specific half in tools/internal/raspberrypi.py. Both are
loaded from the checkout found above, so there is exactly one copy of each in the repository no
matter how many platforms it supports.

Requires Python 3.8 or newer and nothing else - standard library only.

@copyright (c) 2026 Ahura Project Contributors
            See LICENSE in the project root for the full license text.
"""

import argparse
import contextlib
import importlib.util
import os
import sys
from pathlib import Path

PLATFORM = "raspberrypi"
ONLINE = False

REPO = "AhuraRTOS/AhuraRTOS"
TARBALL = "https://codeload.github.com/{repo}/tar.gz/{ref}"

#: Names the engine must expose. A bootstrap and an engine always ship together, so a mismatch
#: means two versions were mixed - say so rather than failing later with an AttributeError.
ENGINE_API = ("Fatal", "run", "looks_like_ahura")
PLATFORM_API = ("NAME", "PROG", "DESCRIPTION", "detect", "add_arguments",
                "Project", "plan", "plan_uninstall")


#: ENABLE_VIRTUAL_TERMINAL_PROCESSING, and the handle to ask for it on. A Windows console renders
#: ANSI only once that bit is set; without it the escapes print as literal junk, which is worse
#: than no colour at all.
_WIN_VT_MODE = 0x0004
_WIN_STDERR = -12


def colour_supported() -> bool:
    """Whether an escape written to stderr will come out as colour rather than as text.

    Three ways it will not, and all three are ordinary rather than exceptional: the output is
    redirected to a file or a pipe, NO_COLOR is set (https://no-color.org, the convention every
    tool that colours anything should honour), or this is a Windows console that has not had
    virtual-terminal processing switched on. The last one is asked for here rather than assumed,
    and a refusal means no colour, never a traceback - a tool that cannot colour its error still
    has to be able to print it.
    """
    if os.environ.get("NO_COLOR"):
        return False

    stream = getattr(sys, "stderr", None)

    if stream is None or not hasattr(stream, "isatty") or not stream.isatty():
        return False

    if os.name != "nt":
        return True

    try:
        import ctypes

        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(_WIN_STDERR)
        mode = ctypes.c_uint32()

        if kernel32.GetConsoleMode(handle, ctypes.byref(mode)) == 0:
            return False

        return kernel32.SetConsoleMode(handle, mode.value | _WIN_VT_MODE) != 0
    except Exception:            # noqa: BLE001 - any failure here means "no colour", nothing more
        return False


def print_error(message: str):
    """Write `message` to stderr as the failure the run ended on, in red where that will show.

    Red for the whole block, not just the label. These messages are several lines - what is wrong,
    then what to do about it - and the part a user most needs to read is the fix at the bottom,
    which is exactly the part a coloured prefix leaves looking like ordinary output.
    """
    print("\n" + (RED + message + RESET if colour_supported() else message), file=sys.stderr)


RED = "\033[31m"
RESET = "\033[0m"


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
    return (path / "tools" / "internal" / "engine.py").is_file() \
        and (path / "tools" / "internal" / (PLATFORM + ".py")).is_file()


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

    raise Fatal(
        "no AhuraRTOS checkout found, and this is the offline installer - it never\n"
        "  reaches the network.\n"
        "\n"
        "  Get the repository on a machine that has a connection:\n"
        "      git clone https://github.com/{}.git\n"
        "  or download the ZIP from https://github.com/{}\n"
        "\n"
        "  Then copy it into your project and run this from the project root, or\n"
        "  point at it directly:\n"
        "      python {} --source /path/to/AhuraRTOS".format(
            REPO, REPO, Path(sys.argv[0]).name))


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
            "  The engine and the platform descriptors live in tools/internal/,\n"
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
            engine = load(repo / "tools" / "internal" / "engine.py",
                          "ahura_install_engine", ENGINE_API)
            platform = load(repo / "tools" / "internal" / (PLATFORM + ".py"),
                            "ahura_platform_" + PLATFORM, PLATFORM_API,
                            inject={k: v for k, v in vars(engine).items()
                                    if not k.startswith("_")})
            return engine.run(platform, repo, argv,
                              title="AhuraRTOS installer (offline)", show_kernel=True)
    except Fatal as exc:
        print_error("error: {}".format(exc))
        return 1
    except KeyboardInterrupt:
        print("\ncancelled", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
