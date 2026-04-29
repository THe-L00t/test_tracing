"""
FLIP 분석 스크립트
- 외부 perceptual metric (NVIDIA FLIP)으로 wPSNR circular logic 방어
- 씬별: Fresnel vs GT, Variance vs GT, Baseline vs GT
- 출력: 씬별 mean FLIP 점수 테이블 + FLIP error map PNG 저장
"""

import os
import numpy as np
from PIL import Image
import flip_evaluator

BASE = r"C:\Users\sigun\University\프로젝트\test_tracing\screenshots"
OUT  = r"C:\Users\sigun\University\프로젝트\test_tracing\flip_results"
os.makedirs(OUT, exist_ok=True)

SCENES = ["scene5", "scene7", "scene8", "scene4", "scene6"]

FILE = {
    "fresnel":  "screenshot_fresnel_00100spp.bmp",
    "variance": "screenshot_variance_00100spp.bmp",
    "baseline": "screenshot_baseline_00100spp.bmp",
    "gt":       "screenshot_baseline_10000spp.bmp",
}

LABELS = {
    "fresnel":  "Fresnel (100spp)",
    "variance": "Variance (100spp)",
    "baseline": "Baseline (100spp)",
}


def load_ldr(path):
    """BMP → [0,1] float32 numpy (H,W,3)."""
    arr = np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    return arr


def run_flip(ref_path, test_path, tag, scene):
    if not os.path.exists(ref_path) or not os.path.exists(test_path):
        return None
    ref  = load_ldr(ref_path)
    test = load_ldr(test_path)
    err_map, mean_err, _ = flip_evaluator.evaluate(ref, test, "LDR")
    # error map 저장 (float32 → uint8 변환)
    out_path = os.path.join(OUT, f"{scene}_{tag}_flip.png")
    err_uint8 = (np.clip(err_map, 0, 1) * 255).astype(np.uint8)
    Image.fromarray(err_uint8).save(out_path)
    return mean_err


print(f"{'씬':<10} {'방법':<22} {'mean FLIP':>10}  (낮을수록 좋음, 0=완벽)")
print("-" * 50)

results = {}
for scene in SCENES:
    scene_dir = os.path.join(BASE, scene)
    gt_path   = os.path.join(scene_dir, FILE["gt"])
    if not os.path.exists(gt_path):
        print(f"{scene:<10} GT 없음 — SKIP")
        continue

    row = {}
    for key in ("fresnel", "variance", "baseline"):
        test_path = os.path.join(scene_dir, FILE[key])
        tag = key
        score = run_flip(gt_path, test_path, tag, scene)
        row[key] = score
        label = LABELS[key]
        score_str = f"{score:.4f}" if score is not None else "  N/A"
        print(f"{scene:<10} {label:<22} {score_str:>10}")
    results[scene] = row
    print()

# ── 요약 테이블 (논문 삽입용) ─────────────────────────────
print("\n" + "=" * 60)
print("논문용 요약 (mean FLIP ↓ better)")
print("=" * 60)
print(f"{'씬':<10} {'Baseline':>10} {'Fresnel':>10} {'Variance':>10}  {'ΔF-B':>8}  {'ΔV-B':>8}")
print("-" * 60)
for scene, row in results.items():
    b = row.get("baseline")
    f = row.get("fresnel")
    v = row.get("variance")
    df = f"{f-b:+.4f}" if (f and b) else "  N/A"
    dv = f"{v-b:+.4f}" if (v and b) else "  N/A"
    bs = f"{b:.4f}" if b else "  N/A"
    fs = f"{f:.4f}" if f else "  N/A"
    vs = f"{v:.4f}" if v else "  N/A"
    print(f"{scene:<10} {bs:>10} {fs:>10} {vs:>10}  {df:>8}  {dv:>8}")

print(f"\nFLIP error maps → {OUT}")
print("완료.")
