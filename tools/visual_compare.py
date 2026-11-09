#!/usr/bin/env python3
"""Compare AMPLIFY-OS reference and actual screenshots without extra deps.
Usage: visual_compare.py reference.png actual.png output_prefix
Writes <prefix>_diff.png and prints normalized RGB/edge/layout metrics.
"""
import sys
from pathlib import Path
import cv2
import numpy as np

if len(sys.argv) != 4:
    raise SystemExit("usage: visual_compare.py reference.png actual.png output_prefix")
ref_path, act_path, prefix = sys.argv[1:]
ref = cv2.imread(ref_path, cv2.IMREAD_COLOR)
act = cv2.imread(act_path, cv2.IMREAD_COLOR)
if ref is None or act is None:
    raise SystemExit("could not read one of the input images")
if act.shape[:2] != ref.shape[:2]:
    act = cv2.resize(act, (ref.shape[1], ref.shape[0]), interpolation=cv2.INTER_AREA)

ref_f = ref.astype(np.float32)
act_f = act.astype(np.float32)
absdiff = np.abs(ref_f - act_f)
mae = float(absdiff.mean() / 255.0)
rmse = float(np.sqrt(np.mean((ref_f - act_f) ** 2)) / 255.0)

ref_gray = cv2.cvtColor(ref, cv2.COLOR_BGR2GRAY)
act_gray = cv2.cvtColor(act, cv2.COLOR_BGR2GRAY)
ref_edge = cv2.Canny(ref_gray, 70, 150)
act_edge = cv2.Canny(act_gray, 70, 150)
intersection = np.logical_and(ref_edge > 0, act_edge > 0).sum()
union = np.logical_or(ref_edge > 0, act_edge > 0).sum()
edge_iou = float(intersection / union) if union else 1.0

heat = np.clip(absdiff.mean(axis=2) * 3.0, 0, 255).astype(np.uint8)
heat = cv2.applyColorMap(heat, cv2.COLORMAP_MAGMA)
overlay = cv2.addWeighted(act, 0.45, heat, 0.55, 0)
out = str(Path(prefix).with_suffix("")) + "_diff.png"
cv2.imwrite(out, overlay)
print(f"size={ref.shape[1]}x{ref.shape[0]}")
print(f"rgb_mae={mae:.5f}")
print(f"rgb_rmse={rmse:.5f}")
print(f"edge_iou={edge_iou:.5f}")
print(f"diff={out}")
