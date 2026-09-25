# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared test helpers: paths, running a script, and lifting functions out of
scripts that do their work at import time."""
import ast
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def path(*parts):
    return os.path.join(ROOT, *parts)


def run_script(rel, *args, cwd=None):
    """Run one of the tools as its users do, returning the CompletedProcess."""
    return subprocess.run([sys.executable, path(*rel.split("/"))] + [str(a) for a in args],
                          capture_output=True, text=True, cwd=cwd, timeout=300)


def lift(rel, names, namespace=None):
    """Top-level functions and assignments named in `names`, from a script that
    must not be imported (load_track.py and score_playhead.py run a whole
    driver at import). Only the named nodes are executed, in `namespace`,
    which must supply whatever module globals they use."""
    with open(path(*rel.split("/")), encoding="utf-8") as fh:
        tree = ast.parse(fh.read())
    wanted = set(names)
    body = []
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name in wanted:
            body.append(node)
            wanted.discard(node.name)
        elif isinstance(node, ast.Assign):
            targets = {t.id for t in node.targets if isinstance(t, ast.Name)}
            targets |= {e.id for t in node.targets if isinstance(t, ast.Tuple)
                        for e in t.elts if isinstance(e, ast.Name)}
            if targets & wanted:
                body.append(node)
                wanted -= targets
    if wanted:
        raise LookupError("%s has no top-level %s" % (rel, ", ".join(sorted(wanted))))
    ns = dict(namespace or {})
    exec(compile(ast.Module(body=body, type_ignores=[]), rel, "exec"), ns)
    return ns
