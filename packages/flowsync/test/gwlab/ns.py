"""Network namespaces as nodes. The hub is the namespace the lab process runs
in; every other node is a namespace held open by `unshare -n sleep infinity`."""
import os
import signal
import subprocess
import time


class Node:
    def __init__(self, name, pid=None, holder=None):
        self.name = name
        self.pid = pid              # None: the hub
        self.holder = holder
        self.procs = []

    @classmethod
    def create(cls, name):
        holder = subprocess.Popen(["unshare", "-n", "sleep", "infinity"],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
        own = os.readlink("/proc/self/ns/net")
        for _ in range(500):
            try:
                if os.readlink("/proc/%d/ns/net" % holder.pid) != own:
                    return cls(name, holder.pid, holder)
            except OSError:
                pass
            time.sleep(0.005)
        holder.kill()
        raise RuntimeError("%s: namespace did not come up" % name)

    def argv(self, argv):
        argv = [str(a) for a in argv]
        return argv if self.pid is None else ["nsenter", "-t", str(self.pid), "-n"] + argv

    def run(self, *argv, input=None, check=True):
        r = subprocess.run(self.argv(argv), input=input, capture_output=True, text=True)
        if check and r.returncode:
            raise RuntimeError("%s: %s: %s" % (self.name, " ".join(map(str, argv)),
                                               (r.stderr or r.stdout).strip()))
        return r.stdout.strip()

    def sh(self, script, check=True):
        return self.run("sh", "-c", script, check=check)

    def ip(self, lines):
        """many `ip` commands in one process"""
        self.run("ip", "-batch", "-", input="\n".join(lines) + "\n")

    def nft(self, text):
        self.run("nft", "-f", "-", input=text)

    def sysctl(self, **kv):
        self.sh("; ".join("echo %s > /proc/sys/%s" % (v, k.replace(".", "/"))
                          for k, v in kv.items()))

    def spawn(self, *argv, log=None, preexec=None):
        """a background process in the node, in a session of its own"""
        out = open(log, "a") if log else subprocess.DEVNULL
        p = subprocess.Popen(self.argv(argv), stdin=subprocess.DEVNULL, stdout=out, stderr=out,
                             start_new_session=True, preexec_fn=preexec)
        if log:
            out.close()
        self.procs.append(p)
        return p

    def close(self):
        for p in self.procs:
            kill(p)
        self.procs = []
        if self.holder:
            kill(self.holder)
            self.holder = None


def kill(p, sig=signal.SIGTERM):
    """end a process and its session"""
    if p.poll() is not None:
        return
    try:
        os.killpg(p.pid, sig)
    except (ProcessLookupError, PermissionError):
        p.send_signal(sig)
    try:
        p.wait(3)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            p.kill()
        p.wait()
