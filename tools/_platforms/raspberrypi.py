"""
***************************************************************************************************
 * @file        _platforms/raspberrypi.py
 * @author      Nima Askari
 * @github      https://github.com/AhuraRTOS/AhuraRTOS
 * @version     1.0.0
 * @date        2026
 * @brief       Raspberry Pi Pico SDK - RP2040, RP2350, RP2354, Arm and RISC-V.
 **************************************************************************************************

A platform descriptor, not an installer. tools/_ahura_install.py drives it; the bootstraps in
tools/ load both. Everything vendor-neutral - managed blocks, diffing, rollback, the download -
lives in the engine and is not repeated here.

The engine injects its own public names into this module before executing it, so `Fatal`,
`SourceFile`, `relative`, `drop_managed` and the rest are available without an import.

@copyright (c) 2026 Ahura Project Contributors
            See LICENSE in the project root for the full license text.
"""

import re
from pathlib import Path

NAME = "raspberrypi"
TITLE = "Raspberry Pi Pico SDK"
PROG = "install_rpi_online.py"
DOC = "doc/raspberry-pi.md"
DESCRIPTION = "Install AhuraRTOS into a Raspberry Pi Pico SDK project (RP2040, RP2350, RP2354). Run it from the project root."


def detect(root: Path) -> bool:
    """Whether `root` looks like this kind of project."""
    top = root / "CMakeLists.txt"
    return top.is_file() and "pico_sdk_init" in top.read_text(errors="replace")

PRUNE = {
    ".git", ".vscode", ".settings", "__pycache__",
    "build", "Build", "Debug", "Release", "cmake-build-debug", "cmake-build-release",
    "AhuraRTOS", "pico-sdk", "lib",
}


SOC_RP2040 = "raspberrypi/rp2040"


SOC_RP235X = "raspberrypi/rp235x_arm"


SOC_RP235X_RISCV = "raspberrypi/rp235x_riscv"


SOC_NOTE_ARM = """# The SoC package for this chip. This is what tells the kernel that the SDK's vector table calls
# entry 14 isr_pendsv rather than PendSV_Handler - without it the build links cleanly and then
# traps at os_start(). It also supplies isr_systick, SystemCoreClock, the core id, the inter-core
# doorbell and the SIO hardware spinlocks. See AhuraRTOS/doc/soc.md."""


SOC_NOTE_RISCV = """# The SoC package for this chip's Hazard3 RISC-V cores. There is no PendSV on RISC-V: the vector
# the kernel owns is the machine software interrupt (trap cause 3), which the SDK's crt0_riscv.S
# names isr_riscv_machine_soft_irq and declares weak so exactly this can replace it. The package
# also supplies the tick off SIO_MTIMECMP, the CPU clock, the core id, the SIO_RISCV_SOFTIRQ
# register that serves as both yield and inter-core interrupt, and the hardware spinlocks. It also
# places the context-switch handler in RAM, which on this chip is a link-time requirement rather
# than a preference. See AhuraRTOS/doc/soc.md."""


SOC_RULES = (
    ("rp2350",  SOC_RP235X),
    ("rp2354",  SOC_RP235X),
    ("pico2",   SOC_RP235X),
    ("pico_2",  SOC_RP235X),
    ("rp2040",  SOC_RP2040),
    ("pico",    SOC_RP2040),
)


def main_span(text: str):
    """(start, end) of `int main(...) { ... }`, or None.

    Not function_span: that one requires a literal `(void)`, and
    the SDK's own project template writes `int main()`. Both spellings, and an
    argc/argv main, have to match. Brace-matched over comment-stripped text so a
    brace in a comment or a string cannot end the function early.
    """
    code = strip_comments(text)
    m = re.search(r"^[ \t]*(?:int|void)[ \t]+main[ \t]*\([^)]*\)[ \t]*\n?[ \t]*\{", code, re.M)
    if not m:
        return None

    depth, i = 1, m.end()
    while i < len(code) and depth:
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
        i += 1
    return (m.end(), i - 1) if not depth else None


