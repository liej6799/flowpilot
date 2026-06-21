#!/usr/bin/env python3
"""
op9_camera_model: openpilot v0.11 two-camera driving-model inference benchmark.

Loads a supercombo model (ONNX) and measures inference speed feeding it the
two-camera (wide + narrow) inputs the v0.11 model expects. Until the raw Spectra
capture path streams real frames, this uses synthetic frames of the correct
shape/dtype so the *inference speed* number is real and representative.

The v0.11 driving model ("big model" lineage) takes two YUV camera streams.
Each camera frame is two consecutive frames in YUV420, reprojected and stacked
into (1, 12, 128, 256), uint8. Plus the usual float32 side inputs
(desire, traffic_convention, lateral_control_params, prev_desired_curv,
nav_features, nav_instructions, features_buffer ...). Exact input names/shapes
are read from the ONNX graph, so this works for v0.8/v0.9/v0.11 supercombos.

Usage:
  python3 bench.py --model /path/to/supercombo.onnx [--runs 200] [--provider cpu|tinygrad]

Notes:
- On the OnePlus 9 the real win is the THNEED/Adreno GPU runner; ONNXRuntime CPU
  here gives a portable baseline. Pass --provider tinygrad to use the tinygrad
  GPU JIT (matches what flowpilot's modeld uses) if tinygrad+CL is available.
"""
import argparse
import time
import sys
import numpy as np


def load_onnx_io(model_path):
    import onnx
    m = onnx.load(model_path)
    inputs = {}
    for inp in m.graph.input:
        shp = tuple(d.dim_value if d.dim_value > 0 else 1
                    for d in inp.type.tensor_type.shape.dim)
        inputs[inp.name] = shp
    outshapes = {}
    for o in m.graph.output:
        outshapes[o.name] = tuple(d.dim_value if d.dim_value > 0 else 1
                                  for d in o.type.tensor_type.shape.dim)
    return inputs, outshapes


def make_inputs(input_shapes):
    """Synthetic inputs matching shapes; image inputs uint8, rest float32.
    Image inputs are identified by name containing 'img' (the v0.11/0.9 dual-cam
    inputs are 'input_imgs' / 'big_input_imgs')."""
    feed = {}
    for name, shp in input_shapes.items():
        if "img" in name.lower():
            feed[name] = (np.random.rand(*shp) * 255).astype(np.uint8)
        else:
            feed[name] = np.random.randn(*shp).astype(np.float32)
    return feed


def bench_onnxruntime(model_path, runs):
    import onnxruntime as ort
    input_shapes, out_shapes = load_onnx_io(model_path)
    print(f"model inputs ({len(input_shapes)}):")
    img_inputs = [n for n in input_shapes if "img" in n.lower()]
    for n, s in input_shapes.items():
        tag = "  <- camera" if n in img_inputs else ""
        print(f"  {n:32s} {s}{tag}")
    print(f"detected {len(img_inputs)} camera image input(s): {img_inputs}")
    print(f"  (v0.11 dual-camera = 2 image inputs: wide + narrow)")

    so = ort.SessionOptions()
    so.intra_op_num_threads = 4
    providers = ort.get_available_providers()
    print(f"\nONNXRuntime providers: {providers}")
    sess = ort.InferenceSession(model_path, so, providers=providers)

    feed = make_inputs(input_shapes)
    # filter feed to actual session inputs
    sess_in = {i.name for i in sess.get_inputs()}
    feed = {k: v for k, v in feed.items() if k in sess_in}

    # warmup
    for _ in range(5):
        sess.run(None, feed)

    ts = []
    for _ in range(runs):
        t0 = time.perf_counter()
        sess.run(None, feed)
        ts.append(time.perf_counter() - t0)
    return ts


def bench_tinygrad(model_path, runs):
    # tinygrad GPU JIT path (matches flowpilot modeld). Requires tinygrad + an
    # OpenCL/GPU backend available in the proot.
    import os
    os.environ.setdefault("FLOAT16", "1")
    os.environ.setdefault("IMAGE", "2")
    try:
        from tinygrad.tensor import Tensor
        from tinygrad import Device
    except Exception as e:
        print("tinygrad not available:", e)
        sys.exit(2)
    print("tinygrad Device:", Device.DEFAULT)
    # Minimal: load via onnx2tinygrad if present; else fall back.
    # (Left as a hook -- the openpilot compile3.py flow builds the JIT pkl.)
    print("NOTE: full tinygrad JIT load uses openpilot's compile3.py to produce a")
    print(".pkl; wire that here. For now use --provider cpu for a real number.")
    sys.exit(2)


def report(ts):
    a = np.array(ts) * 1000.0
    a.sort()
    print("\n=== inference speed ===")
    print(f"  runs:   {len(a)}")
    print(f"  mean:   {a.mean():.2f} ms   ({1000.0/a.mean():.1f} Hz)")
    print(f"  median: {np.median(a):.2f} ms")
    print(f"  p90:    {a[int(len(a)*0.9)]:.2f} ms")
    print(f"  min:    {a.min():.2f} ms   max: {a.max():.2f} ms")
    print(f"\n  openpilot runs the model at 20 Hz (50 ms budget).")
    print(f"  -> {'MEETS' if a.mean() < 50 else 'EXCEEDS'} the 50 ms real-time budget on this provider.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="path to supercombo.onnx")
    ap.add_argument("--runs", type=int, default=200)
    ap.add_argument("--provider", choices=["cpu", "tinygrad"], default="cpu")
    args = ap.parse_args()

    print(f"op9_camera_model inference benchmark")
    print(f"model: {args.model}\nprovider: {args.provider}\n")

    if args.provider == "tinygrad":
        ts = bench_tinygrad(args.model, args.runs)
    else:
        ts = bench_onnxruntime(args.model, args.runs)
    report(ts)


if __name__ == "__main__":
    main()
