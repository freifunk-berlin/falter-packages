#!/usr/bin/env python3
"""The traffic agent: one process per endpoint namespace, one thread per flow.

  agent.py client|server <job.json> <out.jsonl>

job.json: {"t0": <CLOCK_MONOTONIC second all flows count from>, "grace": <seconds>,
           "flows": [{"id", "c", "s", "cport", "sport", "start", "p": {"kind", ...}}, ...]}
out.jsonl: one line per flow with what this end saw.

Traffic kinds (p["kind"]):
  udp_rr      requests, every: the client asks, the server echoes
  udp_stream  up_pps, down_pps, seconds: both ends send numbered packets; the
              server starts with the client's first packet
  tcp_short   connections, every, request, response: a new connection (new
              client port) each time
  tcp_talk    seconds, every: one connection, 1 KiB each way per exchange
  udp_ladder  delays_ms: a new flow per delay, the server answers that much later
Measurements, run one at a time (counted on the interface, not by a reader):
  udp_flood     dir, seconds, size: datagrams as fast as one core sends them
  tcp_bulk      seconds: the server sends at full speed
  udp_newflows  rates, step_seconds, reply_delay: one datagram per new source
                port at each rate; the server answers each after reply_delay
The agent reports what it saw and when; expect.py judges it.
"""
import json
import select
import socket
import struct
import sys
import threading
import time

PAD = b"x" * 60
mono = time.monotonic


def sleep_until(t):
    d = t - mono()
    if d > 0:
        time.sleep(d)


def sock(kind, addr, port):
    s = socket.socket(socket.AF_INET6, kind)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((addr, port))
    return s


def missing(got, pps, seconds):
    """which of the numbered packets 0..n-1 did not arrive (packet k was sent
    k / pps seconds into the stream)"""
    return [k for k in range(int(pps * seconds)) if k not in got]


def stream(s, peer, pps, seconds, deadline):
    """send numbered packets at pps for `seconds` and collect the peer's until
    the deadline. peer None: learn it from the first packet and start then."""
    got, first, last, gap, refused = set(), None, None, 0.0, 0
    n, sent, t_start = int(pps * seconds), 0, None if peer is None else mono()
    while mono() < deadline:
        wait = deadline - mono()
        if t_start is not None and sent < n:
            due = t_start + sent / pps
            if mono() >= due:
                try:
                    s.sendto(struct.pack("!I", sent) + PAD, peer)
                except OSError:
                    refused += 1
                sent += 1
                continue
            wait = min(wait, due - mono())
        if not select.select([s], [], [], max(0, wait))[0]:
            continue
        try:
            d, addr = s.recvfrom(2048)
        except OSError:             # an ICMP error for something we sent
            refused += 1
            continue
        now = mono()
        got.add(struct.unpack("!I", d[:4])[0])
        if first is None:
            first = now
        else:
            gap = max(gap, now - last)
        last = now
        if t_start is None:
            peer, t_start = addr, now
    return got, sent, gap, refused


# ---------------------------------------------------------------- client
def c_udp_rr(f, p, t0, grace):
    s = sock(socket.SOCK_DGRAM, f["c"], f["cport"])
    s.connect((f["s"], f["sport"]))
    sleep_until(t0)
    answered, rtt, refused = [], [], 0
    for k in range(p["requests"]):
        t = mono()
        try:
            s.send(struct.pack("!I", k) + PAD)
        except OSError:
            refused += 1
        ok, end = False, t + p.get("wait", 0.8)
        while not ok and mono() < end:
            if not select.select([s], [], [], max(0, end - mono()))[0]:
                break
            try:
                ok = struct.unpack("!I", s.recv(2048)[:4])[0] == k
            except OSError:
                refused += 1
        if ok:
            rtt.append((mono() - t) * 1000)
        answered.append(ok)
        sleep_until(t + p["every"])
    return dict(answered=answered, rtt_min_ms=min(rtt) if rtt else None, refused=refused)


