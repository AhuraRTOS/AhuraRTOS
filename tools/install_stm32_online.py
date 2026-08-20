#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Install AhuraRTOS into an STM32CubeMX-generated CMake project, over the network.

Run it from the root of your project - the directory holding the top-level
CMakeLists.txt and the .ioc. Nothing needs to be saved there first; piping the
script straight into Python leaves no installer file behind:

    curl -fsSL <raw-url>/tools/install_stm32_online.py | python3 -    # macOS, Linux
    irm <raw-url>/tools/install_stm32_online.py | python -            # Windows

Either way it prints the diff and asks before writing. Options go after the `-`:

    ... | python - --dry-run     # show the diff and exit, writing nothing
    ... | python - --yes         # apply without asking
    ... | python - --uninstall   # take the integration back out

A saved copy works the same way (`python install_stm32_online.py --dry-run`).

For a machine with no route out, use install_stm32_offline.py instead: same six
steps, byte-identical result, and it only ever reads a copy already on disk.

What it does, which is exactly the six steps of doc/installation.md:

    1. put the AhuraRTOS repository at AhuraRTOS/ in the project
    2. copy AhuraRTOS/kernel/template/{os_config.h,os_cb.c,os_main.c} into it
    3. append the kernel block to the top-level CMakeLists.txt
    4. route the tick: os_tick_handler() in SysTick_Handler
    5. disable a CubeMX-generated PendSV_Handler, which the port defines
    6. boot it: os_init() / os_start() in main()

Note what is NOT copied: kernel/template/soc_cb.c. That file is the SoC
half of the callback contract, and STM32 needs none of it - CMSIS-Pack startup
already gives the kernel its vector name and SystemCoreClock, and single-core
parts have no core id, IPI or spinlock. Copying it anyway would put empty weak
stubs in the build; doc/soc.md explains why that is worse than it sounds.

Rules it follows, in order of importance:

  * It never opens the .ioc. That file is the input CubeMX generates *from*;
    editing it behind CubeMX's back is how a project ends up with a
    configuration nobody can reproduce. Everything this script knows it reads
    out of the generated sources - which is also all the compiler ever sees.

  * Every C edit lands inside a CubeMX `USER CODE BEGIN/END` section, so
    regenerating the project keeps it. Two exceptions, both unavoidable: the
    top-level CMakeLists.txt, which CubeMX writes once and never rewrites (it
    says so in its own header), and a generated PendSV_Handler, which is not in
    a USER CODE section because CubeMX owns it.

  * It never silently overwrites your work. os_config.h, os_cb.c and os_main.c
    are yours once copied, so an existing one is kept, never replaced.

  * Running it twice changes nothing. Every edit sits between
    `>>> AhuraRTOS BEGIN` and `<<< AhuraRTOS END`, and each run removes those
    regions and rebuilds them at the right anchors. That is also what repairs a
    project where the calls were moved, or where CubeMX regenerated over them.

  * Nothing it disables is lost. A generated PendSV_Handler is wrapped in
    `#if 0` rather than deleted, and --uninstall puts it back verbatim.

