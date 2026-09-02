"""
***************************************************************************************************
 * @file        internal/stm32.py
 * @author      Nima Askari
 * @github      https://github.com/AhuraRTOS/AhuraRTOS
 * @version     1.0.0
 * @date        2026
 * @brief       STMicroelectronics STM32 - CubeMX / CubeIDE projects.
 **************************************************************************************************

A platform descriptor, not an installer. tools/internal/engine.py drives it; the bootstraps in
tools/ load both. Everything vendor-neutral - managed blocks, diffing, rollback, the download -
lives in the engine and is not repeated here.

The engine injects its own public names into this module before executing it, so `Fatal`,
`SourceFile`, `relative`, `drop_managed` and the rest are available without an import.

@copyright (c) 2026 Ahura Project Contributors
            See LICENSE in the project root for the full license text.
"""

import re
from pathlib import Path

NAME = "stm32"
TITLE = "STM32CubeMX / STM32CubeIDE"
PROG = "install_stm32_online.py"
DOC = "doc/stm32.md"
DESCRIPTION = "Install AhuraRTOS into an STM32CubeMX CMake project. Run it from the project root."


def detect(root: Path) -> bool:
    """Whether `root` looks like this kind of project."""
    return (root / "cmake" / "stm32cubemx" / "CMakeLists.txt").is_file() \
        or any(root.glob("*.ioc"))

SOC = "st/stm32"


PRUNE = {
    ".git", ".vscode", ".settings", "__pycache__",
    "build", "Build", "Debug", "Release", "cmake-build-debug", "cmake-build-release",
    "Drivers", "Middlewares", "Utilities", "AhuraRTOS",
}


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


class Project:
    """Everything the installer needs to know, all of it read from generated
    sources - never from the .ioc."""

    def __init__(self, root: Path, args):
        app_dir = getattr(args, "app_dir", None)
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

    def check(self, args):
        tick = args.tick
        """Everything that would make the build fail or the board hang, checked
        before a single byte is written."""
        # Before anything else: an integration this installer did not write cannot be
        # reconciled, only duplicated. See refuse_unmanaged().
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
        refuse_duplicate_call(
            body, "os_tick_handler", "SysTick_Handler in {}".format(relative(self.it_c, self.root)),
            "Delete that line and run this again. The block it writes back does the same job\n"
            "  and stays up to date on every later run.")

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


    def banner(self):
        """The lines main() used to print between the project path and the blank line."""
        lines = []
        if self.mcu:
            lines.append("  device    {}{}".format(
                self.mcu, " ({})".format(self.core) if self.core else ""))
        lines.append("  target    {}".format(self.target))
        lines.append("  sources   {} / {}".format(relative(self.main_c, self.root),
                                                  relative(self.it_c, self.root)))
        return lines


CMAKE_BLOCK = """\
# Everything the kernel needs from this build lives in this one block, appended after the
# CubeMX-generated part rather than woven into it: target_sources() and target_link_libraries()
# both APPEND, so a later call adds to what is already there. The generated blocks above stay
# exactly as CubeMX wrote them, and the whole integration is one region to read or remove.

# OS_CONFIG_DIR must be set BEFORE add_subdirectory: it tells the kernel library where this
# project's os_config.h lives. The kernel and the application have to compile against the same
# configuration - if only the application saw the file, their structure sizes would silently
# disagree.
# Selects the SoC package under soc/. On STM32 that package contributes no code - the
# CMSIS-Pack startup files already give the kernel the PendSV vector name and SystemCoreClock,
# and single-core parts need no core id, IPI or spinlock - so this line records the choice
# rather than changing the build. See AhuraRTOS/doc/soc.md.
set(AHURA_SOC {soc})

set(OS_CONFIG_DIR ${{CMAKE_CURRENT_SOURCE_DIR}}/{cfg})
add_subdirectory({ahura})

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
    add_subdirectory({ahura}/test)
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


def plan(project, repo_dir: Path, args, copy_tree: bool):
    tick, force = args.tick, args.force_templates
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
    templates = [(repo_dir / "template" / "os_config.h", project.inc_dir),
                 (repo_dir / "template" / "os_cb.c", project.src_dir),
                 (repo_dir / "template" / "os_main.c", project.src_dir)]

    # The SoC package's own options, if it has any. Optional in a way os_config.h is not - the
    # package defaults every one of them - so a package without the file contributes nothing here
    # rather than failing.
    soc_config = repo_dir / "soc" / SOC / "template" / "soc_config.h"
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
        ahura=relative(ahura_dest, project.root),
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


def add_arguments(parser):
    parser.add_argument("--app-dir", metavar="DIR",
                        help="which source tree to install into, on a dual-core part")
    parser.add_argument("--tick", choices=("systick", "external"), default="systick",
                        help="tick source (default: systick)")
