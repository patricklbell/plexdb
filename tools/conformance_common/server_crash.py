# Server-crash attribution shared by the CQL and RESP conformance conftests.
#
# Server output always goes to a log file (the --log-dir one, or a temp dir). Each test
# records the log offset at setup; if a failed test leaves the server dead, the log slice
# written during that test (the assert message / fatal-signal backtrace) is attached to the
# test's report and listed in the end-of-session summary.

import os
import re
import tempfile

import pytest

_LOG_OFFSET = pytest.StashKey[int]()
_CRASHES = pytest.StashKey[list]()
_MANAGER = pytest.StashKey[object]()

_MAX_EXCERPT_BYTES = 16 * 1024
_HEADLINE = re.compile(r"Assert failed.*|Fatal signal.*")


class ServerLog:
    def __init__(self, log_dir):
        self.dir = log_dir or tempfile.mkdtemp(prefix="plexdb_server_log_")
        os.makedirs(self.dir, exist_ok=True)
        self.path = os.path.join(self.dir, "server.log")
        open(self.path, "wb").close()

    def open(self):
        return open(self.path, "ab", buffering=0)

    def mark(self):
        try:
            return os.path.getsize(self.path)
        except OSError:
            return 0

    def read_since(self, offset):
        try:
            with open(self.path, "rb") as f:
                f.seek(offset)
                data = f.read()
        except OSError:
            return ""
        if len(data) > _MAX_EXCERPT_BYTES:
            data = b"...\n" + data[-_MAX_EXCERPT_BYTES:]
        return data.decode("utf-8", "replace")


def register(config, manager):
    """manager must expose `.log` (ServerLog) and `.exited(timeout)` -> returncode or None."""
    config.stash[_MANAGER] = manager
    config.stash[_CRASHES] = []


def on_setup(item):
    # The session-scoped manager is created during the first test's fixture setup, after this
    # hook; the log is truncated per session, so offset 0 is that test's start.
    manager = item.config.stash.get(_MANAGER, None)
    item.stash[_LOG_OFFSET] = manager.log.mark() if manager is not None else 0


def on_report(item, report):
    manager = item.config.stash.get(_MANAGER, None)
    if manager is None or not report.failed or _LOG_OFFSET not in item.stash:
        return
    returncode = manager.exited(timeout=0.1)
    if returncode is None:
        return
    output = manager.log.read_since(item.stash[_LOG_OFFSET])
    match = _HEADLINE.search(output)
    headline = match.group(0) if match else f"server exited with status {returncode} (no assert/signal output)"
    report.sections.append(("server crash", f"exit status {returncode}\n{output}"))
    item.config.stash[_CRASHES].append((item.nodeid, headline))
    # Attribute once: later phases of the same test must not re-report the same crash.
    del item.stash[_LOG_OFFSET]


def terminal_summary(terminalreporter, config):
    crashes = config.stash.get(_CRASHES, [])
    manager = config.stash.get(_MANAGER, None)
    if not crashes:
        return
    terminalreporter.section(f"server crashes ({len(crashes)})")
    for nodeid, headline in crashes:
        terminalreporter.write_line(f"{nodeid}\n    {headline}")
    if manager is not None:
        terminalreporter.write_line(f"full server log: {manager.log.path}")
