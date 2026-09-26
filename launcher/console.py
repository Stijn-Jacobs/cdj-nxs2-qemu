# SPDX-License-Identifier: GPL-2.0-or-later
"""Terminal output and questions for setup: the step headers, the coloured
status lines, the prompts, and a long command run with a spinner and a log."""

import os
import re
import shutil
import subprocess
import sys
import time


def stdin_is_tty():
    return os.environ.get("LAUNCHER_TTY_IN") == "1" or sys.stdin.isatty()


def stdout_is_tty():
    return os.environ.get("LAUNCHER_TTY_OUT") == "1" or sys.stdout.isatty()


class Console:
    def __init__(self, dry, interactive):
        self.out = sys.stdout
        self.dry = dry
        self.interactive = interactive
        # A Windows console sets no TERM and handles the escapes once enabled.
        self.tty = stdout_is_tty() and not os.environ.get("NO_COLOR") and \
            os.environ.get("TERM", "xterm" if os.name == "nt" else "dumb") != "dumb"
        if self.tty:
            self.B, self.D, self.G, self.Y, self.R, self.C, self.N = (
                "\033[1m", "\033[2m", "\033[32m", "\033[33m", "\033[31m", "\033[36m", "\033[0m")
            if os.name == "nt":
                os.system("")  # enables the console's escape sequences
        else:
            self.B = self.D = self.G = self.Y = self.R = self.C = self.N = ""
        loc = os.environ.get("LC_ALL") or os.environ.get("LC_CTYPE") or os.environ.get("LANG") or ""
        utf8 = re.search(r"utf-?8", loc, re.I) or (getattr(self.out, "encoding", "") or "").lower() == "utf-8"
        if utf8 and hasattr(self.out, "reconfigure"):
            # Windows' Python writes a redirected stream in the ANSI code page.
            self.out.reconfigure(encoding="utf-8", errors="replace")
            sys.stderr.reconfigure(encoding="utf-8", errors="replace")
        if utf8:
            self.OK, self.BAD, self.WARN, self.SPIN = "✔", "✖", "!", \
                "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"
        else:
            self.OK, self.BAD, self.WARN, self.SPIN = "ok", "XX", "!!", "|/-\\"

    def _p(self, s=""):
        self.out.write(s + "\n")
        self.out.flush()

    def step(self, n, title):
        self._p("\n%s[%s/6] %s%s" % (self.B + self.C, n, title, self.N))

    def info(self, s):
        self._p("  " + s)

    def good(self, s):
        self._p("  %s%s%s %s" % (self.G, self.OK, self.N, s))

    def warn(self, s):
        self._p("  %s%s%s %s" % (self.Y, self.WARN, self.N, s))

    def fail(self, s):
        self._p("  %s%s%s %s" % (self.R, self.BAD, self.N, s))

    def dim(self, s):
        self._p("  %s%s%s" % (self.D, s, self.N))

    def would(self, s):
        self._p("  %s(dry run) would run:%s %s" % (self.Y, self.N, s))

    def die(self, s):
        sys.stderr.write("\n%s%s %s%s\n" % (self.R, self.BAD, s, self.N))
        raise SystemExit(1)

    def ask(self, q, default=""):
        """The answer, or the default when not asking."""
        if not self.interactive:
            return default
        sys.stderr.write("  %s%s%s%s " % (self.B, q, self.N, " [%s]" % default if default else ""))
        sys.stderr.flush()
        a = sys.stdin.readline()
        if not a:
            # End of input: nobody is there to answer. Windows' NUL device
            # passes isatty(), so this is the check that holds everywhere.
            self.interactive = False
            return default
        a = a.rstrip("\r\n")
        return a if a else default

    def ask_yn(self, q, default):
        return self.ask(q + " (y/n)", default) in ("y", "Y", "yes", "Yes")

    def choose(self, q, default, *options):
        while True:
            a = self.ask("%s (%s)" % (q, " ".join(options)), default)
            if a in options:
                return a
            if not self.interactive:
                return default
            sys.stderr.write("  %splease answer one of: %s%s\n" % (self.Y, " ".join(options), self.N))

    def run_phase(self, label, log, hint, argv, rel=None, env=None, cwd=None):
        """Run a noisy command with its output in `log`. On a terminal: a
        spinner, the elapsed time and the build's own progress ([n/m] from
        ninja); without one a line every 30 s. On failure: the log's tail and
        the hint. Returns True on success."""
        shown = os.path.relpath(log, rel) if rel else log
        if self.dry:
            self.would("%s > %s" % (" ".join(argv), shown))
            return True
        os.makedirs(os.path.dirname(log), exist_ok=True)
        t0 = time.time()
        with open(log, "wb") as f:
            p = subprocess.Popen(argv, stdout=f, stderr=subprocess.STDOUT, env=env, cwd=cwd,
                                 stdin=subprocess.DEVNULL)
            i, last = 0, 0.0
            cols = shutil.get_terminal_size((80, 24)).columns
            try:
                while p.poll() is None:
                    detail = _progress(log)
                    if self.tty:
                        line = "  %s %s  %s  %s" % (self.SPIN[i % len(self.SPIN)], _elapsed(t0), label, detail)
                        self.out.write("\r\033[K" + line[:cols - 1])
                        self.out.flush()
                        i += 1
                    elif time.time() - last >= 30:
                        last = time.time()
                        self._p("  ... %s  %s  %.80s" % (_elapsed(t0), label, detail))
                    time.sleep(0.25)
            except KeyboardInterrupt:
                # The same Ctrl-C reached the command, which stops on its own
                # (a deck's teardown quits MAIN through its monitor first).
                p.wait()
                raise
        rc = p.wait()
        if self.tty:
            self.out.write("\r\033[K")
        failed = rc != 0 or _has_failed_line(log)
        if not failed:
            self.good("%s  %s(%s, log %s)%s" % (label, self.D, _elapsed(t0), shown, self.N))
            return True
        self.fail("%s failed after %s -- the end of %s:" % (label, _elapsed(t0), shown))
        for line in _tail(log, 25):
            self._p("      " + line)
        if hint:
            self.warn(hint)
        return False


def _elapsed(t0):
    s = int(time.time() - t0)
    return "%d:%02d" % (s // 60, s % 60)


def _read_tail(path, n):
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(max(0, size - n))
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def _progress(log):
    lines = [x for x in _read_tail(log, 4000).replace("\r", "\n").split("\n") if x.strip()]
    detail = lines[-1] if lines else ""
    steps = re.findall(r"^\[(\d+)/(\d+)\]", _read_tail(log, 20000), re.M)
    if steps:
        n, m = int(steps[-1][0]), int(steps[-1][1])
        if m:
            detail = "%d%% (%d/%d steps)  %s" % (n * 100 // m, n, m, detail.split("] ", 1)[-1])
    return detail


def _has_failed_line(log):
    try:
        with open(log, "rb") as f:
            return any(line.startswith(b"FAILED ") for line in f)
    except OSError:
        return False


def _tail(path, n):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return [x.rstrip("\n") for x in f.readlines()[-n:]]
    except OSError:
        return []


def dropped_path(p, windows):
    """The path of a file typed or dragged into the terminal. Terminals quote a
    dropped file ("...", '...') and macOS's escape it instead (My\\ Music/x.UPD)
    with a space after it."""
    p = p.strip()
    for q in ('"', "'"):
        if p.endswith(q):
            p = p[:-1]
        if p.startswith(q):
            p = p[1:]
    if windows:
        from . import host

        return host.native(p) if p else p
    if not os.path.exists(p):
        p = re.sub(r"\\(.)", r"\1", p)
    return p