def c_udp_stream(f, p, t0, grace):
    s = sock(socket.SOCK_DGRAM, f["c"], f["cport"])
    sleep_until(t0)
    got, sent, gap, refused = stream(s, (f["s"], f["sport"]), p["up_pps"], p["seconds"],
                                     t0 + p["seconds"] + 1.0)
    return dict(tx=sent, rx=len(got), lost=missing(got, p["down_pps"], p["seconds"]),
                outage_ms=round(gap * 1000, 1), refused=refused)


def retrans(s):
    """tcpi_total_retrans: segments this end sent again, SYNs included"""
    try:
        return struct.unpack_from("I", s.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 232), 100)[0]
    except (OSError, struct.error):
        return None


def c_tcp_short(f, p, t0, grace):
    sleep_until(t0)
    conns = []
    for k in range(p["connections"]):
        t = mono()
        s = sock(socket.SOCK_STREAM, f["c"], f["cport"] + k)
        s.settimeout(p.get("timeout", 6))
        r = dict(ok=False, connect_ms=None)
        try:
            s.connect((f["s"], f["sport"]))
            r["connect_ms"] = round((mono() - t) * 1000, 1)
            s.sendall(b"q" * p["request"])
            n = 0
            while n < p["response"]:
                d = s.recv(65536)
                if not d:
                    raise OSError("closed early")
                n += len(d)
            r["ok"] = True
        except OSError as e:
            r["error"] = type(e).__name__ if not str(e) else str(e)
        r["retrans"] = retrans(s)
        s.close()
        conns.append(r)
        sleep_until(t + p["every"])
    return dict(conns=conns)


def c_tcp_talk(f, p, t0, grace):
    sleep_until(t0)
    s = sock(socket.SOCK_STREAM, f["c"], f["cport"])
    s.settimeout(p.get("timeout", 6))
    r = dict(done=False, connect_ms=None, exchanges=0)
    t = mono()
    try:
        s.connect((f["s"], f["sport"]))
        r["connect_ms"] = round((mono() - t) * 1000, 1)
        while mono() < t0 + p["seconds"]:
            t1 = mono()
            s.sendall(b"t" * 1024)
            n = 0
            while n < 1024:
                d = s.recv(65536)
                if not d:
                    raise OSError("closed early")
                n += len(d)
            r["exchanges"] += 1
            sleep_until(t1 + p["every"])
        r["done"] = True
    except OSError as e:
        r["error"] = type(e).__name__ if not str(e) else str(e)
        r["failed_at_s"] = round(mono() - t0, 1)
    r["retrans"] = retrans(s)
    s.close()
    return r


def c_udp_ladder(f, p, t0, grace):
    """one new flow (a new client port) per delay: the server holds its
    answer back that long. Is it let through?"""
    sleep_until(t0)
    answered = []
    for k, ms in enumerate(p["delays_ms"]):
        s = sock(socket.SOCK_DGRAM, f["c"], f["cport"] + k)
        s.connect((f["s"], f["sport"]))
        try:
            s.send(struct.pack("!d", ms / 1000))
            answered.append(bool(select.select([s], [], [], ms / 1000 + f["rtt_ms"] / 1000 + 0.4)[0]))
        except OSError:
            answered.append(False)
        s.close()
    return dict(answered=answered)


def s_udp_ladder(f, p, t0, grace):
    """answer every datagram after the delay it asks for"""
    s = sock(socket.SOCK_DGRAM, f["s"], f["sport"])
    end, due = t0 + sum(p["delays_ms"]) / 1000 + len(p["delays_ms"]) * 0.6 + 2, []
    while mono() < end:
        wait = 0.2 if not due else max(0, min(due)[0] - mono())
        if select.select([s], [], [], wait)[0]:
            try:
                d, addr = s.recvfrom(64)
                due.append((mono() + struct.unpack("!d", d[:8])[0], addr))
            except (OSError, struct.error):
                pass
        for item in [x for x in due if x[0] <= mono()]:
            due.remove(item)
            try:
                s.sendto(b"answer", item[1])
            except OSError:
                pass
    return {}


# ----------------------------------------------------------- measurements
# These run alone. What arrives is counted on the endpoint's interface, not
# by a reader in Python, so the endpoint is not what limits the number.
def rx_packets(dev="eth0"):
    for line in open("/proc/net/dev"):
        name, _, rest = line.partition(":")
        if name.strip() == dev:
            return int(rest.split()[1])
    return 0


