import os
import pty
import sys

# Run a command on a pseudo-terminal: flowsync logs to stderr with timestamps
# when stderr is a terminal and to syslog otherwise. Used to capture daemon
# logs in the integration tests. Exits with the command's status (128 + the
# signal if it was killed), so that a crashed daemon is visible.
code = os.waitstatus_to_exitcode(pty.spawn(sys.argv[1:]))
sys.exit(code if code >= 0 else 128 - code)
