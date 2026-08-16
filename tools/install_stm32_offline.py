#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Install AhuraRTOS into an STM32CubeMX-generated CMake project, offline.

The companion to install_stm32.py, for a machine with no internet access - an
air-gapped lab, a locked-down corporate network, a build agent with no route
out. It performs exactly the same six-step installation and produces a
byte-identical result; the only difference is where the kernel comes from.

    install_stm32.py           downloads the repository when it cannot find one
    install_stm32_offline.py   only ever uses a copy already on this machine

Nothing here imports urllib, tarfile or socket, so "offline" is a property of
the file rather than a promise in a comment.

USAGE

Download the repository on a machine that has a connection (git clone, or the
"Download ZIP" button on GitHub), copy it into the root of your CubeMX project,
and run it:

    MyProject/
    |- CMakeLists.txt          <- the top-level one CubeMX generated
    |- MyProject.ioc
    |- Core/
    |- AhuraRTOS/              <- the repository you copied in
       |- kernel/
       |- tools/install_stm32_offline.py

    cd MyProject
    python3 AhuraRTOS/tools/install_stm32_offline.py

That is the whole procedure. The script finds the kernel beside itself, reads
the project, prints the diff and asks before writing anything.

A ZIP download unpacks as AhuraRTOS-main/ rather than AhuraRTOS/, and that
works too - it is found, and installed to AhuraRTOS/ so the CMake block and the
online installer agree on one path. Copying this one file to the project root
instead of running it in place works as well; it looks for the kernel next to
itself first, then in the project.

Options are the same as the online installer, minus --ref (nothing to fetch):

    --dry-run            print the diff and exit, writing nothing
    --yes                apply without asking
    --uninstall          take the integration back out
    --source PATH        use this checkout, wherever it is
    --project DIR        project root (default: the current directory)
    --app-dir DIR        which source tree to install into, on a dual-core part
    --tick external      drive os_tick_handler() from your own timer
    --update             refresh AhuraRTOS/ in the project from --source
    --force-templates    overwrite an existing os_config.h / os_cb.c / os_main.c

HOW IT RELATES TO install_stm32.py

Everything that reads the project, computes the edits, prints the diff and
writes with rollback lives in install_stm32.py and is imported from there. This
file contains only what is genuinely different: finding a local checkout, and
the rule below. Duplicating the editing logic would mean two installers that
drift apart, and the one nobody runs on a network-connected machine is the one
that would rot.

THE RULE THIS FILE EXISTS TO ENFORCE

Offline, the source and the destination are routinely the SAME directory: the
repository is already at <project>/AhuraRTOS, which is exactly where the
installer would otherwise copy it. Copying a directory onto itself is not a
copy - the destination is cleared first, so it destroys the source. This script
therefore treats "already in place" as nothing to do, and refuses any copy
whose source and destination resolve to one path.

