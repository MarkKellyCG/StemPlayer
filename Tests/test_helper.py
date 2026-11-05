"""Tests for Helper/stemsplit.py that need no PyTorch.  Run:  python Tests/test_helper.py"""
import os, random, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Helper"))
import stemsplit as ss


def test_plan_chunks():
    rnd = random.Random(1)
    for _ in range(500):
        chunk = rnd.randint(1000, 20000)
        ovl = rnd.randint(10, chunk // 4)
        n = rnd.randint(1, 200000)
        spans = ss.plan_chunks(n, chunk, ovl)
        assert spans[0][0] == 0 and spans[-1][1] == n, (n, chunk, ovl, spans)
        for (s0, e0), (s1, e1) in zip(spans, spans[1:]):
            assert s1 > s0 and e1 > e0
            assert e0 - s1 >= ovl, "overlap smaller than requested"   # contiguous with overlap
            assert e0 - s1 < chunk
        assert all(e - s <= chunk for s, e in spans)
    print("plan_chunks OK")


def test_crossfade_reconstructs():
    rnd = np.random.default_rng(0)
    for n, chunk, ovl in [(5000, 2000, 300), (123457, 20000, 2500), (1500, 2000, 300), (40001, 10000, 1000)]:
        x = rnd.standard_normal((2, n)).astype("float32")
        fn = lambda c: np.stack([c * (k + 1) for k in range(6)])
        y = ss.separate_chunked(fn, x, chunk, ovl)
        exp = np.stack([x * (k + 1) for k in range(6)])
        err = float(np.max(np.abs(y - exp)))
        assert y.shape == exp.shape and err < 1e-4, (n, chunk, ovl, err)
    print("crossfade reconstruction OK")


def test_resample_tone():
    sr0, sr1 = 44100, 48000
    t = np.arange(sr0) / sr0
    x = np.stack([0.5 * np.sin(2 * np.pi * 1000 * t)] * 2).astype("float32")
    y = ss.resample(x, sr0, sr1)
    assert abs(y.shape[1] - sr1) <= 1, y.shape
    spec = np.abs(np.fft.rfft(y[0] * np.hanning(y.shape[1])))
    f = np.argmax(spec) * sr1 / y.shape[1]
    assert abs(f - 1000) < 2, f
    print("resample tone OK (peak %.1f Hz, %d samples)" % (f, y.shape[1]))


if __name__ == "__main__":
    test_plan_chunks(); test_crossfade_reconstructs(); test_resample_tone()
