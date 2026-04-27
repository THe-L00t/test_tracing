"""
compare.py  —  PSNR / SSIM 비교 스크립트
Fresnel-Guided PT 연구용

사용법:
    python compare.py --gt <ground_truth.bmp> --a <baseline.bmp> --b <fresnel.bmp>

예시:
    python compare.py ^
        --gt screenshot_baseline_10000spp.bmp ^
        --a  screenshot_baseline_00100spp.bmp ^
        --b  screenshot_fresnel_00100spp.bmp

출력:
    - 전체 이미지 PSNR / SSIM
    - Fresnel 마스크 영역(고F(p)) 크롭 PSNR / SSIM
    - 차이 이미지 저장 (diff_a.png, diff_b.png)
"""

import argparse
import sys
import io
import numpy as np

# Windows 콘솔 UTF-8 출력
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
from pathlib import Path
from PIL import Image


def load_rgb(path: str) -> np.ndarray:
    """BMP/PNG -> float32 [0,1] RGB 배열 (H, W, 3)"""
    img = Image.open(path).convert("RGB")
    return np.array(img, dtype=np.float32) / 255.0


def psnr(ref: np.ndarray, tst: np.ndarray) -> float:
    mse = np.mean((ref - tst) ** 2)
    if mse < 1e-10:
        return float("inf")
    return 10.0 * np.log10(1.0 / mse)


def ssim(ref: np.ndarray, tst: np.ndarray) -> float:
    """채널별 SSIM 평균 (단순 구현, scikit-image 없을 때 사용)"""
    try:
        from skimage.metrics import structural_similarity
        scores = [
            structural_similarity(ref[..., c], tst[..., c], data_range=1.0)
            for c in range(3)
        ]
        return float(np.mean(scores))
    except ImportError:
        # fallback: 간이 SSIM
        C1, C2 = (0.01 ** 2), (0.03 ** 2)
        mu1, mu2 = ref.mean(), tst.mean()
        s1, s2, s12 = ref.std(), tst.std(), np.mean((ref - mu1) * (tst - mu2))
        return float(((2*mu1*mu2 + C1)*(2*s12 + C2)) /
                     ((mu1**2 + mu2**2 + C1)*(s1**2 + s2**2 + C2)))


def luminance_mask(img: np.ndarray, percentile: float = 70.0) -> np.ndarray:
    """밝기 기준 상위 percentile 픽셀 마스크 — Fresnel 강조 영역 근사"""
    lum = 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]
    threshold = np.percentile(lum, percentile)
    return lum >= threshold


def save_diff(ref: np.ndarray, tst: np.ndarray, path: str, scale: float = 5.0):
    """차이 이미지 저장 (절댓값 차이 × scale, 클램프)"""
    diff = np.clip(np.abs(ref - tst) * scale, 0, 1)
    Image.fromarray((diff * 255).astype(np.uint8)).save(path)


def report(label: str, ref: np.ndarray, tst: np.ndarray, mask: np.ndarray | None = None):
    if mask is not None:
        ref_m = ref[mask]
        tst_m = tst[mask]
        p = psnr(ref_m.reshape(-1, 3) if ref_m.ndim == 1 else ref_m,
                 tst_m.reshape(-1, 3) if tst_m.ndim == 1 else tst_m)
        # SSIM은 마스크 영역 전체 이미지로 계산 (구조 정보 필요)
        ref_masked = ref * mask[..., None]
        tst_masked = tst * mask[..., None]
        s = ssim(ref_masked, tst_masked)
    else:
        p = psnr(ref, tst)
        s = ssim(ref, tst)
    print(f"  {label:<30}  PSNR={p:6.2f} dB   SSIM={s:.4f}")
    return p, s


def main():
    parser = argparse.ArgumentParser(description="PSNR/SSIM 비교 — Fresnel-Guided PT")
    parser.add_argument("--gt", required=True, help="Ground truth 이미지 (10000spp baseline)")
    parser.add_argument("--a",  required=True, help="이미지 A (baseline 저spp)")
    parser.add_argument("--b",  required=True, help="이미지 B (fresnel 저spp)")
    parser.add_argument("--percentile", type=float, default=70.0,
                        help="Fresnel 마스크 상위 밝기 퍼센타일 (기본 70)")
    parser.add_argument("--diff-scale", type=float, default=5.0,
                        help="차이 이미지 증폭 배율 (기본 5)")
    args = parser.parse_args()

    print("\n[compare.py] 이미지 로딩...")
    gt = load_rgb(args.gt)
    a  = load_rgb(args.a)
    b  = load_rgb(args.b)

    if gt.shape != a.shape or gt.shape != b.shape:
        print(f"[오류] 이미지 크기 불일치: gt={gt.shape}, a={a.shape}, b={b.shape}")
        sys.exit(1)

    print(f"  해상도: {gt.shape[1]}×{gt.shape[0]}")
    print(f"  GT    : {args.gt}")
    print(f"  A     : {args.a}")
    print(f"  B     : {args.b}\n")

    mask = luminance_mask(gt, args.percentile)
    mask_ratio = mask.mean() * 100
    print(f"[Fresnel 마스크] 상위 밝기 {args.percentile:.0f}% → 전체 픽셀의 {mask_ratio:.1f}% 해당\n")

    print("── 전체 이미지 ───────────────────────────────────────")
    pa_full, sa_full = report("A (baseline)", gt, a)
    pb_full, sb_full = report("B (fresnel) ", gt, b)
    delta_p_full = pb_full - pa_full
    delta_s_full = sb_full - sa_full
    print(f"  {'Δ (B−A)':30}  ΔPSNR={delta_p_full:+.2f} dB   ΔSSIM={delta_s_full:+.4f}")

    print("\n── Fresnel 마스크 영역 ───────────────────────────────")
    pa_mask, sa_mask = report("A (baseline)", gt, a, mask)
    pb_mask, sb_mask = report("B (fresnel) ", gt, b, mask)
    delta_p_mask = pb_mask - pa_mask
    delta_s_mask = sb_mask - sa_mask
    print(f"  {'Δ (B−A)':30}  ΔPSNR={delta_p_mask:+.2f} dB   ΔSSIM={delta_s_mask:+.4f}")

    print("\n── 차이 이미지 저장 ──────────────────────────────────")
    out_dir = Path(args.gt).parent
    diff_a_path = str(out_dir / "diff_baseline.png")
    diff_b_path = str(out_dir / "diff_fresnel.png")
    save_diff(gt, a, diff_a_path, args.diff_scale)
    save_diff(gt, b, diff_b_path, args.diff_scale)
    print(f"  {diff_a_path}")
    print(f"  {diff_b_path}")

    print("\n── 결론 ──────────────────────────────────────────────")
    if delta_p_full > 0:
        print(f"  ✅ Fresnel-guided가 전체 PSNR {delta_p_full:+.2f} dB 우수")
    else:
        print(f"  ❌ Fresnel-guided 전체 PSNR {delta_p_full:+.2f} dB (baseline 우세)")
    if delta_p_mask > 0:
        print(f"  ✅ Fresnel 마스크 영역 PSNR {delta_p_mask:+.2f} dB 우수")
    else:
        print(f"  ❌ Fresnel 마스크 영역 PSNR {delta_p_mask:+.2f} dB (baseline 우세)")
    print()


if __name__ == "__main__":
    main()