class Project:
    """Everything the installer needs, all of it read from the project's own
    sources."""

    def __init__(self, root: Path, args):
        self.root = root

        top = root / "CMakeLists.txt"
        if not top.is_file():
            raise Fatal(
                "no CMakeLists.txt here.\n"
                "  Run this from the root of your Pico project - the directory holding\n"
                "  CMakeLists.txt and pico_sdk_import.cmake.")
        self.cmake = SourceFile(top)

        if "pico_sdk_init" not in self.cmake.text:
            raise Fatal(
                "this CMakeLists.txt does not call pico_sdk_init(), so it is not a\n"
                "  Pico SDK project. Installing into an STM32CubeMX project? Use\n"
                "  install_stm32_online.py instead.")

        sources = list(walk_sources(root, PRUNE))
        self.main_c = self._pick(
            [p for p in sources if p.suffix == ".c"
             and main_span(p.read_text(errors="replace")) is not None],
            "a .c file defining main()")

        self.src_dir = self.main_c.parent
        self.inc_dir = self.src_dir
        self.target = self._cmake_target()
        self.board = self._board()
        self.platform = self._cmake_string("PICO_PLATFORM")
        self.soc = getattr(args, "soc", None) or self._detect_soc()

    def _pick(self, candidates, what: str) -> Path:
        if not candidates:
            raise Fatal("could not find {} under {}".format(what, self.root))
        if len(candidates) > 1:
            listing = "\n".join("    " + relative(p, self.root) for p in sorted(candidates))
            raise Fatal(
                "found more than one {}:\n{}\n"
                "  The kernel is installed into one of them. Move the others aside, or\n"
                "  install by hand from doc/installation.md.".format(what, listing))
        return candidates[0]

    def _cmake_target(self) -> str:
        """The executable to link against.

        A Pico project names it literally in add_executable(), unlike CubeMX's
        ${CMAKE_PROJECT_NAME} indirection. Where several exist, the one whose
        sources include the main.c found above is the right answer.
        """
        text = strip_comments(self.cmake.text)
        targets = re.findall(r"add_executable\s*\(\s*([A-Za-z0-9_.${}+-]+)", text)
        if not targets:
            raise Fatal("no add_executable() in CMakeLists.txt - is this the project root?")
        if len(targets) == 1:
            return targets[0]

        wanted = self.main_c.name
        for name in targets:
            m = re.search(r"add_executable\s*\(\s*" + re.escape(name) + r"\b([^)]*)\)", text, re.S)
            if m and wanted in m.group(1):
                return name
        raise Fatal(
            "several add_executable() targets and none of them names {}:\n    {}\n"
            "  Cannot tell which one the kernel belongs to."
            .format(wanted, ", ".join(targets)))

    def _cmake_string(self, name: str):
        """The value of a set(<name> <value>) in the top-level CMakeLists, or None."""
        m = re.search(r"set\s*\(\s*" + re.escape(name) + r"\s+([A-Za-z0-9_.-]+)",
                      strip_comments(self.cmake.text))
        return m.group(1) if m else None

    def _board(self):
        """PICO_BOARD, for the summary and for choosing the SoC package. Absent is fine."""
        return self._cmake_string("PICO_BOARD")

    def _is_riscv(self) -> bool:
        """Whether this project targets the RP2350's Hazard3 RISC-V cores rather than its M33s.

        Two places say so, and the second is the one that usually does. A project may set
        PICO_PLATFORM=rp2350-riscv itself; far more often it does not, because the VS Code extension
        writes only a toolchain name and lets pico-vscode.cmake derive the platform from it:

            if(PICO_TOOLCHAIN_PATH MATCHES "RISCV")
                set(PICO_PLATFORM rp2350-riscv CACHE STRING "Pico Platform")

        Reading the toolchain name here the same way is what stops a perfectly unambiguous project
        being told the installer cannot tell which chip it targets.
        """
        for value in (self.platform, self._cmake_string("toolchainVersion")):
            if value and ("riscv" in value.lower()):
                return True

        return False

    def _detect_soc(self) -> str:
        """Which SoC package this project needs, from PICO_PLATFORM then PICO_BOARD.

        The RP2040 and the RP235x are separate packages because they are separate silicon
        generations, so picking the wrong one is not a near miss - it would arm the wrong
        inter-core interrupt and build against the wrong core. Guessing is therefore not on:
        an unrecognised board is an error naming both choices.
        """
        riscv = self._is_riscv()

        for source in (self.platform, self.board):
            if not source:
                continue
            lowered = source.lower()
            for token, soc in SOC_RULES:
                if token in lowered:
                    if not riscv:
                        return soc

                    # Same chip, other core. The RP235x packages are siblings and the toolchain has
                    # already decided between them.
                    if soc == SOC_RP235X:
                        return SOC_RP235X_RISCV

                    raise Fatal(
                        "this project builds for RISC-V, but its board names an RP2040." + "\n" +
                        "  The RP2040 is Cortex-M0+ only and has no RISC-V cores, so there is no" + "\n" +
                        "  package for that combination. Check PICO_BOARD and the toolchain.")

        raise Fatal(
            "cannot tell which chip this project targets.\n"
            "  PICO_PLATFORM={}, PICO_BOARD={}\n"
            "  Pass the package explicitly:\n"
            "      --soc {}           (RP2040)\n"
            "      --soc {}      (RP2350, RP2354 - Arm cores)\n"
            "      --soc {}    (RP2350, RP2354 - RISC-V cores)"
            .format(self.platform or "<unset>", self.board or "<unset>",
                    SOC_RP2040, SOC_RP235X, SOC_RP235X_RISCV))

    def check(self, args):
        """Anything that would make the build fail, checked before writing."""
        # Before anything else: an integration this installer did not write cannot be
        # reconciled, only duplicated. See refuse_unmanaged().
        refuse_unmanaged(self.cmake, self.root)

        text = strip_comments(self.cmake.text)
        if re.search(r"\bFREERTOS|\bfreertos\b", text) or (self.root / "FreeRTOS-Kernel").is_dir():
            raise Fatal(
                "this project already has FreeRTOS in it.\n"
                "  Both kernels claim PendSV and the tick, so they cannot coexist.\n"
                "  Remove the FreeRTOS import from CMakeLists.txt and re-run.")

        if main_span(self.main_c.read_text(errors="replace")) is None:
            raise Fatal("could not brace-match main() in {}"
                        .format(relative(self.main_c, self.root)))


    def banner(self):
        """The lines main() used to print between the project path and the blank line."""
        lines = []
        if self.board:
            lines.append("  board     {}".format(self.board))
        lines.append("  soc       {}".format(self.soc))
        lines.append("  target    {}".format(self.target))
        lines.append("  sources   {}".format(relative(self.main_c, self.root)))
        return lines