Requires Python 3.8 or newer and nothing else. Runs the same on Windows, macOS
and Linux.
"""

import argparse
import importlib.util
import sys
from pathlib import Path

# Where the repository is installed inside the project. Not configurable, and
# deliberately: install_stm32.py hardcodes the same name when it writes the
# CMake block, so a second path here would produce a project the online
# installer could no longer recognise or uninstall.
INSTALL_DIRNAME = "AhuraRTOS"

# Directory names a downloaded repository plausibly arrives under. "AhuraRTOS"
# is a git clone; the rest are what GitHub's ZIP produces for a branch or tag.
SOURCE_GLOB = "AhuraRTOS*"


class Fatal(Exception):
    """A problem the user has to fix before anything can be written.

    Declared here as well as in install_stm32.py because the repository has to
    be located before that module can be imported from it - so this file needs
    a way to fail before it has the shared one. resolve_module() replaces it
    with the shared class the moment one is available, and everything raised
    after that point is caught by the same handler.
    """


def looks_like_ahura(path: Path) -> bool:
    """A usable AhuraRTOS tree: the kernel and the templates that get copied.

    Deliberately the same two-file test install_stm32.py uses. It is duplicated
    rather than imported for the reason above - this is what decides WHICH tree
    to import from - and it is two lines that cannot drift meaningfully.
    """
    return (path.is_dir()
            and (path / "kernel" / "ahura.h").is_file()
            and (path / "kernel" / "template" / "os_config.h").is_file())


def find_repo(source: str, root: Path, script: Path) -> Path:
    """Locate an AhuraRTOS checkout on this machine. Never touches the network.

    Searched in the order a user would expect to win, most explicit first:

      1. --source, if given. An explicit path that is not a checkout is an
         error, never a reason to go looking elsewhere - silently installing
         some other copy is how the wrong kernel version ends up in a build.
      2. The checkout this script is running from. Running
         AhuraRTOS/tools/install_stm32_offline.py means the kernel beside it is
         the one being asked for.
      3. <project>/AhuraRTOS - already installed, or copied in by hand.
      4. <project>/AhuraRTOS* - a ZIP download still under its branch name.
      5. Beside the project, one level up, under either name. Common when
         several projects share one downloaded copy.
    """
    if source:
        given = Path(source).expanduser().resolve()
        if not given.exists():
            raise Fatal("--source {} does not exist".format(source))
        if not looks_like_ahura(given):
            raise Fatal(
                "--source {} is not an AhuraRTOS checkout (no kernel/ahura.h and\n"
                "  kernel/template/os_config.h in it). If you unpacked a ZIP, point at\n"
                "  the directory that CONTAINS kernel/, not at the one above it."
                .format(source))
        return given

    candidates = []

    # The tree this file lives in: tools/install_stm32_offline.py -> repo root.
    # Only when __file__ is a real file on disk: piped into Python it is the
    # literal string "<stdin>" and would resolve against the working directory.
    if script.is_file():
        candidates.append(script.resolve().parent.parent)

    candidates.append(root / INSTALL_DIRNAME)
    candidates.extend(sorted(root.glob(SOURCE_GLOB)))
    candidates.extend(sorted(root.parent.glob(SOURCE_GLOB)))

    seen = set()
    for candidate in candidates:
        try:
            key = candidate.resolve()
        except OSError:
            continue
        if key in seen:
            continue
        seen.add(key)
        if looks_like_ahura(key):
            return key

    raise Fatal(
        "no AhuraRTOS checkout found on this machine, and this installer never\n"
        "  downloads one. Looked in:\n"
        "      {script}\n"
        "      {here}\n"
        "      {beside}\n"
        "  Get the repository on a machine that has a connection:\n"
        "      git clone https://github.com/AhuraRTOS/AhuraRTOS.git\n"
        "  or download the ZIP from https://github.com/AhuraRTOS/AhuraRTOS\n"
        "  copy it into this project, and re-run. Point at it explicitly with\n"
        "  --source PATH if you keep it somewhere else."
        .format(script=script.resolve().parent.parent if script.is_file() else "(this script)",
                here=root / (INSTALL_DIRNAME + "*"),
                beside=root.parent / (INSTALL_DIRNAME + "*")))


def resolve_module(repo: Path):
    """Import install_stm32.py out of the located checkout.

    Loaded by path rather than by name so it is found whether this script is
    run from tools/ or was copied to the project root, and so the module always
    comes from the SAME checkout the kernel is being installed from - a stale
    copy left elsewhere on the machine cannot be picked up instead.

    The module name given here is not "__main__", so importing it does not run
    its own entry point.
    """
    global Fatal

    module_path = repo / "tools" / "install_stm32.py"
    if not module_path.is_file():
        raise Fatal(
            "{} has no tools/install_stm32.py in it.\n"
            "  The offline installer reuses that file for everything except finding\n"
            "  the kernel, so the checkout has to be complete. Re-download it."
            .format(repo))

    spec = importlib.util.spec_from_file_location("ahura_install_stm32", module_path)
    if spec is None or spec.loader is None:
        raise Fatal("could not load {}".format(module_path))

    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module

    # Import writes tools/__pycache__/ next to the module by default, which
    # would leave a build artefact inside the user's downloaded repository -
    # and, when that repository is the tree being copied into the project, ship
    # it into their source control. Turned off across the import only, then put
    # back, since the setting is global and this script does not own it.
    bytecode = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    except Exception as exc:                                  # noqa: BLE001
        raise Fatal("could not load {}: {}".format(module_path, exc))
    finally:
        sys.dont_write_bytecode = bytecode

    required = ("Fatal", "Project", "plan", "plan_uninstall", "finish",
                "looks_like_ahura", "relative")
    missing = [name for name in required if not hasattr(module, name)]
    if missing:
        raise Fatal(
            "{} is not the installer this file was written against (missing: {}).\n"
            "  Both files come from the same repository - use the pair that shipped\n"
            "  together rather than mixing versions."
            .format(module_path, ", ".join(missing)))

    # From here on, raise the shared class: main()'s handler catches that one,
    # and the two must not diverge into "some errors print nicely, some don't".
    Fatal = module.Fatal
    return module


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="install_stm32_offline.py",
        description="Install AhuraRTOS into an STM32CubeMX-generated CMake project "
                    "from a local copy of the repository. Never uses the network. "
                    "Run it from the project root; it never touches the .ioc.")
    parser.add_argument("--project", default=".", metavar="DIR",
                        help="project root (default: the current directory)")
    parser.add_argument("--app-dir", metavar="DIR",
                        help="which source tree to install into, on a dual-core part")
    parser.add_argument("--source", metavar="PATH",
                        help="the AhuraRTOS checkout to install from (default: found "
                             "beside this script, then in or next to the project)")
    parser.add_argument("--tick", choices=("systick", "external"), default="systick",
                        help="tick source (default: systick)")
    parser.add_argument("--update", action="store_true",
                        help="refresh an AhuraRTOS/ already in the project from the "
                             "source checkout (otherwise it is left as it is)")
    parser.add_argument("--force-templates", action="store_true",
                        help="overwrite an existing os_config.h / os_cb.c / os_main.c")
    parser.add_argument("--dry-run", action="store_true",
                        help="show the diff and exit without writing")
    parser.add_argument("-y", "--yes", action="store_true",
                        help="do not ask for confirmation")
    parser.add_argument("--uninstall", action="store_true",
                        help="remove the managed blocks and the AhuraRTOS directory")
    args = parser.parse_args(argv)

    root = Path(args.project).expanduser().resolve()
    script = Path(globals().get("__file__", "<stdin>"))

    try:
        # Uninstall needs no source at all - it only removes managed blocks and
        # the installed directory - so it must not fail on a machine where the
        # original checkout has since been deleted. It is answered first, from
        # the project's own copy, before any search runs.
        if args.uninstall:
            module = resolve_module(_repo_for_uninstall(root, script, args.source))
            project = module.Project(root, args.app_dir)
            _banner(module, project, root, None)

            edits = module.plan_uninstall(project)
            notes = []
            installed = root / INSTALL_DIRNAME
            if module.looks_like_ahura(installed):
                notes.append("{}/ will be deleted".format(INSTALL_DIRNAME))
            notes.append("your os_config.h, os_cb.c and os_main.c are left alone - "
                         "they are your files now")
            if not edits and not notes:
                print("Nothing installed here.")
                return 0
            return module.finish(args, project, root, edits, [], notes)

        repo = find_repo(args.source, root, script)
        module = resolve_module(repo)

        project = module.Project(root, args.app_dir)
        _banner(module, project, root, repo)
        project.check(args.tick)

        destination = root / INSTALL_DIRNAME
        in_place = _same_path(repo, destination)
        installed = module.looks_like_ahura(destination)

        # The rule this file exists for. Three distinct situations, and only one
        # of them is a copy:
        #
        #   in_place            the checkout IS the destination. Nothing to
        #                       copy, and copying would clear the destination
        #                       first and so destroy the source. Not a copy.
        #   installed, no flag  a different checkout exists, but the project
        #                       already carries one. Left alone, so a tree
        #                       pinned deliberately is not silently replaced.
        #                       --update is how to ask for the other one.
        #   otherwise           a real copy into the project.
        copy_tree = (not in_place) and ((not installed) or args.update)

        edits, copies, notes = module.plan(
            project, repo, args.tick, args.force_templates, copy_tree=copy_tree)

        # When nothing is copied, plan() explains it in terms of DOWNLOADING a
        # current version - which is the one thing this installer will never
        # do. Its note is dropped and replaced below with what actually
        # happened. Matched on a phrase rather than the full sentence so a
        # reworded upstream note costs a duplicate line, not a crash.
        if not copy_tree:
            notes = [note for note in notes if "already in the project" not in note]

        if in_place:
            notes.append("the kernel is already at {}/ - installed in place, nothing copied"
                         .format(INSTALL_DIRNAME))
            if args.update:
                notes.append("--update has nothing to do here: the source and the "
                             "destination are the same directory")
        elif installed and not copy_tree:
            notes.append("{}/ is already in the project - left as it is "
                         "(--update replaces it from {})"
                         .format(INSTALL_DIRNAME, module.relative(repo, root)))

        if not edits and not copies:
            print("Already installed, and everything is where it should be.")
            return 0

        return module.finish(args, project, root, edits, copies, notes)

    except Fatal as exc:
        print("\nerror: {}".format(exc), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ncancelled", file=sys.stderr)
        return 130


def _repo_for_uninstall(root: Path, script: Path, source: str) -> Path:
    """A checkout to load install_stm32.py from, for --uninstall only.

    Uninstalling must keep working after the source has been deleted, so this
    accepts anything that can supply the module and says so plainly when it
    cannot, rather than reporting "no kernel found" for an operation that does
    not need one.
    """
    for candidate in (Path(source).expanduser() if source else None,
                      script.resolve().parent.parent if script.is_file() else None,
                      root / INSTALL_DIRNAME):
        if candidate is not None and (candidate / "tools" / "install_stm32.py").is_file():
            return candidate

    raise Fatal(
        "cannot uninstall: no copy of tools/install_stm32.py to work from.\n"
        "  Uninstalling only edits your project, but it reuses that file to do it.\n"
        "  Point at any AhuraRTOS checkout with --source PATH, or remove the blocks\n"
        "  by hand - each one is marked '>>> AhuraRTOS BEGIN' ... '<<< AhuraRTOS END'.")


def _same_path(left: Path, right: Path) -> bool:
    """Whether two paths name one directory, following links and case rules.

    os.path.samefile is the reliable test - it compares what the filesystem
    resolves to, so a symlinked or junction-mounted checkout, and a Windows
    path differing only in case, are all correctly seen as the same directory.
    It needs both to exist; when the destination does not, they cannot be the
    same thing anyway.
    """
    try:
        return left.exists() and right.exists() and left.samefile(right)
    except OSError:
        return left.resolve() == right.resolve()


def _banner(module, project, root: Path, repo):
    print("AhuraRTOS installer (offline)")
    print("  project   {}".format(root))
    if repo is not None:
        print("  kernel    {}".format(module.relative(repo, root)))
    if project.mcu:
        print("  device    {}{}".format(
            project.mcu, " ({})".format(project.core) if project.core else ""))
    print("  target    {}".format(project.target))
    print("  sources   {} / {}".format(module.relative(project.main_c, root),
                                       module.relative(project.it_c, root)))
    print()


if __name__ == "__main__":
    sys.exit(main())
