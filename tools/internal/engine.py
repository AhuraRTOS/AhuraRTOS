"""
***************************************************************************************************
 * @file        internal/engine.py
 * @author      Nima Askari
 * @github      https://github.com/AhuraRTOS/AhuraRTOS
 * @version     1.0.0
 * @date        2026
 * @brief       The installer engine - everything that is the same on every platform.
 **************************************************************************************************

THIS FILE IS NOT AN INSTALLER. Run one of these instead:

    tools/install_rpi_online.py     tools/install_rpi_offline.py
    tools/install_stm32_online.py   tools/install_stm32_offline.py

Each of those is a small bootstrap: it locates an AhuraRTOS checkout - downloading one if it has
to - and then loads this file out of that checkout, together with the matching descriptor from
tools/internal/. So piping a bootstrap straight into Python still works with nothing else on
disk, while the machinery below exists in exactly one place.

WHAT LIVES HERE

Everything that does not name a vendor: reading and rewriting a source file, the >>> AhuraRTOS
BEGIN / <<< AhuraRTOS END managed blocks and the reconciliation that makes a second run a no-op,
refusing an integration this installer did not write, walking the project, showing the diff,
applying it with rollback, and the uninstall path.

WHAT A PLATFORM ADDS

tools/internal/<name>.py supplies the parts that do name a vendor - how to recognise the
project, which files to copy, the CMake block, and the handful of C edits that differ - behind
this contract:

    NAME, TITLE, PROG, DESCRIPTION, DOC   identification, for --help and the banner
    detect(root) -> bool                  is this that kind of project?
    add_arguments(parser)                 platform-only flags, e.g. --soc or --tick
    Project(root, args)                   .check(args), .banner() -> list of lines
    plan(project, repo_dir, args, copy_tree) -> (edits, copies, notes)
    plan_uninstall(project) -> edits

Adding a platform is one file there. Nothing in this file changes.

@copyright (c) 2026 Ahura Project Contributors
            See LICENSE in the project root for the full license text.
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


# ---------------------------------------------------------------------------
# Shared machinery
# ---------------------------------------------------------------------------

def add_common_arguments(parser):
    """The flags every installer accepts, so they cannot drift apart."""
    parser.add_argument("--project", default=".", metavar="DIR",
                        help="project root (default: the current directory)")
    parser.add_argument("--source", metavar="PATH",
                        help="use this AhuraRTOS checkout instead of downloading")
    parser.add_argument("--update", action="store_true",
                        help="replace an AhuraRTOS/ already in the project with the "
                             "current version (otherwise it is left alone)")
    parser.add_argument("--force-templates", action="store_true",
                        help="overwrite an existing os_config.h / os_cb.c / os_main.c")
    parser.add_argument("--dry-run", action="store_true",
                        help="show the diff and exit without writing")
    parser.add_argument("-y", "--yes", action="store_true",
                        help="do not ask for confirmation")
    parser.add_argument("--uninstall", action="store_true",
                        help="remove the managed blocks and the AhuraRTOS directory")
    return parser


REPO = "AhuraRTOS/AhuraRTOS"


TARBALL = "https://codeload.github.com/{repo}/tar.gz/{ref}"


BEGIN_TEXT = ">>> AhuraRTOS BEGIN - managed block, changes here are overwritten"


END_TEXT = "<<< AhuraRTOS END"


class Fatal(Exception):
    """A problem the user has to fix before anything can be written."""


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


def ask(prompt: str):
    """Read one line from the user, or None if there is no terminal to ask at.

    input() is not enough. In the one-line install the script itself arrives on
    stdin - `curl ... | python -` - so stdin is the source code, already read to
    EOF, and input() would raise immediately. The controlling terminal is still
    open in that case, so ask it directly: /dev/tty on POSIX, CONIN$ on Windows.
    """
    if sys.stdin.isatty():
        try:
            return input(prompt)
        except EOFError:
            return None

    # stdin is the script itself, so ask the terminal directly - but only when
    # something is likely to answer. That /dev/tty opens proves a terminal
    # device exists, not that a human is watching it: Git Bash and CI runners
    # both provide one, and a blocking read there hangs the install forever.
    # An unredirected stdout is the usable signal - that is where the prompt
    # goes, so if it is not a terminal, nobody is reading the question either.
    if not sys.stdout.isatty():
        return None

    try:
        terminal = open("CONIN$" if os.name == "nt" else "/dev/tty")
    except OSError:
        return None
    with terminal:
        sys.stdout.write(prompt)
        sys.stdout.flush()
        line = terminal.readline()
    return line or None


def relative(path: Path, root: Path) -> str:
    """Project-relative POSIX path, for messages and for CMake."""
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def strip_comments(text: str) -> str:
    """Blank out comments and string literals so a search sees code, not prose.

    Length-preserving - every removed character becomes a space and newlines
    stay - so offsets into the result still index the original text. Without
    this, a commented-out HAL_IncTick() reads as a live call, and the word
    "os_init()" inside our own explanatory comment would satisfy a placement
    check that nothing actually calls it.
    """
    return re.sub(r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\])*\"",
                  lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)


def function_span(text: str, name: str):
    """(start, end) of a whole `<type> name(void) { ... }`, or None.

    Brace-matched over comment-stripped text, so a brace inside a comment or a
    string cannot end the function early.
    """
    code = strip_comments(text)
    m = re.search(r"^[ \t]*(?:__weak[ \t]+)?(?:void|int)[ \t]+" + re.escape(name) +
                  r"[ \t]*\([ \t]*void[ \t]*\)[ \t]*\n?[ \t]*\{", code, re.M)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(code) and depth:
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
        i += 1
    if depth:
        return None
    while i < len(text) and text[i] == "\n":
        i += 1
    return m.start(), i


def function_body(text: str, name: str):
    """The inside of a function, or None."""
    span = function_span(text, name)
    if span is None:
        return None
    start, end = span
    return text[text.index("{", start) + 1:text.rindex("}", start, end)]


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


def without_managed(text: str) -> str:
    """The text with every managed region cut out, so a search sees only what somebody wrote."""
    out = text
    while True:
        span = find_managed(out)
        if span is None:
            return out
        out = out[:span[0]] + out[span[1]:]


def refuse_duplicate_call(body: str, call: str, where: str, fix: str):
    """Stop if `call` already appears outside any managed block.

    The installer owns only what it marked, so a hand-written call is invisible to the rebuild and
    the managed block lands BESIDE it. Two calls to a tick handler is not a broken build - it is a
    kernel clock at double rate, which every test measuring time in ticks agrees with.
    """
    if re.search(r"\b" + re.escape(call) + r"\s*\(", strip_comments(without_managed(body))):
        raise Fatal(
            "{where} already calls {call}() outside any AhuraRTOS block.\n"
            "  \n"
            "  This installer only owns what it marked, so it cannot replace that call - it would\n"
            "  add its own beside it, and {call}() twice in one handler means a kernel clock\n"
            "  running at DOUBLE rate. Nothing reports it: every test measures time in ticks, so\n"
            "  they all stay consistent with each other and pass.\n"
            "  \n"
            "  {fix}".format(where=where, call=call, fix=fix))


def drop_managed(src: SourceFile) -> int:
    """Remove every managed region from the file. Returns how many went.

    A region that only added code is deleted. A region that *disabled* code
    carries the original verbatim inside `#if 0 ... #endif`, and removing it
    means putting that code back - deleting it would throw away generated code
    this script does not own.
    """
    removed = 0
    while True:
        span = find_managed(src.text)
        if not span:
            return removed
        start, end = span

        restored = re.search(r"^#if 0\n(.*?)^#endif\n", src.text[start:end], re.S | re.M)
        if restored:
            replacement = restored.group(1)
        else:
            replacement = ""
            # Take the blank line we added ahead of the block back out with it.
            while start >= 2 and src.text[start - 2:start] == "\n\n":
                start -= 1

        src.text = src.text[:start] + replacement + src.text[end:]
        removed += 1


def unmanaged_integration(cmake) -> bool:
    """True when the CMakeLists already links the kernel OUTSIDE a managed block.

    An installer only owns what sits between its own sentinels: every run drops
    those regions and rebuilds them, which is what makes a second run a no-op.
    A hand-written integration - added before the installer existed, or by
    following doc/installation.md - carries no sentinels, so it is invisible to
    that reconciliation. Appending beside it produces two add_subdirectory()
    calls for one directory, which CMake rejects, and two OS_CONFIG_DIR values,
    which it does not.

    So this is checked before anything is written, and it is a stop rather than
    a merge: which of the two blocks is authoritative is the user's decision,
    not something to guess at.
    """
    text = cmake.text
    kept, pos = [], 0
    while True:
        span = find_managed(text, pos)
        if span is None:
            break
        kept.append(text[pos:span[0]])
        pos = span[1]
    kept.append(text[pos:])

    # CMake comments, so the explanatory prose in a managed block that someone
    # copied by hand does not read as a live call.
    lines = [ln for ln in "".join(kept).split("\n")
             if not ln.lstrip().startswith("#")]
    return bool(re.search(r"\bahura_kernel\b", "\n".join(lines)))


UNMANAGED_MESSAGE = """{where} already integrates AhuraRTOS, but not in a block this installer wrote -
  there are no '>>> AhuraRTOS BEGIN' markers around it.

  That happens when the kernel was added by hand, by following
  doc/installation.md, or with a version of this script from before the managed
  blocks existed. An installer only owns what it marked - every run drops those
  regions and rebuilds them, which is what makes a second run a no-op - so a
  hand-written integration is invisible to it. Appending beside one leaves two
  integrations in a single file: two add_subdirectory() calls for one directory,
  which CMake rejects, and two OS_CONFIG_DIR values, which it does not.

  Delete the existing AhuraRTOS lines from {where} - the add_subdirectory(), the
  OS_CONFIG_DIR, the target_sources() and the ahura_kernel link - then run this
  again. It writes the same integration back, in a block it can keep up to date
  from then on.

  Your os_config.h, os_cb.c and os_main.c are untouched either way."""


def refuse_unmanaged(cmake, root):
    """Raise the standard explanation for unmanaged_integration()."""
    if not unmanaged_integration(cmake):
        return

    where = relative(cmake.path, root)
    raise Fatal(UNMANAGED_MESSAGE.format(where=where))


def walk_sources(root: Path, prune=frozenset()):
    """Every .c/.h under root, skipping `prune` and dot-directories."""
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in prune and not d.startswith(".")]
        for name in filenames:
            if name.endswith((".c", ".h")):
                yield Path(dirpath) / name


def looks_like_ahura(path: Path) -> bool:
    """A usable AhuraRTOS tree: the kernel and the three templates to copy."""
    return ((path / "ahura.h").is_file()
            and (path / "template" / "os_config.h").is_file())


def repo_source(source: str, ref: str):
    """Yield a directory that is the AhuraRTOS repository root."""
    if source:
        given = Path(source).expanduser().resolve()
        if looks_like_ahura(given):
            yield given
            return
        raise Fatal("--source {} is not an AhuraRTOS checkout "
                    "(no ahura.h in it)".format(source))

    # Running from inside a checkout - tools/install_stm32.py beside ahura.h and kernel/.
    #
    # __file__ is not a reliable signal on its own: piped in as `python -` it is
    # set to the literal string "<stdin>", which resolves against the working
    # directory and would point two levels above the project. Requiring it to be
    # an existing file rules that out.
    here = globals().get("__file__")
    if here and Path(here).is_file():
        local = Path(here).resolve().parent.parent
        if looks_like_ahura(local):
            yield local
            return

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

    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as tar:
            # A GitHub tarball has no absolute or parent paths, no symlinks and
            # no hardlinks, but this is archive extraction: check rather than
            # trust. The link check matters specifically for the fallback
            # below - filter="data" rejects escaping links itself, but on
            # Python < 3.12 there is no filter, and a link member whose
            # linkname points outside the directory is the one way a member
            # with a perfectly innocent name still writes outside tmp.
            for member in tar.getmembers():
                if member.name.startswith("/") or ".." in Path(member.name).parts:
                    raise Fatal("refusing to extract unsafe path from tarball: " + member.name)
                if member.issym() or member.islnk():
                    raise Fatal("refusing to extract link member from tarball: {} -> {}"
                                .format(member.name, member.linkname))
            try:
                tar.extractall(tmp, filter="data")   # filter= is 3.12+
            except TypeError:
                tar.extractall(tmp)
        roots = [p for p in Path(tmp).iterdir() if p.is_dir()]
        if len(roots) != 1 or not looks_like_ahura(roots[0]):
            raise Fatal("downloaded archive does not look like the AhuraRTOS repository")
        yield roots[0]


def apply(edits, copies, root: Path):
    """Write everything, or put it all back. Originals are held in memory, so a
    failure halfway - a read-only file, a full disk - leaves no half-install."""
    written, created = [], []
    try:
        for kind, src, dest in copies:
            if kind == "repo":
                if dest.exists():
                    if not looks_like_ahura(dest):
                        raise Fatal(
                            "{} exists but is not an AhuraRTOS checkout (no\n"
                            "  ahura.h in it). Refusing to delete it - move it aside\n"
                            "  and re-run.".format(relative(dest, root)))
                    # Updating is a replacement, never a merge: a file dropped
                    # upstream would otherwise linger and keep compiling.
                    shutil.rmtree(dest)
                shutil.copytree(src, dest, ignore=shutil.ignore_patterns(".git"))
                created.append(dest)
                print("  tree    -> {}".format(relative(dest, root)))
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
            if path.is_dir():
                shutil.rmtree(path, ignore_errors=True)
            else:
                path.unlink(missing_ok=True)
        raise


def finish(args, project, root, edits, copies, notes):
    for src in edits:
        print(src.diff(root))

    for kind, src, dest in copies:
        print("  {}  {} -> {}".format("tree  " if kind == "repo" else "copy  ",
                                      "AhuraRTOS" if kind == "repo" else src.name,
                                      relative(dest, root)))
    for note in notes:
        print("  note: {}".format(note))
    print()

    if args.dry_run:
        print("Dry run - nothing was written.")
        return 0

    if not args.yes:
        answer = ask("Apply these changes? [y/N] ")
        if answer is None:
            print("Nothing written: there is no terminal to confirm at.\n"
                  "Add --yes to apply, or --dry-run to see this diff again.")
            return 1
        if answer.strip().lower() not in ("y", "yes"):
            print("Cancelled - nothing was written.")
            return 0

    if args.uninstall:
        apply(edits, [], root)
        ahura = root / "AhuraRTOS"
        if ahura.is_dir() and looks_like_ahura(ahura):
            shutil.rmtree(ahura)
            print("  removed -> {}".format(relative(ahura, root)))
        print("\nUninstalled. os_config.h, os_cb.c and os_main.c were left in place,")
        print("and any PendSV_Handler that was disabled is back exactly as it was.")
        return 0

    apply(edits, copies, root)
    print("\nInstalled. Next:")
    print("  1. build - the kernel prints the port it chose: 'Ahura kernel arch: ...'")
    print("  2. write your code in {}".format(relative(project.src_dir / "os_main.c", root)))
    print("  3. to prove the port on this board first, set OS_CONFIG_TEST_ENABLE to 1")
    print("     in {} and rebuild (doc/self-test.md)"
          .format(relative(project.inc_dir / "os_config.h", root)))
    return 0


# ---------------------------------------------------------------------------
# The runner
# ---------------------------------------------------------------------------

def run(platform, repo_dir, argv=None, title="AhuraRTOS installer", show_kernel=False):
    """Install `platform` into the project named on the command line.

    `repo_dir` is the checkout the bootstrap already located - the same directory this file was
    loaded from. It is passed in rather than found again so that the download, if there was one,
    happens exactly once and its temporary directory is still alive.

    `title` and `show_kernel` are the bootstrap's half of the banner: an offline install names
    itself as one and says which checkout it is reading, because there the answer is a choice the
    user made rather than a download that just happened.
    """
    parser = argparse.ArgumentParser(prog=platform.PROG, description=platform.DESCRIPTION)
    add_common_arguments(parser)
    parser.add_argument("--ref", default="main", metavar="REF",
                        help="branch or tag to download (default: main)")
    platform.add_arguments(parser)
    args = parser.parse_args(argv)

    root = Path(args.project).expanduser().resolve()

    try:
        project = platform.Project(root, args)

        print(title)
        print("  project   {}".format(root))
        if show_kernel:
            print("  kernel    {}".format(relative(repo_dir, root)))
        for line in project.banner():
            print(line)
        print()

        if args.uninstall:
            edits, notes = platform.plan_uninstall(project), []
            if (root / "AhuraRTOS").is_dir() and looks_like_ahura(root / "AhuraRTOS"):
                notes.append("AhuraRTOS/ will be deleted")
            notes.append("your os_config.h, os_cb.c and os_main.c are left alone - "
                         "they are your files now")
            if not edits and not notes:
                print("Nothing installed here.")
                return 0
            return finish(args, project, root, edits, [], notes)

        project.check(args)

        # A project that already carries AhuraRTOS/ has everything needed to finish - kernel and
        # templates both - so a re-run neither downloads nor replaces it. That is what makes
        # running this twice free, and what keeps a second run from resetting a tree pinned
        # deliberately. --update is the way to ask for the current version.
        installed = looks_like_ahura(root / "AhuraRTOS")
        copy_tree = not installed or args.update or bool(args.source)
        source = root / "AhuraRTOS" if (installed and not copy_tree) else repo_dir

        edits, copies, notes = platform.plan(project, source, args, copy_tree=copy_tree)
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
