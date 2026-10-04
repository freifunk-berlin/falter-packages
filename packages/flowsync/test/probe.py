#!/usr/bin/env python3
"""Packet helpers for the flowsync integration tests, run inside a network
namespace. Addresses are IPv6 (IPv4 as ::ffff:a.b.c.d).

  probe.py recv  <addr> <port> <timeout>                     print RECV or NONE
  probe.py send  <src> <sport> <dst> <dport>                 one UDP datagram
  probe.py burst <src> <dst> <dport> <count> <first_sport>   one datagram per source port
  probe.py flood <src> <dst> <dport> <count> <first_sport>   like burst, raw and fast: source
                 ports first_sport..65535, then the next source address (last group + 1)
  probe.py xchg  <src> <sport> <dst> <dport> <timeout>       one datagram, then wait for a
                 reply on the same socket: print RECV or NONE
  probe.py echo  <addr> <port> <delay> <count>               UDP echo server, replies after
                 delay seconds, count times
  probe.py tcpsrv <addr> <port> <bytes>                      accept one connection, read bytes,
                 send bytes back, close; print SERVED
  probe.py tcpcli <src> <sport> <dst> <dport> <bytes> <timeout>
                 connect, send bytes, read bytes; print "OK <connect ms> <total ms>" or FAIL
  probe.py tcp   <src> <sport> <dst> <dport> <flags> <seq> <ack> [payload_len]
                 flags: any of S A P F R, e.g. "SA" for a SYN/ACK
  probe.py icmp6 <src> <dst> <count>                         echo requests, one id each
  probe.py raw   <src> <sport> <dst> <dport> <count>         random garbage datagrams

  probe.py --mark N <command> ...                            the same with SO_MARK N on every
                 socket, to pick a policy route (the test topology routes
                 mark i+1 via gateway i)

The TCP segments of "tcp" are raw (no socket state), so handshakes can be
played in any order and from either side; the kernel fills in the checksum.
"tcpsrv"/"tcpcli" are real TCP stacks.
"""
import os
import random
import socket
import struct
import sys
import time

IPV6_FREEBIND = 78
MARK = 0


def sock(family, kind, proto=0):
    """a socket carrying the --mark given on the command line"""
    s = socket.socket(family, kind, proto)
    if MARK:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_MARK, MARK)
    return s


def recv(addr, port, timeout):
    s = sock(socket.AF_INET6, socket.SOCK_DGRAM)
    s.bind((addr, int(port)))
    s.settimeout(float(timeout))
    try:
        s.recvfrom(64)
        print("RECV")
    except socket.timeout:
        print("NONE")


def send(src, sport, dst, dport):
    s = sock(socket.AF_INET6, socket.SOCK_DGRAM)
    s.bind((src, int(sport)))
    s.sendto(b"x", (dst, int(dport)))
    s.close()


def burst(src, dst, dport, count, first):
    for i in range(int(count)):
        send(src, int(first) + i, dst, dport)


def flood(src, dst, dport, count, first):
    # raw UDP: no bind per flow, the source need not be a local address
    dport, first, count = int(dport), int(first), int(count)
    span = 65536 - first
    addr = bytearray(socket.inet_pton(socket.AF_INET6, src))
    s = None
    for i in range(count):
        if i % span == 0:
            if i:
                addr[14:16] = struct.pack("!H", struct.unpack("!H", addr[14:16])[0] + 1)
            s = sock(socket.AF_INET6, socket.SOCK_RAW, socket.IPPROTO_UDP)
            s.setsockopt(socket.IPPROTO_IPV6, IPV6_FREEBIND, 1)
            s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_CHECKSUM, 6)
            s.bind((socket.inet_ntop(socket.AF_INET6, bytes(addr)), 0))
        sport = first + i % span
        s.sendto(struct.pack("!HHHH", sport, dport, 9, 0) + b"x", (dst, 0))


def xchg(src, sport, dst, dport, timeout):
    s = sock(socket.AF_INET6, socket.SOCK_DGRAM)
    s.bind((src, int(sport)))
    s.sendto(b"x", (dst, int(dport)))
    s.settimeout(float(timeout))
    try:
        s.recvfrom(64)
        print("RECV")
    except socket.timeout:
        print("NONE")


def echo(addr, port, delay, count):
    s = sock(socket.AF_INET6, socket.SOCK_DGRAM)
    s.bind((addr, int(port)))
    for _ in range(int(count)):
        data, peer = s.recvfrom(2048)
        time.sleep(float(delay))
        s.sendto(data, peer)


