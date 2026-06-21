#!/usr/bin/env python3
"""
Frame formatting: Spectra ISP output (NV12) -> openpilot model input tensor.

The IFE in IFE_PROCESSED mode emits NV12 (Y plane + interleaved UV, BT.601).
The openpilot model wants two consecutive frames reprojected to YUV420 and
stacked into (1, 12, 128, 256) uint8, per camera. This module does the NV12 ->
6-channel YUV split and the 2-frame stack.

This is the glue between src/spectra_capture.c (once it streams real frames into
a shared buffer) and model/bench.py. The math here is correct and testable now
with synthetic NV12; only the *source* of the NV12 bytes is pending.
"""
import numpy as np


def nv12_to_yuv6(nv12: np.ndarray, width: int, height: int, stride: int):
    """NV12 (Y plane stride x height, then UV plane stride x height/2) ->
    6 channels for one frame, openpilot layout:
      ch0..3: Y subsampled into 4 interleaved quadrants (h/2 x w/2)
      ch4:    U (h/2 x w/2)
      ch5:    V (h/2 x w/2)
    Returns array (6, height//2, width//2) uint8.
    """
    y = nv12[:stride * height].reshape(height, stride)[:, :width]
    uv = nv12[stride * height: stride * height + stride * (height // 2)]
    uv = uv.reshape(height // 2, stride)[:, :width]
    u = uv[:, 0::2]
    v = uv[:, 1::2]
    h2, w2 = height // 2, width // 2
    out = np.empty((6, h2, w2), dtype=np.uint8)
    out[0] = y[0::2, 0::2]   # top-left
    out[1] = y[0::2, 1::2]   # top-right
    out[2] = y[1::2, 0::2]   # bottom-left
    out[3] = y[1::2, 1::2]   # bottom-right
    out[4] = u
    out[5] = v
    return out


def stack_two_frames(prev6, cur6):
    """Stack two 6-channel frames into (1, 12, H, W) uint8 -- the model input."""
    return np.concatenate([prev6, cur6], axis=0)[None, ...]


if __name__ == "__main__":
    # self-test with synthetic NV12 at the model's 256x128 working res
    W, H, S = 256, 128, 256
    nv12 = (np.random.rand(S * H + S * (H // 2)) * 255).astype(np.uint8)
    f6 = nv12_to_yuv6(nv12, W, H, S)
    assert f6.shape == (6, 64, 128), f6.shape
    t = stack_two_frames(f6, f6)
    assert t.shape == (1, 12, 64, 128), t.shape
    print("frames.py self-test OK:", t.shape, t.dtype)
