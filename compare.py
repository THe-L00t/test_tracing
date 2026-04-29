"""
compare.py  —  PSNR / SSIM / wPSNR 비교 스크립트
Fresnel-Guided PT 연구용

사용법:
    python compare.py --gt <gt.bmp> --a <baseline.bmp> --b <fresnel.bmp> [--c <variance.bmp>]
                      [--fmap <fresnel_map.bmp>]

예시:
    python compare.py ^
        --gt screenshot_baseline_10000spp.bmp ^
        --a  screenshot_baseline_00100spp.bmp ^
        --b  screenshot_fresnel_00100spp.bmp ^
        --c  screenshot_variance_00100spp.bmp ^
        --fmap screenshot_fmap.bmp

출력:
    - 전체 이미지 PSNR / SSIM
    - Fresnel 마스크 영역(고F(p)) PSNR / SSIM
    - 비-Fresnel 영역(저F(p)) PSNR / SSIM  ← regression 검증용
    - Weighted PSNR (wPSNR) — F(p) 연속 가중치 적용
    - 차이 이미지 저장 (diff_a.png, diff_b.png [, diff_c.png])

--fmap 미지정 시: GT 휘도를 연속 가중치로 사용 (근사)
--fmap 지정 시:  실제 F(p) 맵 이미지를 가중치+마스크로 사용
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


def luminance(img: np.ndarray) -> np.ndarray:
    """(H,W,3) -> (H,W) 휘도"""
    return 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]


def psnr(ref: np.ndarray, tst: np.ndarray) -> float:
    mse = np.mean((ref - tst) ** 2)
    if mse < 1e-10:
        return float("inf")
    return 10.0 * np.log10(1.0 / mse)


def wpsnr(ref: np.ndarray, tst: np.ndarray, weight: np.ndarray) -> float:
    """Weighted PSNR — 픽셀별 중요도 가중치 적용.

    weight: (H,W) 형태의 비음수 중요도 맵 (정규화 불필요).
    수식: wPSNR = -10*log10( Σ w*(ref-tst)² / Σ w )
    """
    w_sum = weight.sum()
    if w_sum < 1e-10:
        return psnr(ref, tst)  # fallback
    w_norm = weight / w_sum
    wmse = float(np.sum(w_norm[..., None] * (ref - tst) ** 2))
    if wmse < 1e-10:
        return float("inf")
    return 10.0 * np.log10(1.0 / wmse)


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
        C1, C2 = (0.01 ** 2), (0.03 ** 2)
        mu1, mu2 = ref.mean(), tst.mean()
        s1, s2, s12 = ref.std(), tst.std(), np.mean((ref - mu1) * (tst - mu2))
        return float(((2*mu1*mu2 + C1)*(2*s12 + C2)) /
                     ((mu1**2 + mu2**2 + C1)*(s1**2 + s2**2 + C2)))


def luminance_mask(img: np.ndarray, percentile: float = 70.0) -> np.ndarray:
    """밝기 기준 상위 percentile 픽셀 마스크 — Fresnel 강조 영역 근사"""
    lum = luminance(img)
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
        ref_masked = ref * mask[..., None]
        tst_masked = tst * mask[..., None]
        s = ssim(ref_masked, tst_masked)
    else:
        p = psnr(ref, tst)
        s = ssim(ref, tst)
    print(f"  {label:<30}  PSNR={p:6.2f} dB   SSIM={s:.4f}")
    return p, s


def main():
    parser = argparse.ArgumentParser(description="PSNR/SSIM/wPSNR 비교 — Fresnel-Guided PT")
    parser.add_argument("--gt", required=True, help="Ground truth 이미지 (10000spp baseline)")
    parser.add_argument("--a",  required=True, help="이미지 A (baseline 저spp)")
    parser.add_argument("--b",  required=True, help="이미지 B (fresnel-guided 저spp)")
    parser.add_argument("--c",  default=None,  help="이미지 C (variance-guided 또는 fair-baseline, 선택)")
    parser.add_argument("--c-label", default="C (variance)", help="--c 이미지 레이블 (기본: 'C (variance)')")
    parser.add_argument("--fmap", default=None,
                        help="F(p) 맵 이미지 (debugMode=1 스크린샷). 미지정 시 GT 휘도로 근사.")
    parser.add_argument("--fmap-threshold", type=float, default=0.15,
                        help="fmap 기반 이진 마스크 임계값 (기본 0.15)")
    parser.add_argument("--percentile", type=float, default=70.0,
                        help="휘도 마스크 상위 퍼센타일 (fmap 없을 때 사용, 기본 70)")
    parser.add_argument("--diff-scale", type=float, default=5.0,
                        help="차이 이미지 증폭 배율 (기본 5)")
    args = parser.parse_args()

    print("\n[compare.py] 이미지 로딩...")
    gt = load_rgb(args.gt)
    a  = load_rgb(args.a)
    b  = load_rgb(args.b)
    c  = load_rgb(args.c) if args.c else None

    for name, img in [("a", a), ("b", b)] + ([("c", c)] if c is not None else []):
        if gt.shape != img.shape:
            print(f"[오류] 이미지 크기 불일치: gt={gt.shape}, {name}={img.shape}")
            sys.exit(1)

    print(f"  해상도: {gt.shape[1]}×{gt.shape[0]}")
    print(f"  GT    : {args.gt}")
    print(f"  A     : {args.a}")
    print(f"  B     : {args.b}")
    if c is not None:
        print(f"  C     : {args.c}")
    print()

    # ── F(p) 가중치 맵 및 이진 마스크 구성 ──────────────────────
    if args.fmap:
        fmap_img = load_rgb(args.fmap)
        if fmap_img.shape[:2] != gt.shape[:2]:
            print(f"[오류] fmap 크기 불일치: gt={gt.shape[:2]}, fmap={fmap_img.shape[:2]}")
            sys.exit(1)
        weight = luminance(fmap_img)          # F(p) 맵의 휘도 → 중요도
        mask   = weight >= args.fmap_threshold
        mask_label = f"실제 F(p) 맵 (임계값 {args.fmap_threshold:.2f})"
    else:
        weight = luminance(gt)                # GT 휘도 → 연속 가중치 (근사)
        weight = weight / (weight.max() + 1e-10)
        mask   = luminance_mask(gt, args.percentile)
        mask_label = f"GT 휘도 근사 (상위 {args.percentile:.0f}%)"

    non_mask = ~mask
    print(f"[마스크] {mask_label}")
    print(f"  고F(p) {mask.mean()*100:.1f}%  /  저F(p) {non_mask.mean()*100:.1f}%\n")

    c_label = args.c_label

    print("── 전체 이미지 ───────────────────────────────────────")
    pa_full, sa_full = report("A (baseline)  ", gt, a)
    pb_full, sb_full = report("B (fresnel)   ", gt, b)
    if c is not None:
        pc_full, sc_full = report(f"{c_label:<16}", gt, c)
    delta_p_full = pb_full - pa_full
    delta_s_full = sb_full - sa_full
    print(f"  {'Δ B−A':30}  ΔPSNR={delta_p_full:+.2f} dB   ΔSSIM={delta_s_full:+.4f}")
    if c is not None:
        print(f"  {'Δ C−A':30}  ΔPSNR={pc_full - pa_full:+.2f} dB   ΔSSIM={sc_full - sa_full:+.4f}")

    print("\n── Fresnel 마스크 영역 (고F(p)) ──────────────────────")
    pa_mask, sa_mask = report("A (baseline)  ", gt, a, mask)
    pb_mask, sb_mask = report("B (fresnel)   ", gt, b, mask)
    if c is not None:
        pc_mask, sc_mask = report(f"{c_label:<16}", gt, c, mask)
    delta_p_mask = pb_mask - pa_mask
    delta_s_mask = sb_mask - sa_mask
    print(f"  {'Δ B−A':30}  ΔPSNR={delta_p_mask:+.2f} dB   ΔSSIM={delta_s_mask:+.4f}")
    if c is not None:
        print(f"  {'Δ C−A':30}  ΔPSNR={pc_mask - pa_mask:+.2f} dB   ΔSSIM={sc_mask - sa_mask:+.4f}")

    print("\n── 비-Fresnel 영역 (저F(p)) — regression 검증 ───────")
    pa_non, sa_non = report("A (baseline)  ", gt, a, non_mask)
    pb_non, sb_non = report("B (fresnel)   ", gt, b, non_mask)
    if c is not None:
        pc_non, sc_non = report(f"{c_label:<16}", gt, c, non_mask)
    delta_p_non = pb_non - pa_non
    print(f"  {'Δ B−A':30}  ΔPSNR={delta_p_non:+.2f} dB   ΔSSIM={pb_non - pa_non:+.4f}")
    if c is not None:
        print(f"  {'Δ C−A':30}  ΔPSNR={pc_non - pa_non:+.2f} dB   ΔSSIM={sc_non - sa_non:+.4f}")

    # ── Weighted PSNR ────────────────────────────────────────────
    print(f"\n── Weighted PSNR (wPSNR) — {mask_label} ──")
    wa = wpsnr(gt, a, weight)
    wb = wpsnr(gt, b, weight)
    print(f"  {'A (baseline)':30}  wPSNR={wa:6.2f} dB")
    print(f"  {'B (fresnel)':30}  wPSNR={wb:6.2f} dB")
    delta_wp = wb - wa
    print(f"  {'Δ B−A':30}  ΔwPSNR={delta_wp:+.2f} dB  {'✅' if delta_wp > 0 else '❌'}")
    wc = None
    if c is not None:
        wc = wpsnr(gt, c, weight)
        print(f"  {c_label:<30}  wPSNR={wc:6.2f} dB")
        print(f"  {'Δ C−A':30}  ΔwPSNR={wc - wa:+.2f} dB  {'✅' if wc - wa > 0 else '❌'}")

    print("\n── 차이 이미지 저장 ──────────────────────────────────")
    out_dir = Path(args.gt).parent
    for label, img in [("baseline", a), ("fresnel", b)] + ([("variance", c)] if c is not None else []):
        path = str(out_dir / f"diff_{label}.png")
        save_diff(gt, img, path, args.diff_scale)
        print(f"  {path}")

    print("\n── 결론 ──────────────────────────────────────────────")
    print(f"  전체   B−A: ΔPSNR  {delta_p_full:+.2f} dB  "
          f"{'✅' if delta_p_full > 0 else '❌'}")
    print(f"  고F(p) B−A: ΔPSNR  {delta_p_mask:+.2f} dB  "
          f"{'✅' if delta_p_mask > 0 else '❌'}")
    print(f"  저F(p) B−A: ΔPSNR  {delta_p_non:+.2f} dB  "
          f"{'✅ (regression 없음)' if delta_p_non >= -0.05 else '⚠️ regression 확인 필요'}")
    print(f"  wPSNR  B−A: ΔwPSNR {delta_wp:+.2f} dB  "
          f"{'✅' if delta_wp > 0 else '❌'}")
    print()

    # 수치 딕셔너리 반환 (다른 스크립트에서 import 시 활용)
    return {
        "psnr_full":  {"A": pa_full,  "B": pb_full,  "dBA": delta_p_full},
        "psnr_mask":  {"A": pa_mask,  "B": pb_mask,  "dBA": delta_p_mask},
        "psnr_non":   {"A": pa_non,   "B": pb_non,   "dBA": delta_p_non},
        "wpsnr":      {"A": wa,       "B": wb,       "dBA": delta_wp},
        "ssim_full":  {"A": sa_full,  "B": sb_full,  "dBA": delta_s_full},
    }


if __name__ == "__main__":
    main()
