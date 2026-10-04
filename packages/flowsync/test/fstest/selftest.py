"""Unit tests of the framework itself, no namespaces needed:

  python3 -m unittest fstest.selftest
"""
import unittest

from . import matrix
from .matrix import Profile, parse
from .scenario import Scenario


class TestSpec(unittest.TestCase):
    def test_defaults(self):
        c = parse("")
        self.assertEqual(c.n, 3)
        self.assertEqual(c.p, [Profile(False, True)] * 3)

    def test_named(self):
        self.assertEqual(parse("g3 offload noack").p, [Profile(True, False)] * 3)
        self.assertEqual(parse("g3 mixed").p,
                         [Profile(True, True), Profile(True, True), Profile(False, True)])
        self.assertEqual(parse("g2 plain").n, 2)

    def test_indexes_and_overrides(self):
        c = parse("g4 offload=0,2 ack=1 g3:offload,noack")
        self.assertEqual([p.offload for p in c.p], [True, False, True, True])
        self.assertEqual([p.ack for p in c.p], [False, True, False, False])

    def test_name_and_roundtrip(self):
        c = parse("g3 mixed noack")
        self.assertEqual(c.name, "g3 off=01 ack=-")
        self.assertEqual(parse(c.spec()), c)
        self.assertEqual(parse(c.name), c)      # names as printed can be passed back
        self.assertEqual(parse("g3 off=012 ack=012").name, "g3 off=012 ack=012")

    def test_errors(self):
        for bad in ("g3 offload=3", "g3 bogus", "g2 g5:offload", "g0", "g3 g1:fast"):
            with self.assertRaises(ValueError):
                parse(bad)

    def test_matrices(self):
        d = matrix.expand("default")
        self.assertEqual(len(d), len(matrix.MATRICES["default"]))
        self.assertTrue(any(any(p.offload for p in c.p) and not all(p.offload for p in c.p)
                            for c in d), "default has a mixed combination")
        f = matrix.expand("full")
        self.assertEqual(len(f), len({c.name for c in f}), "full has no duplicates")
        self.assertEqual({c.n for c in f}, {2, 3, 4})
        self.assertEqual(matrix.expand(profiles=["g2 plain"])[0].n, 2)
        with self.assertRaises(ValueError):
            matrix.expand("nope")

    def test_depends(self):
        udp = Scenario(lambda env: None, 2, None, {"basic"}, False, None)
        tcp = Scenario(lambda env: None, 2, None, {"tcp"}, False, None)
        plain_ack, plain_noack, mixed = (parse(s) for s in ("g3 plain ack", "g3 plain noack",
                                                            "g3 mixed ack"))
        self.assertEqual(udp.key(plain_ack), udp.key(plain_noack), "ACK rule: TCP only")
        self.assertNotEqual(tcp.key(plain_ack), tcp.key(plain_noack))
        self.assertNotEqual(udp.key(plain_ack), udp.key(mixed))
        self.assertNotEqual(udp.key(plain_ack), udp.key(parse("g2 plain ack")))


if __name__ == "__main__":
    unittest.main()
