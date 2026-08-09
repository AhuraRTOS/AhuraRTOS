#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Install AhuraRTOS into an STM32CubeMX-generated CMake project.

Run it from the root of your project - the directory holding the top-level
CMakeLists.txt and the .ioc:

    python install.py               # show the diff, then ask before writing
    python install.py --yes         # apply without asking
    python install.py --dry-run     # show the diff and exit, writing nothing
    python install.py --uninstall   # take the integration back out

What it does, which is exactly the six steps of doc/installation.md:

    1. put kernel/ at AhuraRTOS/kernel/ in the project
    2. copy kernel/template/{os_config.h,os_cb.c,os_main.c} into the project
    3. append the kernel block to the top-level CMakeLists.txt
    4. route the tick: os_tick_handler() in SysTick_Handler
    5. check nothing else defines PendSV_Handler
    6. boot it: os_init() / os_start() in main()

Three rules it follows, in order of importance:

  * It never opens the .ioc. The .ioc is the input CubeMX generates *from*;
    editing it behind CubeMX's back is how a project ends up with a
    configuration nobody can reproduce. Everything this script knows, it reads
    out of the generated sources - which is also the only thing the compiler
    ever sees.

  * Every C edit lands inside a CubeMX `USER CODE BEGIN/END` section, so
    regenerating the project keeps it. The one file edited outside such a
    section is the top-level CMakeLists.txt, which CubeMX writes once and
    never rewrites (it says so in its own header comment).

  * It never silently overwrites your work. os_config.h, os_cb.c and os_main.c
    are yours once copied, so an existing one is kept, not replaced. Every
    other edit sits between `>>> AhuraRTOS BEGIN` / `<<< AhuraRTOS END`
    markers, so re-running replaces that region and nothing else, and
    --uninstall can find it again.