Requires Python 3.8 or newer and nothing else - standard library only, no pip
install, no shell commands. Runs the same on Windows, macOS and Linux.
"""

import argparse
import contextlib
import re
import sys
from pathlib import Path

from install_common import (
    BEGIN_TEXT, Fatal, SourceFile, add_common_arguments, apply, drop_managed,
    finish, function_body, function_span, looks_like_ahura, managed_block,
    refuse_unmanaged, relative, repo_source, strip_comments, walk_sources,
)

SOC = "st/stm32"

PRUNE = {
    ".git", ".vscode", ".settings", "__pycache__",
    "build", "Build", "Debug", "Release", "cmake-build-debug", "cmake-build-release",
    "Drivers", "Middlewares", "Utilities", "AhuraRTOS",
}


# ---------------------------------------------------------------------------
# Managed regions
# ---------------------------------------------------------------------------


def user_code_span(text: str, tag: str):
    """Offsets of the body between USER CODE BEGIN/END <tag>, plus both indents.

    Returns (body_start, body_end, begin_indent, end_indent) or None when the
    section is absent - which happens legitimately: a project generated without
    the peripheral that owns a section simply does not have it.

    The two indents are not always the same, and the difference matters. In
    main.c the whole `while (1) {` sits *between* BEGIN WHILE and END WHILE, so
    the closing comment is one level deeper than the opening one. Code that
    belongs before the loop has to follow the opening indent.
    """
    b = re.search(r"^([ \t]*)/\*[ \t]*USER CODE BEGIN[ \t]+" + re.escape(tag) +
                  r"[ \t]*\*/[ \t]*\n", text, re.M)
    if not b:
        return None
    e = re.search(r"^([ \t]*)/\*[ \t]*USER CODE END[ \t]+" + re.escape(tag) + r"[ \t]*\*/",
                  text[b.end():], re.M)
    if not e:
        return None
    return b.end(), b.end() + e.start(), b.group(1), e.group(1)


def put_in_user_code(src: SourceFile, tag: str, payload: str, where: str = "end") -> bool:
    """Insert our block into a USER CODE section, at the start or the end.

    Existing user content in the section is copied through byte for byte,
    including its blank lines: `while (1) {` lives inside USER CODE WHILE, and
    re-spacing generated code is not this script's job.

    `where="start"` exists for exactly that section, where appending would put
    os_init() *inside* the infinite loop rather than before it.
    """
    span = user_code_span(src.text, tag)
    if span is None:
        raise Fatal("no 'USER CODE BEGIN {}' section in {}".format(tag, src.path.name))

    start, end, begin_indent, end_indent = span
    body = src.text[start:end]
    block = managed_block(payload, begin_indent if where == "start" else end_indent)

    if where == "start":
        new_body = "\n" + block + "\n" + body.lstrip("\n")
    elif body.strip():
        new_body = body.rstrip("\n") + "\n\n" + block + "\n"
    else:
        new_body = "\n" + block + "\n"

    if new_body == body:
        return False
    src.text = src.text[:start] + new_body + src.text[end:]
    return True


PENDSV_NOTE = """\
/* CubeMX generated a PendSV_Handler here, and the kernel's port defines
   PendSV_Handler too - two definitions of one symbol is a link error.
   Disabled rather than deleted, so nothing of yours is lost: --uninstall
   restores it exactly as it was.

   This is the one edit outside a USER CODE section, because CubeMX owns this
   function and gives no section inside it. Regenerating from the .ioc brings
   the stub back; re-run this installer afterwards and it is disabled again.
   To stop it being generated at all: Pinout & Configuration -> System Core ->
   NVIC -> Code generation -> uncheck 'Generate IRQ handler' for 'Pendable
   request for system service'. */"""


def disable_pendsv(src: SourceFile) -> bool:
    """Wrap a generated PendSV_Handler in #if 0, keeping it recoverable.

    #if 0 rather than /* */ because the function contains USER CODE comments,
    and C has no nested block comments. The original text is copied in verbatim
    - not through managed_block(), whose per-line rstrip would quietly drop
    trailing whitespace and break the byte-exact --uninstall.
    """
    span = function_span(src.text, "PendSV_Handler")
    if span is None:
        return False

    start, end = span
    original = src.text[start:end]
    if not original.endswith("\n"):
        original += "\n"

    block = ("/* " + BEGIN_TEXT + " */\n"
             + PENDSV_NOTE + "\n"
             "#if 0\n" + original + "#endif\n"
             "/* " + END_TEXT + " */\n")

    src.text = src.text[:start] + block + src.text[end:]
    return True


# ---------------------------------------------------------------------------
# Reading the generated project
# ---------------------------------------------------------------------------


class Project:
    """Everything the installer needs to know, all of it read from generated
    sources - never from the .ioc."""

    def __init__(self, root: Path, app_dir: str = None):
        self.root = root

        top = root / "CMakeLists.txt"
        if not top.is_file():
            raise Fatal(
                "no CMakeLists.txt here.\n"
                "  Run this from the root of your project - the directory holding\n"
                "  CMakeLists.txt and the .ioc. If your project has a .cproject but no\n"
                "  CMakeLists.txt it was generated for STM32CubeIDE; regenerate it with\n"
                "  Project Manager -> Toolchain/IDE -> CMake, or install by hand from\n"
                "  doc/installation.md.")
        self.cmake = SourceFile(top)

        sources = list(walk_sources(root, PRUNE))
        if app_dir:
            wanted = (root / app_dir).resolve()
            sources = [p for p in sources
                       if wanted == p.parent.resolve() or wanted in p.resolve().parents]

        self.main_c = self._pick(
            [p for p in sources if p.name == "main.c"
             and re.search(r"\bint\s+main\s*\(", p.read_text(errors="replace"))],
            "main.c")

        self.it_c = self._pick(
            [p for p in sources if p.suffix == ".c"
             and "void SysTick_Handler" in p.read_text(errors="replace")],
            "the interrupt file defining SysTick_Handler")

        self.src_dir = self.main_c.parent
        main_h = next((p for p in sources if p.name == "main.h"), None)
        self.inc_dir = main_h.parent if main_h else self.src_dir

        self.target = self._cmake_target()
        self.mcu, self.core = self._identity()

    def _pick(self, candidates, what: str) -> Path:
        if not candidates:
            raise Fatal("could not find {} under {}".format(what, self.root))
        if len(candidates) > 1:
            listing = "\n".join("    " + relative(p, self.root) for p in sorted(candidates))
            raise Fatal(
                "found more than one {}:\n{}\n"
                "  This is normal on a dual-core part (H7 dual, WB, WL, MP1): each core\n"
                "  gets its own tree and the kernel is installed into one of them.\n"
                "  Re-run with --app-dir pointing at the core you want, e.g.\n"
                "      --app-dir CM7".format(what, listing))
        return candidates[0]

    def _cmake_target(self) -> str:
        """The executable to link against, as a CMake expression.

        CubeMX writes `set(CMAKE_PROJECT_NAME <name>)` then
        `add_executable(${CMAKE_PROJECT_NAME})`, so the variable is the stable
        thing to use - it keeps working if the project is renamed.
        """
        text = self.cmake.text
        if re.search(r"add_executable\s*\(\s*\$\{CMAKE_PROJECT_NAME\}", text):
            return "${CMAKE_PROJECT_NAME}"
        m = re.search(r"add_executable\s*\(\s*([A-Za-z0-9_.+-]+)", text)
        if m:
            return m.group(1)
        raise Fatal("no add_executable() in CMakeLists.txt - is this the project root?")

    def _identity(self):
        """MCU and core, for the summary only. Both come from generated CMake;
        neither is required, so a project laid out differently still installs."""
        mcu = core = None
        generated = self.root / "cmake" / "stm32cubemx" / "CMakeLists.txt"
        if generated.is_file():
            m = re.search(r"\b(STM32[A-Z0-9]+xx)\b", generated.read_text(errors="replace"))
            mcu = m.group(1) if m else None
        cmake_dir = self.root / "cmake"
        if cmake_dir.is_dir():
            for toolchain in sorted(cmake_dir.glob("*.cmake")):
                m = re.search(r"-mcpu=(cortex-m[0-9a-z.+]*)",
                              toolchain.read_text(errors="replace"))
                if m:
                    core = m.group(1)
                    break
        return mcu, core

    def check(self, tick: str):
        """Everything that would make the build fail or the board hang, checked
        before a single byte is written."""
        # Before anything else: an integration this installer did not write cannot be
        # reconciled, only duplicated. See install_common.refuse_unmanaged().
        refuse_unmanaged(self.cmake, self.root)

        it_text = self.it_c.read_text(errors="replace")

        # Two RTOSes on one PendSV cannot both win.
        if re.search(r"\b(osKernelInitialize|MX_FREERTOS_Init|xPortStartScheduler)\b",
                     strip_comments(it_text)) \
                or (self.root / "Middlewares" / "Third_Party" / "FreeRTOS").is_dir():
            raise Fatal(
                "this project already has FreeRTOS/CMSIS-RTOS in it.\n"
                "  Both kernels claim PendSV and the tick, so they cannot coexist.\n"
                "  Turn FreeRTOS off in CubeMX (Middleware -> FREERTOS -> Interface:\n"
                "  Disabled), regenerate, then run this script again.")

        if tick == "external":
            return  # the user drives os_tick_handler() from their own timer

        # The tick. When the HAL timebase is still SysTick, CubeMX emits
        # HAL_IncTick() into the handler - two time bases on one interrupt drift
        # against each other, so this is a stop rather than something to patch.
        body = function_body(it_text, "SysTick_Handler")
        if body is None:
            raise Fatal("no SysTick_Handler in {} - cannot route the tick"
                        .format(relative(self.it_c, self.root)))
        if re.search(r"\bHAL_IncTick\s*\(", strip_comments(body)):
            raise Fatal(
                "SysTick still drives the HAL timebase (HAL_IncTick() is in\n"
                "  SysTick_Handler), so the kernel cannot have it. Move the HAL onto a\n"
                "  spare timer in CubeMX:\n"
                "      Pinout & Configuration -> System Core -> SYS\n"
                "      -> Timebase Source -> TIM6 (or any timer you are not using)\n"
                "  Regenerate, then run this script again.\n"
                "  Driving the kernel from your own timer instead? Re-run with\n"
                "  --tick external and see doc/installation.md step 4.")


# ---------------------------------------------------------------------------
# The plan
# ---------------------------------------------------------------------------

CMAKE_BLOCK = """\
# Everything the kernel needs from this build lives in this one block, appended after the
# CubeMX-generated part rather than woven into it: target_sources() and target_link_libraries()
# both APPEND, so a later call adds to what is already there. The generated blocks above stay
# exactly as CubeMX wrote them, and the whole integration is one region to read or remove.