def flood(s, peer, size, until):
    """as many datagrams as one core sends through the whole path"""
    data, n = b"f" * size, 0
    while mono() < until:
        for _ in range(200):
            try:
                s.sendto(data, peer)
                n += 1
            except OSError:
                pass
    return n


def c_udp_flood(f, p, t0, grace):
    """dir=up: the client floods. dir=down: the client opens the flow and
    keeps it alive, the server floods back from 2 s on (the sync has had its
    time); the client counts."""
    s = sock(socket.SOCK_DGRAM, f["c"], f["cport"])
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    peer = (f["s"], f["sport"])
    sleep_until(t0)
    if p["dir"] == "up":
        return dict(sent=flood(s, peer, p["size"], t0 + p["seconds"]))
    rx0, end = None, t0 + 2 + p["seconds"]
    while True:                                     # count from 0.3 s into the flood to 0.3 s before its end
        s.sendto(b"open", peer)
        time.sleep(0.25)
        now = mono()
        if rx0 is None and now >= t0 + 2.3:
            rx0, ta = rx_packets(), now
        if rx0 is not None and now >= end - 0.3:
            return dict(delivered_pps=round((rx_packets() - rx0) / (now - ta)))


def s_udp_flood(f, p, t0, grace):
    s = sock(socket.SOCK_DGRAM, f["s"], f["sport"])
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    if p["dir"] == "up":
        sleep_until(t0 + 0.3)
        rx0, ta = rx_packets(), mono()
        sleep_until(t0 + p["seconds"] - 0.3)
        return dict(delivered_pps=round((rx_packets() - rx0) / (mono() - ta)))
    sleep_until(t0 - 0.5)
    s.settimeout(5)
    try:
        _, peer = s.recvfrom(64)
    except OSError:
        return dict(sent=0)
    t1 = mono()
    sleep_until(t1 + 2)
    return dict(sent=flood(s, peer, p["size"], t1 + 2 + p["seconds"]))


def c_tcp_bulk(f, p, t0, grace):
    """the server sends as fast as it can for `seconds`"""
    sleep_until(t0)
    s = sock(socket.SOCK_STREAM, f["c"], f["cport"])
    s.settimeout(8)
    r = dict(done=False, connect_ms=None, mbit=0)
    t = mono()
    try:
        s.connect((f["s"], f["sport"]))
        r["connect_ms"] = round((mono() - t) * 1000, 1)
        n, t1 = 0, mono()
        while True:
            d = s.recv(1 << 20)
            if not d:
                break
            n += len(d)
        r["mbit"] = round(n * 8 / (mono() - t1) / 1e6)
        r["done"] = True
    except OSError as e:
        r["error"] = str(e) or type(e).__name__
    s.close()
    return r


def udp_header(sport, dport, n):
    return struct.pack("!HHHH", sport, dport, 8 + n, 0)


def c_udp_newflows(f, p, t0, grace):
    """new flows at a rising rate: one datagram per new source port. The
    server answers each after reply_delay; an answer arrives only if the
    return gateway knows the flow by then."""
    s = socket.socket(socket.AF_INET6, socket.SOCK_RAW, socket.IPPROTO_UDP)
    s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_CHECKSUM, 6)
    s.bind((f["c"], 0))
    sleep_until(t0)
    port, steps = 1024, []
    for rate in p["rates"]:
        rx0, t1, n = rx_packets(), mono(), int(rate * p["step_seconds"])
        for k in range(n):
            d = t1 + k / rate - mono()
            if d > 0:
                time.sleep(d)
            try:
                s.sendto(udp_header(port, f["sport"], 8) + b"newflow!", (f["s"], 0))
            except OSError:
                pass
            port = port + 1 if port < 65535 else 1024
        sent_in = mono() - t1
        time.sleep(p["reply_delay"] + 0.7)
        steps.append(dict(rate=rate, sent=n, answered=rx_packets() - rx0,
                          achieved=round(n / sent_in)))
    return dict(steps=steps)