Requires Python 3.8 or newer and nothing else - standard library only, no pip
install, no shell commands. Runs the same on Windows, macOS and Linux.
"""

import argparse
import contextlib
import difflib
import io
import os
import re
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

REPO = "AhuraRTOS/AhuraRTOS"
TARBALL = "https://codeload.github.com/{repo}/tar.gz/{ref}"

BEGIN_TEXT = ">>> AhuraRTOS BEGIN - managed block, changes here are overwritten"
END_TEXT = "<<< AhuraRTOS END"

# Directories never searched for project sources. "cmake" holds the CubeMX
# sub-build and the toolchain files, which are read for the MCU identity but
# are never edit targets.
PRUNE = {
    ".git", ".vscode", ".settings", "__pycache__",
    "build", "Build", "Debug", "Release", "cmake-build-debug", "cmake-build-release",
    "Drivers", "Middlewares", "Utilities", "AhuraRTOS",
}


class Fatal(Exception):
    """A problem the user has to fix before anything can be written."""


# ---------------------------------------------------------------------------
# Text files that survive a round trip
# ---------------------------------------------------------------------------

class SourceFile:
    """A text file whose byte-level identity survives editing.

    CubeMX writes CRLF on Windows and sometimes leaves a UTF-8 BOM or a stray
    cp1252 byte in a comment. A naive read/write cycle would rewrite every line
    ending in the file and bury the real change in a whole-file diff, so
    encoding, BOM and newline style are captured on load and restored on save.
    Internally the text always uses '\\n'.
    """

    def __init__(self, path: Path):
        self.path = path
        raw = path.read_bytes()

        self.bom = raw.startswith(b"\xef\xbb\xbf")
        if self.bom:
            raw = raw[3:]

        for encoding in ("utf-8", "cp1252"):
            try:
                text = raw.decode(encoding)
            except UnicodeDecodeError:
                continue
            self.encoding = encoding
            break
        else:
            raise Fatal("cannot decode {} as UTF-8 or cp1252".format(path))

        self.newline = "\r\n" if "\r\n" in text else "\n"
        self.text = text.replace("\r\n", "\n")
        self.original = self.text

    def to_bytes(self) -> bytes:
        out = self.text.replace("\n", self.newline).encode(self.encoding)
        return b"\xef\xbb\xbf" + out if self.bom else out

    @property
    def changed(self) -> bool:
        return self.text != self.original

    def diff(self, root: Path) -> str:
        rel = relative(self.path, root)
        return "".join(difflib.unified_diff(
            self.original.splitlines(keepends=True),
            self.text.splitlines(keepends=True),
            fromfile=rel, tofile=rel, n=3,
        ))


def relative(path: Path, root: Path) -> str:
    """Project-relative POSIX path, for messages and for CMake."""
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


# ---------------------------------------------------------------------------
# Managed regions
# ---------------------------------------------------------------------------

def managed_block(payload: str, indent: str, comment: str = "c") -> str:
    """Wrap `payload` in the sentinel markers, indented to match its context."""
    if comment == "cmake":
        begin, end = "# " + BEGIN_TEXT, "# " + END_TEXT
    else:
        begin, end = "/* " + BEGIN_TEXT + " */", "/* " + END_TEXT + " */"

    lines = [indent + begin]
    lines += [(indent + line).rstrip() for line in payload.strip("\n").split("\n")]
    lines.append(indent + end)
    return "\n".join(lines) + "\n"


def find_managed(text: str, start: int = 0, stop: int = None):
    """Offsets of the whole managed region (markers included), or None."""
    window = text[start:stop if stop is not None else len(text)]

    b = re.search(r"^[ \t]*(?:/\*|#)[ \t]*" + re.escape(BEGIN_TEXT), window, re.M)
    if not b:
        return None
    e = re.search(r"^[ \t]*(?:/\*|#)[ \t]*" + re.escape(END_TEXT) + r".*$\n?",
                  window[b.start():], re.M)
    if not e:
        raise Fatal("found an unterminated AhuraRTOS block - remove it by hand and re-run")

    return start + b.start(), start + b.start() + e.end()


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
    """Insert or refresh our block inside a USER CODE section. Idempotent.

    Existing user content in the section is always preserved: on first install
    the block goes at `where` ("start" or "end") of whatever is already there,
    and on every later run only the block itself is rewritten.

    `where="start"` exists for USER CODE BEGIN WHILE, where appending would put
    os_init() *inside* the infinite loop rather than before it.
    """
    span = user_code_span(src.text, tag)
    if span is None:
        raise Fatal("no 'USER CODE BEGIN {}' section in {}".format(tag, src.path.name))

    start, end, begin_indent, end_indent = span
    body = src.text[start:end]
    block = managed_block(payload, begin_indent if where == "start" else end_indent)

    existing = find_managed(src.text, start, end)
    if existing:
        # Already ours: rewrite the region in place and leave the rest of the
        # section - including anything the user added around it - untouched.
        new_body = src.text[start:existing[0]] + block + src.text[existing[1]:end]
    # Whatever is already in the section is copied through byte for byte -
    # including its blank lines. `while (1) {` lives inside USER CODE WHILE, and
    # re-spacing generated code is not this script's job.
    elif where == "start":
        new_body = "\n" + block + "\n" + body.lstrip("\n")
    elif body.strip():
        new_body = body.rstrip("\n") + "\n\n" + block + "\n"
    else:
        new_body = "\n" + block + "\n"

    if new_body == body:
        return False
    src.text = src.text[:start] + new_body + src.text[end:]
    return True


def drop_managed(src: SourceFile) -> int:
    """Remove every managed region from the file. Returns how many went."""
    removed = 0
    while True:
        span = find_managed(src.text)
        if not span:
            return removed
        start, end = span
        # Take the blank line we added ahead of the block back out with it.
        while start >= 2 and src.text[start - 2:start] == "\n\n":
            start -= 1
        src.text = src.text[:start] + src.text[end:]
        removed += 1


# ---------------------------------------------------------------------------
# Reading the generated project
# ---------------------------------------------------------------------------

def walk_sources(root: Path):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in PRUNE and not d.startswith(".")]
        for name in filenames:
            if name.endswith((".c", ".h")):
                yield Path(dirpath) / name


def function_body(text: str, name: str):
    """The body of `void name(void)`, or None. Brace-matched, so nested blocks
    and the USER CODE comments inside come along."""
    m = re.search(r"\bvoid\s+" + re.escape(name) + r"\s*\(\s*void\s*\)\s*\{", text)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(text) and depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[m.end():i - 1]


def strip_comments(text: str) -> str:
    """Blank out comments and strings so a search sees code, not prose.

    Without this, the commented-out `HAL_IncTick()` that CubeMX leaves behind
    in some versions reads as a live call and the tick check fires wrongly.
    """
    return re.sub(r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\])*\"", " ", text, flags=re.S)


class Project:
    """Everything the installer needs to know, all of it read from generated
    sources - never from the .ioc."""

    def __init__(self, root: Path, app_dir: Path = None):
        self.root = root

        top = root / "CMakeLists.txt"
        if not top.is_file():
            raise Fatal(
                "no CMakeLists.txt here.\n"
                "  Run this from the root of your project - the directory holding\n"
                "  CMakeLists.txt and the .ioc. If your project has a .cproject but no\n"
                "  CMakeLists.txt it was generated for STM32CubeIDE; regenerate it with\n"
                "  Project Manager -> Toolchain/IDE -> CMake, or install by hand from\n"
                "  doc/installation.md."
            )
        self.cmake = SourceFile(top)

        sources = list(walk_sources(root))
        if app_dir:
            app_dir = (root / app_dir).resolve()
            sources = [p for p in sources if app_dir in p.resolve().parents or p.parent == app_dir]

        self.main_c = self._pick(
            [p for p in sources
             if p.name == "main.c" and re.search(r"\bint\s+main\s*\(", p.read_text(errors="replace"))],
            "main.c")

        self.it_c = self._pick(
            [p for p in sources
             if p.suffix == ".c" and "void SysTick_Handler" in p.read_text(errors="replace")],
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
                "      python install.py --app-dir CM7".format(what, listing))
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
        for toolchain in sorted((self.root / "cmake").glob("*.cmake")) if (self.root / "cmake").is_dir() else []:
            m = re.search(r"-mcpu=(cortex-m[0-9a-z.+]*)", toolchain.read_text(errors="replace"))
            if m:
                core = m.group(1)
                break
        return mcu, core

    # -- preflight ----------------------------------------------------------

    def check(self, tick: str):
        """Everything that would make the build fail or the board hang, checked
        before a single byte is written."""
        it_text = self.it_c.read_text(errors="replace")
        code = strip_comments(it_text)

        # Step 5. The port defines PendSV_Handler; a CubeMX stub is a duplicate
        # symbol. Deleting generated code is not this script's call - and CubeMX
        # would put it straight back on the next regeneration anyway.
        for path in [self.it_c, self.main_c]:
            if re.search(r"\bvoid\s+PendSV_Handler\s*\(", strip_comments(path.read_text(errors="replace"))):
                raise Fatal(
                    "{} defines PendSV_Handler, which the kernel's port also defines.\n"
                    "  Fix it in CubeMX, not here - it is generated code and would come\n"
                    "  back on the next regeneration:\n"
                    "      Pinout & Configuration -> System Core -> NVIC -> Code generation\n"
                    "      -> uncheck 'Generate IRQ handler' for 'Pendable request for\n"
                    "         system service'\n"
                    "  Regenerate, then run this script again."
                    .format(relative(path, self.root)))

        # Two RTOSes on one PendSV cannot both win.
        if re.search(r"\b(osKernelInitialize|MX_FREERTOS_Init|xPortStartScheduler)\b", code) \
                or (self.root / "Middlewares" / "Third_Party" / "FreeRTOS").is_dir():
            raise Fatal(
                "this project already has FreeRTOS/CMSIS-RTOS in it.\n"
                "  Both kernels claim PendSV and the tick, so they cannot coexist.\n"
                "  Turn FreeRTOS off in CubeMX (Middleware -> FREERTOS -> Interface:\n"
                "  Disabled), regenerate, then run this script again.")

        if tick == "external":
            return  # the user drives os_tick_handler() from their own timer

        # Step 4. SysTick must be free. When the HAL timebase is still SysTick,
        # CubeMX emits HAL_IncTick() into the handler - two time bases on one
        # interrupt drift against each other, so this is a stop.
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
# Getting the kernel
# ---------------------------------------------------------------------------

@contextlib.contextmanager
def kernel_source(source: str, ref: str):
    """Yield a directory that is the kernel/ tree, from wherever it can be had."""
    if source:
        given = Path(source).expanduser().resolve()
        for candidate in (given, given / "kernel"):
            if (candidate / "ahura.h").is_file():
                yield candidate
                return
        raise Fatal("--source {} does not contain a kernel (no ahura.h)".format(source))

    # Running from inside a checkout - tools/install.py next to kernel/.
    if "__file__" in globals():
        local = Path(__file__).resolve().parent.parent / "kernel"
        if (local / "ahura.h").is_file():
            yield local
            return

    url = TARBALL.format(repo=REPO, ref=ref)
    print("  downloading {} ...".format(url))
    try:
        with urllib.request.urlopen(url, timeout=60) as response:
            payload = response.read()
    except (urllib.error.URLError, OSError) as exc:
        raise Fatal(
            "could not download the kernel: {}\n"
            "  Clone it yourself and point the script at the clone instead:\n"
            "      git clone https://github.com/{}.git\n"
            "      python install.py --source AhuraRTOS".format(exc, REPO))

    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as tar:
            # A GitHub tarball has no absolute or parent paths, but this is
            # archive extraction: check rather than trust.
            for member in tar.getmembers():
                if member.name.startswith("/") or ".." in Path(member.name).parts:
                    raise Fatal("refusing to extract unsafe path from tarball: " + member.name)
            # filter= is 3.12+; on 3.14 'data' is the default anyway.
            try:
                tar.extractall(tmp, filter="data")
            except TypeError:
                tar.extractall(tmp)
        roots = [p for p in Path(tmp).iterdir() if p.is_dir()]
        if len(roots) != 1 or not (roots[0] / "kernel" / "ahura.h").is_file():
            raise Fatal("downloaded archive does not look like the AhuraRTOS repository")
        yield roots[0] / "kernel"


def looks_like_kernel(path: Path) -> bool:
    return (path / "ahura.h").is_file() and (path / "core" / "os_kernel.c").is_file()


# ---------------------------------------------------------------------------
# The plan
# ---------------------------------------------------------------------------

# The payloads below are written exactly as they will appear in the file,
# comment markers and all. An earlier version derived the comments from prose
# by pattern-matching which lines "looked like code", and quietly commented out
# a set_property() call - so the templates are now literal.

CMAKE_BLOCK = """\
# Everything the kernel needs from this build lives in this one block, appended after the
# CubeMX-generated part rather than woven into it: target_sources() and target_link_libraries()
# both APPEND, so a later call adds to what is already there. The generated blocks above stay
# exactly as CubeMX wrote them, and the whole integration is one region to read or remove.

