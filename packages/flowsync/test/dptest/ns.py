"""Network namespaces as nodes.

The hub is the network namespace the test process runs in (the root of its
own user namespace, see runner.py). Every other node is a network namespace
held open by `unshare -n sleep infinity`; commands run in it via nsenter.
"""
import os
import signal
import subprocess
import time


class CmdError(RuntimeError):
    pass


class Node:
    def __init__(self, name, pid=None, holder=None):
        self.name = name
        self.pid = pid          # None: the hub, run commands directly
        self.holder = holder
        self.spawned = []

    @classmethod
    def create(cls, name):
        holder = subprocess.Popen(["unshare", "-n", "sleep", "infinity"],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
        own = os.readlink("/proc/self/ns/net")
        for _ in range(200):
            try:
                if os.readlink("/proc/%d/ns/net" % holder.pid) != own:
                    return cls(name, holder.pid, holder)
            except OSError:
                pass
            time.sleep(0.01)
        holder.kill()
        raise CmdError("%s: namespace did not come up" % name)

    def argv(self, argv):
        argv = [str(a) for a in argv]
        return argv if self.pid is None else ["nsenter", "-t", str(self.pid), "-n"] + argv

    def run(self, *argv, input=None, check=True, timeout=120):
        """run a command in the node; returns stdout (stripped)"""
        r = subprocess.run(self.argv(argv), input=input, capture_output=True, text=True,
                           timeout=timeout)
        if check and r.returncode:
            raise CmdError("%s: %s: %s" % (self.name, " ".join(map(str, argv)),
                                           (r.stderr or r.stdout).strip()))
        return r.stdout.strip()

    def ok(self, *argv, **kw):
        """run, True if it exited 0"""
        r = subprocess.run(self.argv(argv), capture_output=True, text=True, **kw)
        return r.returncode == 0

    def sh(self, script, check=True):
        return self.run("sh", "-c", script, check=check)

    def nft(self, text):
        return self.run("nft", "-f", "-", input=text)

    def sysctl(self, **kv):
        for k, v in kv.items():
            path = "/proc/sys/" + k.replace(".", "/")
            self.sh("echo %s > %s" % (v, path))

    def read(self, path):
        return self.run("cat", path, check=False)

    def spawn(self, *argv, stdout=None, stderr=None):
        """a background process in the node (killed by close())"""
        p = subprocess.Popen(self.argv(argv), stdin=subprocess.DEVNULL,
                             stdout=stdout or subprocess.DEVNULL,
                             stderr=stderr or subprocess.DEVNULL, start_new_session=True)
        self.spawned.append(p)
        return p

    def close(self):
        for p in self.spawned:
            kill(p)
        self.spawned = []
        if self.holder:
            kill(self.holder)
            self.holder = None


def kill(p, sig=signal.SIGTERM):
    """kill a process and its process group (spawned with a session of its own)"""
    if p.poll() is not None:
        return
    try:
        os.killpg(p.pid, sig)
    except (ProcessLookupError, PermissionError):
        try:
            p.send_signal(sig)
        except ProcessLookupError:
            pass
    try:
        p.wait(2)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            p.kill()
        p.wait()