CMAKE_BLOCK = """\
# Everything the kernel needs from this build lives in this one block, appended after the
# generated part rather than woven into it: target_sources() and target_link_libraries() both
# APPEND, so a later call adds to what is already there. It also has to come after
# pico_sdk_init(), which it does by sitting at the end of the file.

{soc_note}
set(AHURA_SOC {soc})

# OS_CONFIG_DIR must be set BEFORE add_subdirectory: it tells the kernel library where this
# project's os_config.h lives. The kernel and the application have to compile against the same
# configuration - if only the application saw the file, their structure sizes would silently
# disagree.
set(OS_CONFIG_DIR {cfg})
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
# library: os_cb.c holds the application's callbacks, os_main.c the default task's body. The SoC
# half of the callback contract is NOT here - the {soc} package above provides it.
target_sources({target} PRIVATE
    {src}os_cb.c
    {src}os_main.c
)

target_link_libraries({target}
    ahura_kernel
    ${{AHURA_TEST_LIB}}   # empty unless OS_CONFIG_TEST_ENABLE is 1, see above
)"""


INCLUDE_PAYLOAD = """\
/* The kernel's one public header, and the only one an application needs. The SoC package has
   no public header at all: every entry point it has is a callback the kernel invokes itself,
   secondary-core start-up included. */
#include "ahura.h" """.rstrip()


BOOT_PAYLOAD = """\
/* Nothing SoC-specific to call here: the SoC package hooks os_init() itself, through
   os_arch_soc_init_cb(), so the CPU clock, the hardware spinlock and this core's inter-core
   interrupt are all ready before the tick is programmed. os_init() then creates the idle task,
   the kernel service tasks and the default application task running os_main(); os_start()
   switches to task context and never returns, so anything below here is unreachable. */
os_init();
os_start();"""


def insert_include(src: SourceFile) -> bool:
    """Put the include after the last existing one, or at the top of the file."""
    last = None
    for m in re.finditer(r"^[ \t]*#[ \t]*include[^\n]*\n", src.text, re.M):
        last = m
    at = last.end() if last else 0
    src.text = src.text[:at] + "\n" + managed_block(INCLUDE_PAYLOAD, "") + src.text[at:]
    return True


def insert_boot(src: SourceFile) -> bool:
    """Put the boot calls in main(), ahead of its first loop.

    A Pico main() is hand-written, so there is no marker to aim at. The first
    `while`/`for` inside the body is the reliable landmark: the SDK's own
    template ends in one, and anything the application set up - stdio, GPIO,
    clocks - is above it, which is exactly where the kernel wants to start.
    With no loop at all the calls go at the end of the body, which is the same
    position by a different route.
    """
    span = main_span(src.text)
    if span is None:
        raise Fatal("could not find main() in {}".format(src.path.name))
    body_start, body_end = span

    code = strip_comments(src.text)
    loop = re.search(r"^[ \t]*(?:while|for)[ \t]*\(", code[body_start:body_end], re.M)
    at = body_start + loop.start() if loop else body_end

    # Match the indentation of whatever follows, so the block sits with the code.
    line_start = src.text.rfind("\n", 0, at) + 1
    indent = re.match(r"[ \t]*", src.text[line_start:]).group(0) or "    "

    src.text = (src.text[:line_start]
                + managed_block(BOOT_PAYLOAD, indent) + "\n"
                + src.text[line_start:])
    return True