# OS_CONFIG_DIR must be set BEFORE add_subdirectory: it tells the kernel library where this
# project's os_config.h lives. The kernel and the application have to compile against the same
# configuration - if only the application saw the file, their structure sizes would silently
# disagree.
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


def plan(project: Project, kernel_dir: Path, tick: str, force: bool):
    """Return (file edits, copy actions, notes) without touching the disk."""
    edits, copies, notes = [], [], []

    kernel_dest = project.root / "AhuraRTOS" / "kernel"
    copies.append(("kernel", kernel_dir, kernel_dest))

    for name, dest_dir in (("os_config.h", project.inc_dir),
                           ("os_cb.c", project.src_dir),
                           ("os_main.c", project.src_dir)):
        dest = dest_dir / name
        if dest.exists() and not force:
            notes.append("kept your existing {} (use --force-templates to replace it)"
                         .format(relative(dest, project.root)))
        else:
            copies.append(("template", kernel_dir / "template" / name, dest))

    # CMakeLists.txt: append, or refresh in place.
    body = CMAKE_BLOCK.format(
        cfg=relative(project.inc_dir, project.root),
        src=relative(project.src_dir, project.root),
        kernel=relative(kernel_dest, project.root),
        target=project.target,
    )
    block = managed_block(body, "", comment="cmake")
    existing = find_managed(project.cmake.text, 0)
    if existing:
        project.cmake.text = project.cmake.text[:existing[0]] + block + project.cmake.text[existing[1]:]
    else:
        project.cmake.text = project.cmake.text.rstrip("\n") + "\n\n" + block
    if project.cmake.changed:
        edits.append(project.cmake)

    # The interrupt file: include + tick.
    it = SourceFile(project.it_c)
    put_in_user_code(it, "Includes", '#include "ahura.h"')
    if tick == "systick":
        put_in_user_code(it, "SysTick_IRQn 0", TICK_PAYLOAD)
    else:
        notes.append("--tick external: wire os_tick_handler() to your own timer yourself, "
                     "and set OS_CONFIG_TICK_SOURCE_EXTERNAL in os_config.h")
    if it.changed:
        edits.append(it)

    # main.c: include + boot. USER CODE BEGIN WHILE is the last user section
    # before the infinite loop, so everything main() sets up has already run.
    mc = SourceFile(project.main_c)
    put_in_user_code(mc, "Includes", '#include "ahura.h"')
    put_in_user_code(mc, "WHILE", BOOT_PAYLOAD, where="start")
    if mc.changed:
        edits.append(mc)

    return edits, copies, notes