# OS_CONFIG_DIR must be set BEFORE add_subdirectory: it tells the kernel library where this
# project's os_config.h lives. The kernel and the application have to compile against the same
# configuration - if only the application saw the file, their structure sizes would silently
# disagree.
# Selects the SoC package under kernel/soc/. On STM32 that package contributes no code - the
# CMSIS-Pack startup files already give the kernel the PendSV vector name and SystemCoreClock,
# and single-core parts need no core id, IPI or spinlock - so this line records the choice
# rather than changing the build. See AhuraRTOS/doc/soc.md.
set(AHURA_SOC {soc})

set(OS_CONFIG_DIR ${{CMAKE_CURRENT_SOURCE_DIR}}/{cfg})
add_subdirectory({kernel})

# A header is not normally a configure-time dependency, so CMake would not notice a changed
# os_config.h until the next manual re-run. Listing it here makes editing it re-run CMake.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${{OS_CONFIG_DIR}}/os_config.h)

# Self-test suite, driven by os_config.h so there is only one switch to flip. With
# OS_CONFIG_TEST_ENABLE at 1, os_init() runs os_test() instead of os_main(), and nothing in the
# kernel defines that function - so forgetting the library is a link error rather than a test
# build that silently tests nothing.
file(READ ${{OS_CONFIG_DIR}}/os_config.h _os_config_contents)
if(_os_config_contents MATCHES "#define[ \\t]+OS_CONFIG_TEST_ENABLE[ \\t]+1")
    message(STATUS "Ahura self-test suite: ENABLED (os_main() is not run in this build)")
    add_subdirectory({kernel}/test)
    set(AHURA_TEST_LIB os_test)
