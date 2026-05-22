"""
논문 Figure 1, 3 생성 스크립트
- 그림 1: Scene 5 — F(p) map + Fresnel-guided render (teaser)
- 그림 3: Scene 8 — Baseline / Fresnel / Reference + diff 비교
"""

import matplotlib.pyplot as plt
import matplotlib.patches as patches
import numpy as np
from PIL import Image
import os

plt.rcParams.update({
    'font.family': 'DejaVu Sans',
    'figure.dpi': 150,
})

S5 = r'screenshots\scene5'
S8 = r'screenshots\scene8'


# ─────────────────────────────────────────────────────────────
# 그림 1: Scene 5 teaser — F(p) map | Fresnel render
# ─────────────────────────────────────────────────────────────
fmap   = Image.open(os.path.join(S5, 'screenshot_fmap_00100spp.bmp'))
render = Image.open(os.path.join(S5, 'screenshot_fresnel_00100spp.bmp'))

fig, axes = plt.subplots(1, 2, figsize=(13, 5))
fig.patch.set_facecolor('#111')

titles = ['(a)  F(p) Priority Map', '(b)  Fresnel-Guided PT  (100 spp)']
imgs   = [fmap, render]
for ax, img, title in zip(axes, imgs, titles):
    ax.imshow(img)
    ax.set_title(title, color='white', fontsize=12, pad=8)
    ax.axis('off')

# F(p) map에 컬러바 느낌 텍스트 추가
axes[0].text(0.01, 0.99, 'Low F(p)', transform=axes[0].transAxes,
             color='#90CAF9', fontsize=9, va='top', ha='left')
axes[0].text(0.99, 0.99, 'High F(p)', transform=axes[0].transAxes,
             color='white', fontsize=9, va='top', ha='right', fontweight='bold')

fig.suptitle('Scene 5 (Cornell Box, A = 0.2%) — Fresnel Priority Map & Result',
             color='white', fontsize=13, y=1.01)

plt.tight_layout(pad=0.8)
plt.savefig('figure1_teaser.png', bbox_inches='tight',
            facecolor=fig.get_facecolor())
plt.close()
print("figure1_teaser.png saved")


# ─────────────────────────────────────────────────────────────
# 그림 3: Scene 8 비교 — 2행 3열
#   Row 0: Baseline 100spp | Fresnel 100spp | Reference 10000spp
#   Row 1: diff Baseline   | diff Fresnel   | (차이 설명 텍스트)
# ─────────────────────────────────────────────────────────────
base100  = Image.open(os.path.join(S8, 'screenshot_baseline_00100spp.bmp'))
fres100  = Image.open(os.path.join(S8, 'screenshot_fresnel_00100spp.bmp'))
ref10k   = Image.open(os.path.join(S8, 'screenshot_baseline_10000spp.bmp'))
diff_b   = Image.open(os.path.join(S8, 'diff_baseline.png'))
diff_f   = Image.open(os.path.join(S8, 'diff_fresnel.png'))

fig, axes = plt.subplots(2, 3, figsize=(15, 8))
fig.patch.set_facecolor('#111')

row0_imgs   = [base100,  fres100,  ref10k]
row0_titles = ['Baseline  (100 spp)', 'Fresnel-Guided  (100 spp)', 'Reference  (10 000 spp)']
row1_imgs   = [diff_b,   diff_f,   None]
row1_titles = ['Error Map — Baseline', 'Error Map — Fresnel-Guided', '']

for col, (img, title) in enumerate(zip(row0_imgs, row0_titles)):
    ax = axes[0][col]
    ax.imshow(img)
    ax.set_title(title, color='white', fontsize=11, pad=6)
    ax.axis('off')

for col, (img, title) in enumerate(zip(row1_imgs, row1_titles)):
    ax = axes[1][col]
    if img is not None:
        ax.imshow(img)
        ax.set_title(title, color='#FFCC80', fontsize=10, pad=6)
        ax.axis('off')
    else:
        ax.set_facecolor('#111')
        ax.axis('off')
        # 수치 요약 텍스트
        summary = (
            "Scene 8  (A = 0.9%)\n\n"
            "wPSNR  Δ\n"
            "  Fresnel-Guided :  +0.81 dB\n"
            "  Variance-Guided:  +0.74 dB\n\n"
            "Pass 2 Cost\n"
            "  Fresnel-Guided :   0.52 ms\n"
            "  Variance-Guided:  14.28 ms"
        )
        ax.text(0.5, 0.5, summary, transform=ax.transAxes,
                color='white', fontsize=10, va='center', ha='center',
                fontfamily='monospace',
                bbox=dict(boxstyle='round,pad=0.6', facecolor='#222', edgecolor='#555'))

fig.suptitle('Scene 8 (Metal + Glass, A = 0.9%) — Quality Comparison',
             color='white', fontsize=13, y=1.01)

plt.tight_layout(pad=0.6)
plt.savefig('figure3_comparison.png', bbox_inches='tight',
            facecolor=fig.get_facecolor())
plt.close()
print("figure3_comparison.png saved")
