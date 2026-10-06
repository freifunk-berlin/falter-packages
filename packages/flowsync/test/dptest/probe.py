#!/usr/bin/env python3
"""Packet helpers for dptest, run inside a network
namespace. Addresses are IPv6 (IPv4 as ::ffff:a.b.c.d).

  probe.py recv  <addr> <port> <timeout>                     print RECV or NONE
  probe.py send  <src> <sport> <dst> <dport> [size]          one UDP datagram (of size bytes:
                 above the MTU it leaves in fragments)
  probe.py burst <src> <dst> <dport> <count> <first_sport>   one datagram per source port
  probe.py flood <src> <dst> <dport> <count> <first_sport>   like burst, raw and fast: source
                 ports first_sport..65535, then the next source address (last group + 1)
  probe.py xchg  <src> <sport> <dst> <dport> <timeout> [size] one datagram, then wait for a
                 reply on the same socket: print "RECV <bytes>" or NONE
  probe.py echo  <addr> <port> <delay> <count>               UDP echo server, replies after
                 delay seconds, count times
  probe.py tcpsrv <addr> <port> <bytes>                      accept one connection, read bytes,
                 send bytes back, close; print SERVED
  probe.py tcpcli <src> <sport> <dst> <dport> <bytes> <timeout>
                 connect, send bytes, read bytes; print "OK <connect ms> <total ms>" or FAIL
  probe.py tcppush <addr> <port> <gap,gap,...>               accept one connection, send 1 KiB
                 after each gap (seconds); the client only acknowledges
  probe.py tcpread <src> <sport> <dst> <dport> <count> <timeout>
                 connect, read count KiB; print "OK <n>" or "FAIL after <n>: ..."
  probe.py tcp   <src> <sport> <dst> <dport> <flags> <seq> <ack> [payload_len]
  probe.py tcpflood <src> <dst> <fixport> <first> <count> fwd|rev <flags> <rate> <seconds>
                 flags: any of S A P F R, e.g. "SA" for a SYN/ACK
  probe.py icmp6 <src> <dst> <count>                         echo requests, one id each
  probe.py raw   <src> <sport> <dst> <dport> <count>         random garbage datagrams
  probe.py v6udp <src> <sport> <dst> <dport> <hex payload>   one UDP datagram in a hand-built
                 IPv6 header: any source, a v4-mapped one too
  probe.py v6ext <src> <sport> <dst> <dport> <kind>          one UDP datagram behind extension
                 headers: dstopt, hbh, atomic (a fragment header on an unfragmented
                 packet), chain (hop-by-hop, destination options, fragment)
  probe.py v6proto <src> <dst> <proto> <count>               packets of an IP protocol without
                 ports (e.g. 47), 16 bytes of payload each
  probe.py l2udp <dev> <src> <sport> <dst> <dport>           one UDP datagram in an Ethernet
                 frame addressed to another host's MAC, sent out of dev

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


def send(src, sport, dst, dport, size="1"):
    s = sock(socket.AF_INET6, socket.SOCK_DGRAM)
    s.bind((src, int(sport)))
    s.sendto(b"x" * int(size), (dst, int(dport)))
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


def xchg(src, sport, dst, dport, timeout, size="1"):
    s = sock(socket.AF_INET6, socket.SOCK_DGRAM)
    s.bind((src, int(sport)))
    s.sendto(b"x" * int(size), (dst, int(dport)))
    s.settimeout(float(timeout))
    try:
        d, _ = s.recvfrom(65535)
        print("RECV %d" % len(d))
    except socket.timeout:
        print("NONE")


def echo(addr, port, delay, count):
    s = sock(socket.AF_INET6, socket.SOCK_DGRAM)
    s.bind((addr, int(port)))
    for _ in range(int(count)):
        data, peer = s.recvfrom(65535)
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


def tcpmss(src, sport, dst, dport, nbytes, timeout):
    # like tcpcli, but print the MSS the connection ended up with: "OK <mss>"
    nbytes = int(nbytes)
    s = sock(socket.AF_INET6, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((src, int(sport)))
    s.settimeout(float(timeout))
    try:
        s.connect((dst, int(dport)))
        mss = s.getsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG)
        s.sendall(b"x" * nbytes)
        got = 0
        while got < nbytes:
            d = s.recv(65536)
            if not d:
                break
            got += len(d)
        s.close()
        print("OK %d" % mss if got == nbytes else "FAIL short read %d" % got)
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


def tcppush(addr, port, gaps, talk="0"):
    # accept one connection and send 1 KiB after each of the comma-separated
    # gaps (seconds): the server speaks, the client only acknowledges; with
    # talk=1 the client sends 1 KiB first and gets it echoed
    ls = sock(socket.AF_INET6, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((addr, int(port)))
    ls.listen(1)
    ls.settimeout(30)
    try:
        c, _ = ls.accept()
        if talk == "1":
            c.settimeout(10)
            got = b""
            while len(got) < 1024:
                got += c.recv(65536)
            c.sendall(got)
        for g in gaps.split(","):
            time.sleep(float(g))
            c.sendall(b"y" * 1024)
        time.sleep(1)
        c.close()
    except OSError:
        pass


def tcpread(src, sport, dst, dport, count, timeout, talk="0"):
    # connect and read count KiB pushed by the server: "OK <n>" or "FAIL after <n>";
    # with talk=1 send 1 KiB first (its echo counts as the first KiB)
    s = sock(socket.AF_INET6, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((src, int(sport)))
    s.settimeout(float(timeout))
    got = 0
    try:
        s.connect((dst, int(dport)))
        if talk == "1":
            s.sendall(b"x" * 1024)
            count = int(count) + 1
        while got < int(count) * 1024:
            d = s.recv(65536)
            if not d:
                raise OSError("closed")
            got += len(d)
        s.close()
        print("OK %d" % (got // 1024))
    except OSError as e:
        print("FAIL after %d: %s" % (got // 1024, e))


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


def _csum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return ~s & 0xffff


def v6udp(src, sport, dst, dport, payload):
    # one UDP datagram in a hand-built IPv6 header, so the source may be
    # anything, a v4-mapped address too (no socket can bind to one)
    data = bytes.fromhex(payload)
    s6, d6 = socket.inet_pton(socket.AF_INET6, src), socket.inet_pton(socket.AF_INET6, dst)
    ulen = 8 + len(data)
    udp = struct.pack("!HHHH", int(sport), int(dport), ulen, 0) + data
    c = _csum(s6 + d6 + struct.pack("!I3xB", ulen, 17) + udp) or 0xffff
    udp = udp[:6] + struct.pack("!H", c) + udp[8:]
    ip6 = struct.pack("!IHBB", 6 << 28, ulen, 17, 64) + s6 + d6
    s = sock(socket.AF_INET6, socket.SOCK_RAW, socket.IPPROTO_RAW)
    s.sendto(ip6 + udp, (dst, 0))
    s.close()


def _v6send(src, dst, nexthdr, body):
    s6, d6 = socket.inet_pton(socket.AF_INET6, src), socket.inet_pton(socket.AF_INET6, dst)
    ip6 = struct.pack("!IHBB", 6 << 28, len(body), nexthdr, 64) + s6 + d6
    s = sock(socket.AF_INET6, socket.SOCK_RAW, socket.IPPROTO_RAW)
    s.sendto(ip6 + body, (dst, 0))
    s.close()


def v6ext(src, sport, dst, dport, kind):
    # one UDP datagram behind extension headers, each 8 bytes long
    s6, d6 = socket.inet_pton(socket.AF_INET6, src), socket.inet_pton(socket.AF_INET6, dst)
    data = b"x" * 8
    ulen = 8 + len(data)
    udp = struct.pack("!HHHH", int(sport), int(dport), ulen, 0) + data
    c = _csum(s6 + d6 + struct.pack("!I3xB", ulen, 17) + udp) or 0xffff
    udp = udp[:6] + struct.pack("!H", c) + udp[8:]
    pad = bytes([1, 4, 0, 0, 0, 0])             # a PadN option filling the header

    def opts(nxt):
        return bytes([nxt, 0]) + pad

    def frag(nxt):
        return struct.pack("!BBHI", nxt, 0, 0, 0x1234)     # offset 0, no more fragments

    if kind == "dstopt":
        _v6send(src, dst, 60, opts(17) + udp)
    elif kind == "hbh":
        _v6send(src, dst, 0, opts(17) + udp)
    elif kind == "atomic":
        _v6send(src, dst, 44, frag(17) + udp)
    elif kind == "chain":
        _v6send(src, dst, 0, opts(60) + opts(44) + frag(17) + udp)
    else:
        raise SystemExit("unknown kind %s" % kind)


def l2udp(dev, src, sport, dst, dport):
    # what a NIC in promiscuous mode picks up for another host on the link:
    # the stack drops it, a tc program sees it first
    s6, d6 = socket.inet_pton(socket.AF_INET6, src), socket.inet_pton(socket.AF_INET6, dst)
    data = b"x" * 8
    ulen = 8 + len(data)
    udp = struct.pack("!HHHH", int(sport), int(dport), ulen, 0) + data
    c = _csum(s6 + d6 + struct.pack("!I3xB", ulen, 17) + udp) or 0xffff
    udp = udp[:6] + struct.pack("!H", c) + udp[8:]
    ip6 = struct.pack("!IHBB", 6 << 28, ulen, 17, 64) + s6 + d6
    frame = bytes.fromhex("02deadbeef00") + bytes.fromhex("02aabbccdd01") + b"\x86\xdd"
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    s.bind((dev, 0))
    s.send(frame + ip6 + udp)
    s.close()


def v6proto(src, dst, proto, count):
    for _ in range(int(count)):
        _v6send(src, dst, int(proto), b"p" * 16)


def tcpflood(src, dst, fixport, first, count, direction, flags, rate, seconds):
    """raw TCP segments for count flows, cycling over them at rate per second for
    seconds (0: one pass). fwd: sport first+i -> dport fixport; rev: sport
    fixport -> dport first+i. Prints SENT n."""
    first, count, fixport = int(first), int(count), int(fixport)
    rate, seconds = float(rate), float(seconds)
    bits = 0
    for ch, bit in (("F", 1), ("S", 2), ("R", 4), ("P", 8), ("A", 16)):
        if ch in flags:
            bits |= bit
    s = sock(socket.AF_INET6, socket.SOCK_RAW, socket.IPPROTO_TCP)
    s.setsockopt(socket.IPPROTO_IPV6, IPV6_FREEBIND, 1)
    s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_CHECKSUM, 16)
    s.bind((src, 0))
    hdrs = []
    for i in range(count):
        sp, dp = (first + i, fixport) if direction == "fwd" else (fixport, first + i)
        hdrs.append(struct.pack("!HHIIBBHHH", sp, dp, 100000 + i, 200000 + i, 5 << 4, bits,
                                65535, 0, 0))
    n, t0 = 0, time.monotonic()
    end = t0 + seconds
    while True:
        s.sendto(hdrs[n % count], (dst, 0))
        n += 1
        if seconds == 0 and n >= count:
            break
        if n % 64 == 0:
            now = time.monotonic()
            if seconds and now >= end:
                break
            ahead = n / rate - (now - t0)
            if ahead > 0:
                time.sleep(ahead)
    print("SENT %d %.3f" % (n, time.monotonic() - t0))
    sys.stdout.flush()


if __name__ == "__main__":
    argv = sys.argv[1:]
    if argv[0] == "--mark":
        MARK = int(argv[1])
        argv = argv[2:]
    cmd, args = argv[0], argv[1:]
    {"recv": recv, "send": send, "burst": burst, "flood": flood, "xchg": xchg, "echo": echo,
     "tcpsrv": tcpsrv, "tcpcli": tcpcli, "tcpmss": tcpmss, "tcpecho": tcpecho, "tcptalk": tcptalk,
     "tcppush": tcppush, "tcpread": tcpread, "spoof": spoof, "tcp": tcp, "icmp6": icmp6, "raw": raw,
     "v6udp": v6udp, "v6ext": v6ext, "v6proto": v6proto, "l2udp": l2udp,
     "tcpflood": tcpflood}[cmd](*args)
