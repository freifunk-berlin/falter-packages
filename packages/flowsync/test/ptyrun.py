import pty
import sys

# Run a command on a pseudo-terminal: flowsync logs to stderr with timestamps
# when stderr is a terminal and to syslog otherwise. Used to capture daemon
# logs in the integration tests.
pty.spawn(sys.argv[1:])
