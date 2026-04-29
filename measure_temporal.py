"""
Temporal Stability 분석 스크립트
- 두 독립 캡처 세션(screenshots / screenshots005)을 프록시로 사용
  → 동일 씬, 동일 spp의 두 독립 렌더링 간 차이
  → 실제 연속 프레임 차이의 상한(upper bound) — 연속 프레임은 더 안정적
- 지표: Mean Absolute Error (MAE), Std, max

직접 연속 프레임을 캡처했다면:
  BASE_A = 첫 번째 캡처 폴더, BASE_B = 두 번째 폴더로 교체
"""

import os
import numpy as np
from PIL import Image

BASE_A = r"C:\Users\sigun\University\프로젝트\test_tracing\screenshots"
BASE_B = r"C:\Users\sigun\University\프로젝트\test_tracing\screenshots005"
OUT    = r"C:\Users\sigun\University\프로젝트\test_tracing\temporal_results"
os.makedirs(OUT, exist_ok=True)

SCENES = ["scene5", "scene7", "scene8", "scene4"]

FILES = {
    "fresnel":  "screenshot_fresnel_00100spp.bmp",
    "variance": "screenshot_variance_00100spp.bmp",
    "baseline": "screenshot_baseline_00100spp.bmp",
}


def load(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def temporal_error(path_a, path_b):
    if not os.path.exists(path_a) or not os.path.exists(path_b):
        return None
    a = load(path_a)
    b = load(path_b)
    diff = np.abs(a - b)
    mae  = float(diff.mean())
    std  = float(diff.std())
    pct99 = float(np.percentile(diff, 99))
    # diff map 저장 (밝기 강조)
    return mae, std, pct99, diff


print("두 독립 캡처 세션 간 MAE (낮을수록 안정적)")
print(f"{'씬':<10} {'방법':<12} {'MAE':>8} {'Std':>8} {'P99':>8}")
print("-" * 52)

summary = {}
for scene in SCENES:
    dir_a = os.path.join(BASE_A, scene)
    dir_b = os.path.join(BASE_B, scene)
    row = {}
    for key, fname in FILES.items():
        pa = os.path.join(dir_a, fname)
        pb = os.path.join(dir_b, fname)
        result = temporal_error(pa, pb)
        if result is None:
            print(f"{scene:<10} {key:<12} {'N/A':>8}")
            continue
        mae, std, p99, diff = result
        row[key] = (mae, std, p99)
        print(f"{scene:<10} {key:<12} {mae:8.5f} {std:8.5f} {p99:8.5f}")

        # diff map 저장
        diff_vis = (np.clip(diff * 5, 0, 1) * 255).astype(np.uint8)  # 5× 강조
        Image.fromarray(diff_vis).save(
            os.path.join(OUT, f"{scene}_{key}_temporal_diff.png")
        )
    summary[scene] = row
    print()

# ── 논문용 요약 ───────────────────────────────────────────────
print("=" * 65)
print("논문용 요약 - Temporal MAE (lower = more stable)")
print("두 독립 100spp 렌더링 간 차이 -> 실제 연속 프레임의 상한")
print("=" * 65)
print(f"{'씬':<10} {'Baseline':>10} {'Fresnel':>10} {'Variance':>10}  {'ΔF-B':>8}")
print("-" * 55)
for scene, row in summary.items():
    b = row.get("baseline", (None,))[0]
    f = row.get("fresnel",  (None,))[0]
    v = row.get("variance", (None,))[0]
    bs = f"{b:.5f}" if b else "  N/A"
    fs = f"{f:.5f}" if f else "  N/A"
    vs = f"{v:.5f}" if v else "  N/A"
    df = f"{f-b:+.5f}" if (f and b) else "  N/A"
    print(f"{scene:<10} {bs:>10} {fs:>10} {vs:>10}  {df:>8}")

print(f"\ndiff maps (5× 강조) → {OUT}")
print("완료.")