def plan_uninstall(project: Project):
    edits = []
    for path in (project.root / "CMakeLists.txt", project.it_c, project.main_c):
        src = SourceFile(path)
        if drop_managed(src) and src.changed:
            edits.append(src)
    return edits


# ---------------------------------------------------------------------------
# Applying, with rollback
# ---------------------------------------------------------------------------

def apply(edits, copies, root: Path):
    """Write everything, or put it all back. Originals are held in memory, so a
    failure halfway - a read-only file, a full disk - leaves no half-install."""
    written, created = [], []
    try:
        for kind, src, dest in copies:
            if kind == "kernel":
                if dest.exists():
                    if not looks_like_kernel(dest):
                        raise Fatal(
                            "{} exists but does not look like the kernel (no ahura.h).\n"
                            "  Refusing to delete it. Move it aside and re-run."
                            .format(relative(dest, root)))
                    # Updating is a replacement, never a merge: a file dropped
                    # upstream would otherwise linger and keep compiling.
                    shutil.rmtree(dest)
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copytree(src, dest)
                created.append(dest)
                print("  kernel  -> {}".format(relative(dest, root)))
            else:
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(src, dest)
                created.append(dest)
                print("  copied  -> {}".format(relative(dest, root)))

        for src in edits:
            tmp = src.path.with_suffix(src.path.suffix + ".ahura-tmp")
            tmp.write_bytes(src.to_bytes())
            os.replace(tmp, src.path)
            written.append(src)
            print("  patched -> {}".format(relative(src.path, root)))

    except Exception:
        print("\n! failed - rolling back", file=sys.stderr)
        for src in written:
            src.path.write_bytes(
                src.original.replace("\n", src.newline).encode(src.encoding))
        for path in created:
            shutil.rmtree(path, ignore_errors=True) if path.is_dir() else path.unlink(missing_ok=True)
        raise


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="install.py",
        description="Install AhuraRTOS into an STM32CubeMX-generated CMake project. "
                    "Run it from the project root. It never touches the .ioc.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project", default=".", metavar="DIR",
                        help="project root (default: the current directory)")
    parser.add_argument("--app-dir", metavar="DIR",
                        help="which source tree to install into, on a dual-core part")
    parser.add_argument("--source", metavar="PATH",
                        help="use this AhuraRTOS checkout instead of downloading")
    parser.add_argument("--ref", default="main", metavar="REF",
                        help="branch or tag to download (default: main)")
    parser.add_argument("--tick", choices=("systick", "external"), default="systick",
                        help="tick source (default: systick)")
    parser.add_argument("--force-templates", action="store_true",
                        help="overwrite an existing os_config.h / os_cb.c / os_main.c")
    parser.add_argument("--dry-run", action="store_true",
                        help="show the diff and exit without writing")
    parser.add_argument("-y", "--yes", action="store_true",
                        help="do not ask for confirmation")
    parser.add_argument("--uninstall", action="store_true",
                        help="remove the managed blocks and AhuraRTOS/kernel")
    args = parser.parse_args(argv)

    root = Path(args.project).expanduser().resolve()

    try:
        project = Project(root, args.app_dir)

        print("AhuraRTOS installer")
        print("  project   {}".format(root))
        if project.mcu:
            print("  device    {}{}".format(project.mcu,
                                            " ({})".format(project.core) if project.core else ""))
        print("  target    {}".format(project.target))
        print("  sources   {} / {}".format(relative(project.main_c, root),
                                           relative(project.it_c, root)))
        print()

        if args.uninstall:
            edits, copies, notes = plan_uninstall(project), [], []
            kernel_dest = root / "AhuraRTOS" / "kernel"
            if kernel_dest.is_dir() and looks_like_kernel(kernel_dest):
                notes.append("AhuraRTOS/kernel will be deleted")
            notes.append("your os_config.h, os_cb.c and os_main.c are left alone - "
                         "they are your files now")
        else:
            project.check(args.tick)
            with kernel_source(args.source, args.ref) as kernel_dir:
                edits, copies, notes = plan(project, kernel_dir, args.tick, args.force_templates)

                if not edits and not copies:
                    print("Already installed and up to date. Nothing to do.")
                    return 0
                return finish(args, project, root, edits, copies, notes, kernel_dir)

        return finish(args, project, root, edits, copies, notes, None)

    except Fatal as exc:
        print("\nerror: {}".format(exc), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ncancelled", file=sys.stderr)
        return 130


def finish(args, project, root, edits, copies, notes, kernel_dir):
    for src in edits:
        print(src.diff(root))

    for kind, src, dest in copies:
        print("  {} {} -> {}".format("copy" if kind == "template" else "tree",
                                     src.name if kind == "template" else "kernel/",
                                     relative(dest, root)))
    for note in notes:
        print("  note: {}".format(note))
    print()

    if args.dry_run:
        print("Dry run - nothing was written.")
        return 0

    if not args.yes:
        if not sys.stdin.isatty():
            print("Nothing written: stdin is not a terminal, so there is no way to confirm.\n"
                  "Re-run with --yes to apply.")
            return 1
        if input("Apply these changes? [y/N] ").strip().lower() not in ("y", "yes"):
            print("Cancelled - nothing was written.")
            return 0

    if args.uninstall:
        kernel_dest = root / "AhuraRTOS" / "kernel"
        apply(edits, [], root)
        if kernel_dest.is_dir() and looks_like_kernel(kernel_dest):
            shutil.rmtree(kernel_dest)
            print("  removed -> {}".format(relative(kernel_dest, root)))
            with contextlib.suppress(OSError):
                kernel_dest.parent.rmdir()
        print("\nUninstalled. os_config.h, os_cb.c and os_main.c were left in place.")
        return 0

    apply(edits, copies, root)
    print("\nInstalled. Next:")
    print("  1. build - the kernel prints the port it chose: 'Ahura kernel arch: ...'")
    print("  2. write your code in {}".format(relative(project.src_dir / "os_main.c", root)))
    print("  3. to prove the port on this board first, set OS_CONFIG_TEST_ENABLE to 1")
    print("     in {} and rebuild (doc/self-test.md)"
          .format(relative(project.inc_dir / "os_config.h", root)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
