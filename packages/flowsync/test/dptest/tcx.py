"""The tcx hooks of a device, through the bpf() system call alone: what a
scenario does to the daemon's programs from outside (bpftool may not be in
the VM). Run inside the gateway's namespace.

  python3 tcx.py list DEV ingress|egress              id and name of every program
  python3 tcx.py detach DEV ingress|egress NAME...    detach the programs so named
  python3 tcx.py fill DEV ingress|egress              fill the hook up with dummies

The dummies (name dptest_dummy, "return TC_ACT_OK") make the hook refuse the
daemon's program (ERANGE: a hook holds 63 programs); detach them by name.
"""
import ctypes
import os
import platform
import socket
import struct
import sys

SYS_BPF = {"x86_64": 321, "aarch64": 280, "riscv64": 280}[platform.machine()]
BPF_PROG_LOAD, BPF_PROG_ATTACH, BPF_PROG_DETACH = 5, 8, 9
BPF_PROG_GET_FD_BY_ID, BPF_OBJ_GET_INFO_BY_FD, BPF_PROG_QUERY = 13, 15, 16
BPF_PROG_TYPE_SCHED_CLS = 3
TCX = {"ingress": 46, "egress": 47}          # BPF_TCX_INGRESS, BPF_TCX_EGRESS
MPROG_MAX = 64
DUMMY = b"dptest_dummy"

libc = ctypes.CDLL(None, use_errno=True)
libc.syscall.restype = ctypes.c_long


def bpf(cmd, attr):
    """bpf(cmd, attr, size); the result, or raises OSError"""
    buf = ctypes.create_string_buffer(attr, len(attr))
    r = libc.syscall(SYS_BPF, cmd, buf, len(attr))
    if r < 0:
        e = ctypes.get_errno()
        raise OSError(e, os.strerror(e))
    return r, buf.raw


def ifindex(dev):
    """in this network namespace (sysfs would show the one it was mounted in)"""
    return socket.if_nametoindex(dev)


def query(idx, hook):
    """the ids of the programs on the hook, in order"""
    ids = (ctypes.c_uint32 * MPROG_MAX)()
    attr = struct.pack("=IIIIQIIQQQQ", idx, TCX[hook], 0, 0, ctypes.addressof(ids),
                       MPROG_MAX, 0, 0, 0, 0, 0)
    try:
        _, out = bpf(BPF_PROG_QUERY, attr)
    except OSError as e:
        if e.errno == 2:                     # no hook yet: nothing on it
            return []
        raise
    n = struct.unpack_from("=I", out, 24)[0]
    return list(ids[:min(n, MPROG_MAX)])


def fd_by_id(pid):
    return bpf(BPF_PROG_GET_FD_BY_ID, struct.pack("=IIIi", pid, 0, 0, 0))[0]


def name_of(fd):
    info = ctypes.create_string_buffer(80)   # up to and including bpf_prog_info.name
    bpf(BPF_OBJ_GET_INFO_BY_FD, struct.pack("=IIQ", fd, 80, ctypes.addressof(info)))
    return info.raw[64:80].split(b"\0", 1)[0].decode()


def progs(idx, hook):
    """(id, name, fd) of every program on the hook; the caller closes the fds"""
    out = []
    for pid in query(idx, hook):
        fd = fd_by_id(pid)
        out.append((pid, name_of(fd), fd))
    return out


def load_dummy():
    insns = bytes([0xb7, 0, 0, 0, 0, 0, 0, 0,            # r0 = 0 (TC_ACT_OK)
                   0x95, 0, 0, 0, 0, 0, 0, 0])           # exit
    ins = ctypes.create_string_buffer(insns, len(insns))
    lic = ctypes.create_string_buffer(b"GPL")
    attr = struct.pack("=IIQQIIQII16s", BPF_PROG_TYPE_SCHED_CLS, 2, ctypes.addressof(ins),
                       ctypes.addressof(lic), 0, 0, 0, 0, 0, DUMMY.ljust(16, b"\0"))
    return bpf(BPF_PROG_LOAD, attr)[0]


def attach(fd, idx, hook):
    bpf(BPF_PROG_ATTACH, struct.pack("=IIIIIIQ", idx, fd, TCX[hook], 0, 0, 0, 0))


def detach(fd, idx, hook):
    bpf(BPF_PROG_DETACH, struct.pack("=IIIIIIQ", idx, fd, TCX[hook], 0, 0, 0, 0))


def main(argv):
    cmd, dev, hook = argv[0], argv[1], argv[2]
    idx = ifindex(dev)
    if cmd == "list":
        for pid, name, fd in progs(idx, hook):
            print(pid, name)
            os.close(fd)
    elif cmd == "detach":
        names = argv[3:]
        n = 0
        for pid, name, fd in progs(idx, hook):
            if name in names:
                detach(fd, idx, hook)
                n += 1
            os.close(fd)
        print(n)
    elif cmd == "fill":
        n = len(query(idx, hook))
        while True:                         # the kernel says when it is full (63: one
            try:                            # slot of BPF_MPROG_MAX is the qdisc's)
                attach(load_dummy(), idx, hook)   # the fd may close: the attachment holds it
            except OSError as e:
                if e.errno == 34:           # ERANGE
                    break
                raise
            n += 1
        print(n)
    else:
        sys.exit("tcx.py: %s?" % cmd)


if __name__ == "__main__":
    main(sys.argv[1:])
