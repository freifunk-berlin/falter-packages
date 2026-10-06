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
Losses are split at "grace" seconds into the flow: before it is the start of
the flow, after it the flow has to be clean.
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


def missing(got, pps, seconds, grace):
    """(lost at the start, lost later) of the numbered packets 0..n-1"""
    n, edge = int(pps * seconds), grace * pps
    lost = [k for k in range(n) if k not in got]
    return sum(1 for k in lost if k < edge), sum(1 for k in lost if k >= edge)


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
    start, late = missing(got, p["down_pps"], p["seconds"], grace)
    return dict(tx=sent, rx=len(got), lost_start=start, lost_late=late,
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
    start, late = missing(got, p["up_pps"], p["seconds"], grace)
    return dict(tx=sent, rx=len(got), lost_start=start, lost_late=late, refused=refused)


def s_tcp(f, p, t0, grace):
    ls = sock(socket.SOCK_STREAM, f["s"], f["sport"])
    ls.listen(16)
    ls.settimeout(0.5)
    span = p["seconds"] if p["kind"] == "tcp_talk" else p["connections"] * p["every"]
    end, stat, workers = t0 + span + 8, dict(accepted=0, errors=0), []

    def serve(c):
        c.settimeout(10)
        try:
            if p["kind"] == "tcp_short":
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
    "client": dict(udp_rr=c_udp_rr, udp_stream=c_udp_stream, tcp_short=c_tcp_short, tcp_talk=c_tcp_talk),
    "server": dict(udp_rr=s_udp_rr, udp_stream=s_udp_stream, tcp_short=s_tcp, tcp_talk=s_tcp),
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
