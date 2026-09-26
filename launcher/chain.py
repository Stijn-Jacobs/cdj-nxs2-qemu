# SPDX-License-Identifier: GPL-2.0-or-later
"""What the scripts under scripts/run/ share now that they run in Python.

Each script there (live.sh, rig.sh, play_real_dsp.sh, instrumented_batch.sh,
boot_decks.sh, boot_deck.sh, warm_jit.sh) is a shim over a module of this
package with the same name, and they still start each other through those
shims: callers find and stop a run by its command line
(pgrep -f 'run/rig.sh <tag> '), so the process tree looks as it did. Without
bash (the packaged program) the modules start each other directly.
"""

import os
import subprocess
import sys
import time

from . import host
from .layout import Layout

SCRIPTS = ("live", "live_linked", "rig", "play_real_dsp", "instrumented_batch", "boot_decks",
           "boot_deck", "warm_jit")


def nonempty(env, name, default):
    """${name:-default}"""
    v = env.get(name, "")
    return v if v != "" else default


def ifset(env, name, default):
    """${name-default}"""
    return env[name] if name in env else default


def export_default(env, name, default):
    env[name] = nonempty(env, name, default)


def unset(env, *names):
    for n in names:
        env.pop(n, None)


def say(*a):
    print(*a, flush=True)


def err(*a):
    print(*a, file=sys.stderr, flush=True)


def script_argv(name, args):
    """How one script starts another: `bash scripts/run/<name>.sh args` in a
    checkout, else this launcher's `run <name> args`."""
    lay = Layout()
    bash = None if lay.packaged else host.find_bash()
    if bash:
        return [bash, host.posix(os.path.join(lay.run, name + ".sh"))] + [str(a) for a in args]
    return launcher_argv() + ["run", name] + [str(a) for a in args]


def launcher_argv():
    return [sys.executable, "-m", "launcher"]


def launcher_env(env):
    """`env` with this package importable, for a child run through launcher_argv()."""
    env = dict(env)
    emu = Layout().emu
    env["PYTHONPATH"] = emu + (os.pathsep + env["PYTHONPATH"] if env.get("PYTHONPATH") else "")
    return env


def start_script(name, args, env=None, stdout=None):
    env = launcher_env(os.environ if env is None else env)
    return subprocess.Popen(script_argv(name, args), env=env, stdout=stdout,
                            stderr=subprocess.STDOUT if stdout else None)


def run_script(name, args, env=None, stdout=None):
    return start_script(name, args, env, stdout).wait()


def run_main(name, args):
    """Run one script's module. Ctrl-C and TERM become a stop request, which
    every process of the run polls for, so each deck is shut down in order and
    nothing dies halfway through a teardown."""
    import importlib
    import signal

    lay = Layout()
    if lay.packaged:
        for k, v in lay.runtime_env().items():
            os.environ.setdefault(k, v)
    own = not stop_file()
    if own:
        os.environ["LAUNCHER_STOP_FILE"] = "%s/cdj-stop-%d" % (host.rig_tmp(), os.getpid())
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: request_stop())
    try:
        if name in ("live", "live_linked"):
            from . import live
            return live.main(args, linked=name == "live_linked")
        return importlib.import_module("launcher." + name).main(args)
    finally:
        if own:
            remove(stop_file())


def stop_file():
    return os.environ.get("LAUNCHER_STOP_FILE", "")


def request_stop():
    f = stop_file()
    if f:
        open(f, "a").close()


def stop_requested(parent=None):
    """A stop was asked for (the shim that started this was sent INT or TERM,
    or `start.sh stop` ran), or the process that started this is gone."""
    f = stop_file()
    if f and os.path.exists(f):
        return True
    return parent is not None and not host.is_windows() and os.getppid() != parent


def sleep(seconds, until=None):
    """time.sleep that ends early on a stop request or when until() is true."""
    parent = None if host.is_windows() else os.getppid()
    end = time.time() + seconds
    while time.time() < end and not stop_requested(parent) and not (until and until()):
        time.sleep(min(0.5, max(0.0, end - time.time())))


def remove(path):
    try:
        os.remove(path)
    except OSError:
        pass


def pgrep_count(pattern):
    """`pgrep <pattern> | wc -l`, as the scripts print it (0 without pgrep)."""
    try:
        out = subprocess.run(["pgrep", pattern], capture_output=True, text=True).stdout
    except OSError:
        return 0
    return len(out.split())


def loadavg():
    """The 1/5/15-minute load averages, as the scripts print them."""
    try:
        with open("/proc/loadavg") as f:
            return " ".join(f.read().split()[:3])
    except OSError:
        pass
    for argv in (["sysctl", "-n", "vm.loadavg"], ["cut", "-d ", "-f1-3", "/proc/loadavg"]):
        try:
            out = subprocess.run(argv, capture_output=True, text=True).stdout
        except OSError:
            continue
        if out.strip():
            return " ".join(out.replace("{", "").replace("}", "").split())
    return ""


def bash_report(name, args, env):
    """Run one of the run reports (report_*.sh), which stay in bash: the
    packaged program has no bash and no use for them. Its exit status, 0 when
    skipped."""
    bash = None if Layout().packaged else host.find_bash()
    if not bash:
        return 0
    sys.stdout.flush()
    return subprocess.run([bash, host.posix(os.path.join(Layout().run, name))] + [str(a) for a in args],
                          env=env).returncode
