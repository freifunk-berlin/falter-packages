"""python3 -m fstest --flowsync PATH --ctquery PATH [options]

  --matrix NAME       default | quick | full (default: default)
  --profile SPEC      one combination instead of a matrix, repeatable
                      (e.g. "g3 mixed ack", "g2 plain noack", "g3 plain g0:offload")
  --scenarios A B ..  only these (default: all but the "slow" ones)
  --tags T ..         only scenarios with one of these tags
  --jobs N            runs in parallel, one per scenario and combination
                      (default: 2/3 of the CPUs, at most 24)
  --heavy N           heavy scenarios (floods) at the same time (default 1)
  --log FILE          timestamped progress of every check (tail -f it)
  --quiet, -q         print failures, the result table and the summary only
  --keep              keep the logs of passing runs too
  --list              list scenarios and matrices
"""
import argparse
import os
import sys

from . import matrix, runner
from .scenario import load


def main():
    ap = argparse.ArgumentParser(prog="fstest", usage=__doc__)
    ap.add_argument("--flowsync")
    ap.add_argument("--ctquery")
    ap.add_argument("--matrix", default="default")
    ap.add_argument("--profile", dest="profile_list", action="append", default=[])
    ap.add_argument("--scenarios", nargs="*", default=[])
    ap.add_argument("--tags", nargs="*", default=[])
    ap.add_argument("--jobs", type=int, default=max(2, min(24, (os.cpu_count() or 4) * 2 // 3)))
    ap.add_argument("--heavy", type=int, default=1, help="heavy scenarios at the same time")
    ap.add_argument("--log", default="/tmp/flowsync-test.log")
    ap.add_argument("--out")
    ap.add_argument("--keep", action="store_true", help="keep logs of passing runs")
    ap.add_argument("--verbose", "-v", action="store_true")
    ap.add_argument("--quiet", "-q", action="store_true")
    ap.add_argument("--list", action="store_true")
    # child mode, started by the parent inside unshare -Urn
    ap.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--first", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--lockdir", help=argparse.SUPPRESS)
    args, rest = ap.parse_known_args()
    if args.child:
        # the parent passes --profile SPEC once: it lands in profile_list
        args.profile = args.profile_list[0]
        return runner.child(args)
    if args.list:
        for s in load().values():
            print("%-24s g>=%d %s%s" % (s.name, s.gateways, ",".join(sorted(s.tags)),
                                        " (once)" if s.once else ""))
        for name, specs in matrix.MATRICES.items():
            print("matrix %s: %s" % (name, "; ".join(specs)))
        print("matrix full: %d combinations" % len(matrix.full()))
        return 0
    if not args.flowsync or not args.ctquery:
        ap.error("--flowsync and --ctquery are required")
    return runner.parent(args)


if __name__ == "__main__":
    sys.exit(main())
