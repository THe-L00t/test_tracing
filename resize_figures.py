"""
두 단(two-column) 논문 삽입용 이미지 리사이즈
300 DPI 기준:
  두 단 전체(170mm): 2008 px
  한 단(82mm):        969 px
"""

from PIL import Image
import os

# (파일명, 목표 width px, 목표 height px)
# height=None 이면 비율 유지
targets = [
    ('figure1_teaser.png',           2008,  620),   # 두 단, 가로 긴 teaser
    ('figure2_self_regulation.png',   969,  780),   # 한 단, 거의 정방형
    ('figure3_comparison.png',       2008, 1100),   # 두 단, 2행 그리드
    ('figure4_gpu_timing.png',       2008,  780),   # 두 단 bar chart
    ('figure5_user_study.png',       2008,  780),   # 두 단 bar chart
]

OUT_DIR = 'figures_paper'
os.makedirs(OUT_DIR, exist_ok=True)

for fname, tw, th in targets:
    if not os.path.exists(fname):
        print(f'SKIP (not found): {fname}')
        continue
    img = Image.open(fname).convert('RGBA')
    resized = img.resize((tw, th), Image.LANCZOS)
    # 흰 배경 합성 (BMP/PDF 삽입 시 투명도 문제 방지)
    bg = Image.new('RGB', (tw, th), (255, 255, 255))
    bg.paste(resized, mask=resized.split()[3] if resized.mode == 'RGBA' else None)
    out_path = os.path.join(OUT_DIR, fname)
    bg.save(out_path, dpi=(300, 300))
    print(f'{fname}  ->  {tw}x{th}  saved')

print(f'\n완료: ./{OUT_DIR}/')