else()
    set(AHURA_TEST_LIB "")
endif()
unset(_os_config_contents)

# Application-owned kernel files. These belong to the APPLICATION build, never to the kernel
# library: os_cb.c holds the platform callbacks, os_main.c the default task's body.
target_sources({target} PRIVATE
    {src}/os_cb.c
    {src}/os_main.c
)

target_link_libraries({target}
    ahura_kernel
    ${{AHURA_TEST_LIB}}   # empty unless OS_CONFIG_TEST_ENABLE is 1, see above
)"""

TICK_PAYLOAD = """\
/* The kernel's tick, and the only thing the application owes it besides PendSV. HAL_IncTick()
   is deliberately NOT called here: the HAL runs off its own timer (CubeMX -> SYS -> Timebase
   Source), so the two time bases never share this interrupt. */
os_tick_handler();"""

BOOT_PAYLOAD = """\
/* Both calls come after the clock tree and the peripherals are configured: os_init() programs
   the tick from the live SystemCoreClock, so a still-default clock would give the wrong tick
   rate. os_init() creates the idle task, the kernel service tasks and the default application
   task running os_main(); os_start() then switches to task context and never returns - the
   while(1) below is never reached. */
os_init();
os_start();"""


def verify_placement(it: SourceFile, mc: SourceFile, tick: str):
    """Prove the calls ended up where they have to be, before anything is written.

    Cheap insurance against a project laid out in some way not seen before: a
    USER CODE section that turns up somewhere unexpected would otherwise put
    os_init() outside main() and produce a board that does nothing.
    """
    if tick == "systick":
        body = function_body(it.text, "SysTick_Handler")
        if body is None or "os_tick_handler" not in strip_comments(body):
            raise Fatal("os_tick_handler() did not land inside SysTick_Handler in {} - "
                        "refusing to write a build that would never tick"
                        .format(it.path.name))

    body = function_body(mc.text, "main")
    if body is None:
        raise Fatal("could not find main() in {}".format(mc.path.name))
    code = strip_comments(body)

    at_init = code.find("os_init()")
    at_start = code.find("os_start()")
    if at_init < 0 or at_start < 0:
        raise Fatal("os_init()/os_start() did not land inside main() in {}"
                    .format(mc.path.name))

    at_loop = re.search(r"\bwhile[ \t]*\([ \t]*1[ \t]*\)", code)
    if at_loop and at_init > at_loop.start():
        raise Fatal("os_init() landed inside the infinite loop in {} - "
                    "it has to run before it".format(mc.path.name))
    if at_start < at_init:
        raise Fatal("os_start() landed before os_init() in {}".format(mc.path.name))


def plan(project: Project, repo_dir: Path, tick: str, force: bool, copy_tree: bool):
    """Return (file edits, copy actions, notes) without touching the disk."""
    edits, copies, notes = [], [], []

    ahura_dest = project.root / "AhuraRTOS"
    if copy_tree:
        copies.append(("repo", repo_dir, ahura_dest))
    else:
        notes.append("AhuraRTOS/ is already in the project - left as it is "
                     "(--update fetches the current version)")

    # The three files become yours the moment they exist. Each is checked for
    # individually, so a project missing just one gets just that one.
    templates = [(repo_dir / "kernel" / "template" / "os_config.h", project.inc_dir),
                 (repo_dir / "kernel" / "template" / "os_cb.c", project.src_dir),
                 (repo_dir / "kernel" / "template" / "os_main.c", project.src_dir)]

    # The SoC package's own options, if it has any. Optional in a way os_config.h is not - the
    # package defaults every one of them - so a package without the file contributes nothing here
    # rather than failing.
    soc_config = repo_dir / "kernel" / "soc" / SOC / "template" / "soc_config.h"
    if soc_config.is_file():
        templates.append((soc_config, project.inc_dir))

    for source, dest_dir in templates:
        dest = dest_dir / source.name
        if dest.is_file() and not force:
            notes.append("{} is already there - kept as it is "
                         "(--force-templates replaces it)".format(relative(dest, project.root)))
        else:
            copies.append(("template", source, dest))

    body = CMAKE_BLOCK.format(
        soc=SOC,
        cfg=relative(project.inc_dir, project.root),
        src=relative(project.src_dir, project.root),
        kernel=relative(ahura_dest / "kernel", project.root),
        target=project.target,
    )
    drop_managed(project.cmake)
    project.cmake.text = project.cmake.text.rstrip("\n") + "\n\n" + managed_block(
        body, "", comment="cmake")
    if project.cmake.changed:
        edits.append(project.cmake)

    # Both C files are reconciled, not patched: every managed region comes out
    # first and is rebuilt at the right anchor. That is what makes a second run
    # a no-op, and what repairs a file where the calls were moved or where
    # CubeMX regenerated over the top.
    it = SourceFile(project.it_c)
    drop_managed(it)
    put_in_user_code(it, "Includes", '#include "ahura.h"')
    if tick == "systick":
        put_in_user_code(it, "SysTick_IRQn 0", TICK_PAYLOAD)
    else:
        notes.append("--tick external: wire os_tick_handler() to your own timer yourself, "
                     "and set OS_CONFIG_TICK_SOURCE_EXTERNAL in os_config.h")
    if disable_pendsv(it):
        notes.append("{} has a CubeMX-generated PendSV_Handler - it will be wrapped in "
                     "#if 0 (the kernel's port defines that symbol)"
                     .format(relative(project.it_c, project.root)))

    mc = SourceFile(project.main_c)
    drop_managed(mc)
    put_in_user_code(mc, "Includes", '#include "ahura.h"')
    put_in_user_code(mc, "WHILE", BOOT_PAYLOAD, where="start")

    verify_placement(it, mc, tick)

    for src in (it, mc):
        if src.changed:
            edits.append(src)

    return edits, copies, notes


def plan_uninstall(project: Project):
    edits = []
    for path in (project.root / "CMakeLists.txt", project.it_c, project.main_c):
        src = SourceFile(path)
        if drop_managed(src) and src.changed:
            edits.append(src)
    return edits


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="install_stm32_online.py",
        description="Install AhuraRTOS into an STM32CubeMX-generated CMake project. "
                    "Run it from the project root. It never touches the .ioc.")
    add_common_arguments(parser)
    parser.add_argument("--app-dir", metavar="DIR",
                        help="which source tree to install into, on a dual-core part")
    parser.add_argument("--ref", default="main", metavar="REF",
                        help="branch or tag to download (default: main)")
    parser.add_argument("--tick", choices=("systick", "external"), default="systick",
                        help="tick source (default: systick)")
    args = parser.parse_args(argv)

    root = Path(args.project).expanduser().resolve()

    try:
        project = Project(root, args.app_dir)

        print("AhuraRTOS installer")
        print("  project   {}".format(root))
        if project.mcu:
            print("  device    {}{}".format(
                project.mcu, " ({})".format(project.core) if project.core else ""))
        print("  target    {}".format(project.target))
        print("  sources   {} / {}".format(relative(project.main_c, root),
                                           relative(project.it_c, root)))
        print()

        if args.uninstall:
            edits, notes = plan_uninstall(project), []
            if (root / "AhuraRTOS").is_dir() and looks_like_ahura(root / "AhuraRTOS"):
                notes.append("AhuraRTOS/ will be deleted")
            notes.append("your os_config.h, os_cb.c and os_main.c are left alone - "
                         "they are your files now")
            if not edits and not notes:
                print("Nothing installed here.")
                return 0
            return finish(args, project, root, edits, [], notes)

        project.check(args.tick)

        # A project that already carries AhuraRTOS/ has everything needed to
        # finish - kernel and templates both - so a re-run neither downloads nor
        # replaces it. That is what makes running this twice free, and what
        # keeps a second run from resetting a tree you pinned deliberately.
        # --update is the way to ask for the current version.
        installed = looks_like_ahura(root / "AhuraRTOS")
        source = contextlib.nullcontext(root / "AhuraRTOS") \
            if installed and not args.update and not args.source \
            else repo_source(args.source, args.ref)

        with source as repo_dir:
            edits, copies, notes = plan(project, repo_dir, args.tick,
                                        args.force_templates,
                                        copy_tree=not installed or args.update or bool(args.source))
            if not edits and not copies:
                print("Already installed, and everything is where it should be.")
                return 0
            return finish(args, project, root, edits, copies, notes)

    except Fatal as exc:
        print("\nerror: {}".format(exc), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ncancelled", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
