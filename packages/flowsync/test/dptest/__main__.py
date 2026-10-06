"""python3 -m dptest --flowsync PATH --bpf-object PATH [options]     (as root)

  --scenarios A B ..  only these (default: all but the "slow" ones)
  --tags T ..         only scenarios with one of these tags
  --jobs N            workers in parallel (default: 2/3 of the CPUs, at most 24)
  --log FILE          timestamped progress of every check (tail -f it)
  --quiet, -q         print failures, the result table and the summary only
  --keep              keep the logs of passing runs too
  --list              list scenarios
"""
import argparse
import os
import sys

from . import runner
from .scenario import load


def main():
    ap = argparse.ArgumentParser(prog="dptest", usage=__doc__)
    ap.add_argument("--flowsync")
    ap.add_argument("--bpf-object", dest="bpf_object")
    ap.add_argument("--scenarios", nargs="*", default=[])
    ap.add_argument("--tags", nargs="*", default=[])
    ap.add_argument("--jobs", type=int, default=max(2, min(24, (os.cpu_count() or 4) * 2 // 3)))
    ap.add_argument("--log", default="/tmp/flowsync-dptest.log")
    ap.add_argument("--out")
    ap.add_argument("--keep", action="store_true", help="keep logs of passing runs")
    ap.add_argument("--verbose", "-v", action="store_true")
    ap.add_argument("--quiet", "-q", action="store_true")
    ap.add_argument("--list", action="store_true")
    # child mode, started by the parent inside unshare -n
    ap.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    args, rest = ap.parse_known_args()
    if args.child:
        return runner.child(args)
    if args.list:
        for s in load().values():
            print("%-24s %d gateways %s" % (s.name, s.gateways, ",".join(sorted(s.tags))))
        return 0
    if not args.flowsync or not args.bpf_object:
        ap.error("--flowsync and --bpf-object are required")
    if os.geteuid():
        ap.error("needs root: BPF programs cannot be loaded from a user namespace")
    return runner.parent(args)


if __name__ == "__main__":
    sys.exit(main())