def s_udp_newflows(f, p, t0, grace):
    import collections
    s = sock(socket.SOCK_DGRAM, f["s"], f["sport"])
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
    end = t0 + len(p["rates"]) * (p["step_seconds"] + p["reply_delay"] + 1.2) + 2
    due, n = collections.deque(), 0
    while mono() < end:
        wait = 0.2 if not due else max(0, due[0][0] - mono())
        if select.select([s], [], [], wait)[0]:
            for _ in range(64):
                try:
                    d, addr = s.recvfrom(64, socket.MSG_DONTWAIT)
                except OSError:
                    break
                due.append((mono() + p["reply_delay"], addr))
                n += 1
        while due and due[0][0] <= mono():
            try:
                s.sendto(b"answer", due.popleft()[1])
            except OSError:
                pass
    return dict(seen=n)


# ---------------------------------------------------------------- server
def s_udp_rr(f, p, t0, grace):
    s = sock(socket.SOCK_DGRAM, f["s"], f["sport"])
    end, n = t0 + p["requests"] * p["every"] + 2, 0
    while mono() < end:
        if not select.select([s], [], [], 0.5)[0]:
            continue
        try:
            d, addr = s.recvfrom(2048)
            s.sendto(d, addr)
            n += 1
        except OSError:
            pass
    return dict(requests_seen=n)


def s_udp_stream(f, p, t0, grace):
    s = sock(socket.SOCK_DGRAM, f["s"], f["sport"])
    got, sent, gap, refused = stream(s, None, p["down_pps"], p["seconds"], t0 + p["seconds"] + 2.0)
    return dict(tx=sent, rx=len(got), lost=missing(got, p["up_pps"], p["seconds"]), refused=refused)


def s_tcp(f, p, t0, grace):
    ls = sock(socket.SOCK_STREAM, f["s"], f["sport"])
    ls.listen(16)
    ls.settimeout(0.5)
    span = p["seconds"] if "seconds" in p else p["connections"] * p["every"]
    end, stat, workers = t0 + span + 8, dict(accepted=0, errors=0), []

    def serve(c):
        c.settimeout(10)
        try:
            if p["kind"] == "tcp_bulk":
                chunk, until = b"b" * 65536, mono() + p["seconds"]
                while mono() < until:
                    c.sendall(chunk)
                stat["retrans"] = retrans(c)
            elif p["kind"] == "tcp_short":
                n = 0
                while n < p["request"]:
                    d = c.recv(65536)
                    if not d:
                        return
                    n += len(d)
                c.sendall(b"r" * p["response"])
            else:
                while True:
                    d = c.recv(65536)
                    if not d:
                        return
                    c.sendall(d)
        except OSError:
            stat["errors"] += 1
        finally:
            c.close()

    while mono() < end:
        try:
            c, _ = ls.accept()
        except OSError:
            continue
        stat["accepted"] += 1
        w = threading.Thread(target=serve, args=(c,), daemon=True)
        w.start()
        workers.append(w)
    return stat


KINDS = {
    "client": dict(udp_rr=c_udp_rr, udp_stream=c_udp_stream, tcp_short=c_tcp_short, tcp_talk=c_tcp_talk,
                   udp_flood=c_udp_flood, tcp_bulk=c_tcp_bulk, udp_newflows=c_udp_newflows,
                   udp_ladder=c_udp_ladder),
    "server": dict(udp_rr=s_udp_rr, udp_stream=s_udp_stream, tcp_short=s_tcp, tcp_talk=s_tcp,
                   udp_flood=s_udp_flood, tcp_bulk=s_tcp, udp_newflows=s_udp_newflows,
                   udp_ladder=s_udp_ladder),
}


def main():
    role, job, out = sys.argv[1], json.load(open(sys.argv[2])), open(sys.argv[3], "w", buffering=1)
    lock = threading.Lock()

    def run(f):
        try:
            r = KINDS[role][f["p"]["kind"]](f, f["p"], job["t0"] + f["start"], job["grace"])
        except Exception as e:      # a flow that could not even run is a result, too
            r = dict(crashed="%s: %s" % (type(e).__name__, e))
        with lock:
            out.write(json.dumps(dict(id=f["id"], role=role, **r)) + "\n")

    threading.stack_size(256 * 1024)
    threads = [threading.Thread(target=run, args=(f,)) for f in job["flows"]]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


if __name__ == "__main__":
    main()
