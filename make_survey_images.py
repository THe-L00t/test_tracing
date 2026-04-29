"""
설문용 이미지 생성 스크립트
- BMP → PNG 변환 (sRGB, 8-bit)
- F(p) 맵 기반 자동 crop
- A | B 나란히 합성 (카운터밸런싱 2버전)
"""

from PIL import Image, ImageDraw, ImageFont
import numpy as np
import os

BASE = r"C:\Users\sigun\University\프로젝트\test_tracing\screenshots"
OUT  = r"C:\Users\sigun\University\프로젝트\test_tracing\survey_images"
os.makedirs(OUT, exist_ok=True)

PAD   = 80    # crop 여백 (px)
GAP   = 20    # A|B 사이 간격 (px)
LABEL_SIZE = 48  # "A" / "B" 폰트 크기
THRESH = 38   # F(p) 임계값 (0-255, 0.15 = 38)


def load(path):
    return Image.open(path).convert("RGB")


def find_crop_box(fmap_path, pad=PAD):
    """F(p) 맵에서 고F 영역 bounding box + padding 계산."""
    arr = np.array(Image.open(fmap_path).convert("RGB"))
    ch  = arr[:, :, 0]
    mask = ch > THRESH
    if mask.sum() == 0:
        return None
    rows = np.any(mask, axis=1)
    cols = np.any(mask, axis=0)
    r0, r1 = np.where(rows)[0][[0, -1]]
    c0, c1 = np.where(cols)[0][[0, -1]]
    h, w = ch.shape
    r0 = max(0, r0 - pad)
    r1 = min(h, r1 + pad)
    c0 = max(0, c0 - pad)
    c1 = min(w, c1 + pad)
    return (c0, r0, c1, r1)   # PIL crop 형식 (left, top, right, bottom)


def add_label(img, text, pos="TL"):
    """이미지 모서리에 A / B 레이블 추가 (복사본 반환)."""
    img = img.copy()
    draw = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("arial.ttf", LABEL_SIZE)
    except Exception:
        font = ImageFont.load_default()

    margin = 12
    if pos == "TL":
        x, y = margin, margin
    else:
        x, y = margin, margin

    # 배경 박스
    bbox = draw.textbbox((x, y), text, font=font)
    draw.rectangle([bbox[0]-6, bbox[1]-4, bbox[2]+6, bbox[3]+4], fill=(0, 0, 0, 200))
    draw.text((x, y), text, fill=(255, 255, 255), font=font)
    return img


def make_sbs(img_a, img_b, gap=GAP):
    """두 이미지를 좌우 나란히 합성 (흰 배경)."""
    w = img_a.width + gap + img_b.width
    h = max(img_a.height, img_b.height)
    canvas = Image.new("RGB", (w, h), (255, 255, 255))
    ya = (h - img_a.height) // 2
    yb = (h - img_b.height) // 2
    canvas.paste(img_a, (0, ya))
    canvas.paste(img_b, (img_a.width + gap, yb))
    return canvas


def save_png(img, path):
    img.save(path, format="PNG")
    print(f"  saved: {os.path.basename(path)}")


# ── 처리할 씬 설정 ─────────────────────────────────────────────
SCENES = {
    "scene5": {
        "crop": True,
        "pairs": [
            ("S5-A", "fresnel", "baseline"),
            ("S5-B", "fresnel", "variance"),
        ],
    },
    "scene8": {
        "crop": True,
        "pairs": [
            ("S8-A", "fresnel", "baseline"),
        ],
    },
    "scene7": {
        "crop": False,   # F(p) 전체 분포 → 전체 이미지
        "pairs": [
            ("S7-AC", "fresnel", "baseline"),
        ],
    },
}

FILE = {
    "fresnel":  "screenshot_fresnel_00100spp.bmp",
    "baseline": "screenshot_baseline_00100spp.bmp",
    "variance": "screenshot_variance_00100spp.bmp",
}

# ── 메인 처리 루프 ─────────────────────────────────────────────
for scene, cfg in SCENES.items():
    print(f"\n[{scene}]")
    scene_dir = os.path.join(BASE, scene)

    # F(p) crop box
    fmap_path = os.path.join(scene_dir, "screenshot_fmap_00100spp.bmp")
    box = None
    if cfg["crop"] and os.path.exists(fmap_path):
        box = find_crop_box(fmap_path)
        if box:
            print(f"  crop box: {box}  size: {box[2]-box[0]}x{box[3]-box[1]}")

    # 각 쌍 처리
    for pair_id, key_a, key_b in cfg["pairs"]:
        path_a = os.path.join(scene_dir, FILE[key_a])
        path_b = os.path.join(scene_dir, FILE[key_b])

        if not (os.path.exists(path_a) and os.path.exists(path_b)):
            print(f"  SKIP {pair_id}: 파일 없음")
            continue

        full_a = load(path_a)
        full_b = load(path_b)

        # ── 전체 이미지 PNG 저장 ──
        save_png(full_a, os.path.join(OUT, f"{pair_id}_{key_a}_full.png"))
        save_png(full_b, os.path.join(OUT, f"{pair_id}_{key_b}_full.png"))

        # ── 전체 이미지 A|B 합성 (AB / BA 2버전) ──
        la = add_label(full_a, "A")
        lb = add_label(full_b, "B")
        save_png(make_sbs(la, lb), os.path.join(OUT, f"{pair_id}_full_AB.png"))
        save_png(make_sbs(lb, la), os.path.join(OUT, f"{pair_id}_full_BA.png"))

        # ── crop 처리 ──
        if box:
            crop_a = full_a.crop(box)
            crop_b = full_b.crop(box)

            save_png(crop_a, os.path.join(OUT, f"{pair_id}_{key_a}_crop.png"))
            save_png(crop_b, os.path.join(OUT, f"{pair_id}_{key_b}_crop.png"))

            ca = add_label(crop_a, "A")
            cb = add_label(crop_b, "B")
            save_png(make_sbs(ca, cb), os.path.join(OUT, f"{pair_id}_crop_AB.png"))
            save_png(make_sbs(cb, ca), os.path.join(OUT, f"{pair_id}_crop_BA.png"))

print("\n완료. 출력 폴더:", OUT)