def verify_placement(mc: SourceFile):
    """Prove the calls ended up inside main(), and in order, before writing."""
    span = main_span(mc.text)
    if span is None:
        raise Fatal("could not find main() in {} after editing".format(mc.path.name))

    body = strip_comments(mc.text)[span[0]:span[1]]
    positions = {name: body.find(name + "()")
                 for name in ("os_init", "os_start")}
    missing = [n for n, at in positions.items() if at < 0]
    if missing:
        raise Fatal("{}() did not land inside main() in {} - refusing to write a build "
                    "that would never start".format(", ".join(missing), mc.path.name))

    if not positions["os_init"] < positions["os_start"]:
        raise Fatal("the boot calls landed out of order in {} - os_init() must run before "
                    "os_start()".format(mc.path.name))

    loop = re.search(r"^[ \t]*(?:while|for)[ \t]*\(", body, re.M)
    if loop and positions["os_init"] > loop.start():
        raise Fatal("os_init() landed inside the loop in {} - it has to run before it"
                    .format(mc.path.name))


def plan(project, repo_dir: Path, args, copy_tree: bool):
    force = args.force_templates
    """Return (file edits, copy actions, notes) without touching the disk."""
    edits, copies, notes = [], [], []

    ahura_dest = project.root / "AhuraRTOS"
    if copy_tree:
        copies.append(("repo", repo_dir, ahura_dest))
    else:
        notes.append("AhuraRTOS/ is already in the project - left as it is "
                     "(--update fetches the current version)")

    # The three files become yours the moment they exist, so an existing one is
    # kept. soc_cb.c is deliberately not among them: the SoC package is it.
    templates = [(repo_dir / "template" / "os_config.h", project.inc_dir),
                 (repo_dir / "template" / "os_cb.c", project.src_dir),
                 (repo_dir / "template" / "os_main.c", project.src_dir)]

    # The SoC package's own options, if it has any. Optional in a way os_config.h is not - the
    # package defaults every one of them - so a package without the file contributes nothing here
    # rather than failing.
    soc_config = repo_dir / "soc" / project.soc / "template" / "soc_config.h"
    if soc_config.is_file():
        templates.append((soc_config, project.inc_dir))

    for source, dest_dir in templates:
        dest = dest_dir / source.name
        if dest.is_file() and not force:
            notes.append("{} is already there - kept as it is "
                         "(--force-templates replaces it)".format(relative(dest, project.root)))
        else:
            copies.append(("template", source, dest))

    # relative() answers "." for a directory that IS the project root, which is the normal
    # layout for a Pico project - main.c beside CMakeLists.txt. Emitting that verbatim would give
    # "${CMAKE_CURRENT_SOURCE_DIR}/." and "./os_cb.c": both work, both look like a bug.
    cfg = relative(project.inc_dir, project.root)
    src = relative(project.src_dir, project.root)

    body = CMAKE_BLOCK.format(
        soc=project.soc,
        soc_note=SOC_NOTE_RISCV if project.soc == SOC_RP235X_RISCV else SOC_NOTE_ARM,
        cfg="${CMAKE_CURRENT_SOURCE_DIR}" + ("" if cfg == "." else "/" + cfg),
        src="" if src == "." else src + "/",
        ahura=relative(ahura_dest, project.root),
        target=project.target,
    )
    drop_managed(project.cmake)
    project.cmake.text = project.cmake.text.rstrip("\n") + "\n\n" + managed_block(
        body, "", comment="cmake")
    if project.cmake.changed:
        edits.append(project.cmake)

    # Reconciled, not patched: every managed region comes out first and is
    # rebuilt at the right anchor, which is what makes a second run a no-op.
    mc = SourceFile(project.main_c)
    drop_managed(mc)
    insert_include(mc)
    insert_boot(mc)
    verify_placement(mc)
    if mc.changed:
        edits.append(mc)

    if project.soc == SOC_RP235X_RISCV:
        notes.append("the tick and the context-switch vector need no edits here - the {} package "
                     "drives the tick off SIO_MTIMECMP and the kernel replaces the SDK's weak "
                     "isr_riscv_machine_soft_irq".format(project.soc))
    else:
        notes.append("the tick and the PendSV vector need no edits here - the {} package "
                     "supplies isr_systick and names isr_pendsv for the kernel".format(project.soc))

    return edits, copies, notes


def plan_uninstall(project: Project):
    edits = []
    for path in (project.root / "CMakeLists.txt", project.main_c):
        src = SourceFile(path)
        if drop_managed(src) and src.changed:
            edits.append(src)
    return edits


def add_arguments(parser):
    parser.add_argument("--soc", metavar="PKG",
                        help="SoC package to install (default: chosen from PICO_PLATFORM, "
                             "the toolchain name and PICO_BOARD); raspberrypi/rp2040, "
                             "raspberrypi/rp235x_arm or raspberrypi/rp235x_riscv")