def tcpsrv(addr, port, nbytes):
    nbytes = int(nbytes)
    ls = sock(socket.AF_INET6, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((addr, int(port)))
    ls.listen(1)
    ls.settimeout(60)
    c, _ = ls.accept()
    c.settimeout(30)
    got = 0
    while got < nbytes:
        d = c.recv(65536)
        if not d:
            break
        got += len(d)
    c.sendall(b"y" * nbytes)
    c.close()
    print("SERVED %d" % got)


def tcpcli(src, sport, dst, dport, nbytes, timeout):
    nbytes = int(nbytes)
    s = sock(socket.AF_INET6, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((src, int(sport)))
    s.settimeout(float(timeout))
    t0 = time.monotonic()
    try:
        s.connect((dst, int(dport)))
        t1 = time.monotonic()
        s.sendall(b"x" * nbytes)
        got = 0
        while got < nbytes:
            d = s.recv(65536)
            if not d:
                break
            got += len(d)
        s.close()
        if got != nbytes:
            print("FAIL short read %d" % got)
            return
        print("OK %d %d" % ((t1 - t0) * 1000, (time.monotonic() - t0) * 1000))
    except OSError as e:
        print("FAIL %s" % e)


def tcpecho(addr, port, seconds):
    # accept one connection and echo whatever arrives until it closes or time is up
    ls = sock(socket.AF_INET6, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((addr, int(port)))
    ls.listen(1)
    ls.settimeout(float(seconds))
    end = time.monotonic() + float(seconds)
    try:
        c, _ = ls.accept()
        c.settimeout(1)
        while time.monotonic() < end:
            try:
                d = c.recv(65536)
            except socket.timeout:
                continue
            if not d:
                break
            c.sendall(d)
        c.close()
    except OSError:
        pass


def tcptalk(src, sport, dst, dport, count, interval, timeout):
    # one long-lived connection: count times send 1 KiB, read the echo, sleep
    s = sock(socket.AF_INET6, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((src, int(sport)))
    s.settimeout(float(timeout))
    done = 0
    try:
        s.connect((dst, int(dport)))
        for _ in range(int(count)):
            s.sendall(b"x" * 1024)
            got = 0
            while got < 1024:
                d = s.recv(65536)
                if not d:
                    raise OSError("closed")
                got += len(d)
            done += 1
            time.sleep(float(interval))
        s.close()
        print("OK %d" % done)
    except OSError as e:
        print("FAIL after %d: %s" % (done, e))


def spoof(src, dst, port, rate, seconds):
    # flowsync wire format v1, 34 policy-conforming UDP records per datagram
    # (clients in 2001:db8:100::/44, server outside the mesh), from src
    s = sock(socket.AF_INET6, socket.SOCK_DGRAM)
    s.bind((src, 0))
    rate, end = float(rate), time.monotonic() + float(seconds)
    server = socket.inet_pton(socket.AF_INET6, "2a00:1450:4001:900::1")
    n, t0 = 0, time.monotonic()
    while time.monotonic() < end:
        recs = []
        for _ in range(34):
            client = bytes.fromhex("20010db801") + bytes([random.randint(0, 15)]) + os.urandom(10)
            recs.append(struct.pack("!BBHHH", 17, 0, random.randint(1024, 65535), 443, 0)
                        + client + server)
        s.sendto(struct.pack("!BBH", 1, 34, 0) + b"".join(recs), (dst, int(port)))
        n += 1
        ahead = n / rate - (time.monotonic() - t0)
        if ahead > 0:
            time.sleep(ahead)
    print("SENT %d" % n)


def tcp(src, sport, dst, dport, flags, seq, ack, plen=0):
    bits = 0
    for ch, bit in (("F", 1), ("S", 2), ("R", 4), ("P", 8), ("A", 16)):
        if ch in flags:
            bits |= bit
    hdr = struct.pack("!HHIIBBHHH", int(sport), int(dport), int(seq), int(ack),
                      5 << 4, bits, 65535, 0, 0)
    s = sock(socket.AF_INET6, socket.SOCK_RAW, socket.IPPROTO_TCP)
    # checksum computed by the kernel, it lives at offset 16 of the TCP header
    s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_CHECKSUM, 16)
    s.bind((src, 0))
    s.sendto(hdr + b"x" * int(plen), (dst, 0))
    s.close()


def icmp6(src, dst, count):
    # the kernel always checksums ICMPv6 raw sockets
    s = sock(socket.AF_INET6, socket.SOCK_RAW, socket.IPPROTO_ICMPV6)
    s.bind((src, 0))
    for i in range(int(count)):
        s.sendto(struct.pack("!BBHHH", 128, 0, 0, 1000 + i, 1) + b"x" * 8, (dst, 0))
    s.close()


def raw(src, sport, dst, dport, count):
    s = sock(socket.AF_INET6, socket.SOCK_DGRAM)
    s.bind((src, int(sport)))
    for _ in range(int(count)):
        s.sendto(os.urandom(random.randint(0, 1500)), (dst, int(dport)))
    s.close()


if __name__ == "__main__":
    argv = sys.argv[1:]
    if argv[0] == "--mark":
        MARK = int(argv[1])
        argv = argv[2:]
    cmd, args = argv[0], argv[1:]
    {"recv": recv, "send": send, "burst": burst, "flood": flood, "xchg": xchg, "echo": echo,
     "tcpsrv": tcpsrv, "tcpcli": tcpcli, "tcpecho": tcpecho, "tcptalk": tcptalk, "spoof": spoof, "tcp": tcp, "icmp6": icmp6, "raw": raw}[cmd](*args)
