"""
두 단 논문 삽입용 세로형 Figure 재생성
- 그림 1: F(p) map / Fresnel render 세로 2단
- 그림 2: Self-regulation scatter (세로형)
- 그림 3: Scene 8 비교 — 3행 2열 세로 배치
- 그림 4: GPU 타이밍 bar chart (세로형)
- 그림 5: 사용자 설문 bar chart (세로형)
"""

import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import matplotlib.patches as mpatches
import numpy as np
from PIL import Image
import os

plt.rcParams.update({
    'font.family': 'DejaVu Sans',
    'axes.spines.top': False,
    'axes.spines.right': False,
    'figure.dpi': 300,
})

S5 = r'screenshots\scene5'
S8 = r'screenshots\scene8'
os.makedirs('figures_paper', exist_ok=True)

# ─────────────────────────────────────────────────────────────
# 그림 1: Scene 5 — F(p) map (위) / Fresnel render (아래)
# ─────────────────────────────────────────────────────────────
fmap   = Image.open(os.path.join(S5, 'screenshot_fmap_00100spp.bmp'))
render = Image.open(os.path.join(S5, 'screenshot_fresnel_00100spp.bmp'))

fig, axes = plt.subplots(2, 1, figsize=(4.5, 7))
fig.patch.set_facecolor('#111')
fig.subplots_adjust(hspace=0.06, top=0.93, bottom=0.02, left=0.02, right=0.98)

axes[0].imshow(fmap)
axes[0].set_title('(a)  F(p) Priority Map', color='white', fontsize=10, pad=5)
axes[0].axis('off')
axes[0].text(0.01, 0.97, 'Low F(p)',  transform=axes[0].transAxes,
             color='#90CAF9', fontsize=8, va='top')
axes[0].text(0.99, 0.97, 'High F(p)', transform=axes[0].transAxes,
             color='white', fontsize=8, va='top', ha='right', fontweight='bold')

axes[1].imshow(render)
axes[1].set_title('(b)  Fresnel-Guided PT  (100 spp)', color='white', fontsize=10, pad=5)
axes[1].axis('off')

fig.suptitle('Scene 5 (Cornell Box, A = 0.2%)', color='white', fontsize=10, y=0.98)
plt.savefig('figures_paper/figure1_teaser.png', bbox_inches='tight',
            facecolor=fig.get_facecolor())
plt.close()
print("figure1_teaser.png saved")


# ─────────────────────────────────────────────────────────────
# 그림 2: Self-regulation scatter (세로형)
# ─────────────────────────────────────────────────────────────
A_pct = {'Scene 4': 29.1, 'Scene 5': 0.2, 'Scene 6': 0.0,
          'Scene 7': 29.1, 'Scene 8': 0.9}
pass2_f = {'Scene 4': 7.392, 'Scene 5': 0.562, 'Scene 6': 0.042,
           'Scene 7': 6.109, 'Scene 8': 0.522}
pass2_v = {'Scene 4': 17.287, 'Scene 5': 23.924, 'Scene 6': 11.611,
           'Scene 7':  9.075, 'Scene 8': 14.279}

label_offset = {
    'Scene 4': ( 5,  5),
    'Scene 5': ( 5, -11),
    'Scene 6': ( 5,  5),
    'Scene 7': (-52,  5),
    'Scene 8': ( 5,  5),
}

fig, ax = plt.subplots(figsize=(4.5, 6.5))
ax.axvspan(-0.5,  5.0, alpha=0.07, color='blue',  zorder=0)
ax.axvspan( 5.0, 32.0, alpha=0.07, color='red',   zorder=0)
ax.axvline(5.0, color='gray', linestyle=':', linewidth=1.0, zorder=1)
ax.text(2.5, 26, 'Envelope\n(A≲5%)',  ha='center', fontsize=7.5, color='#1565C0')
ax.text(18,  26, 'Outside\n(A>20%)', ha='center', fontsize=7.5, color='#B71C1C')

for scene in A_pct:
    ax.scatter(A_pct[scene], pass2_f[scene], color='#1976D2', s=70, zorder=5)
    ax.scatter(A_pct[scene], pass2_v[scene], color='#D32F2F', s=70, marker='s', zorder=5)
    ox, oy = label_offset.get(scene, (5, 5))
    ax.annotate(scene,
                (A_pct[scene], max(pass2_f[scene], pass2_v[scene])),
                textcoords='offset points', xytext=(ox, oy), fontsize=8)

ax.axhline(16.67, color='#2E7D32', linestyle='--', linewidth=1.3, zorder=3)

f_p = plt.Line2D([0],[0], marker='o', color='w', markerfacecolor='#1976D2',
                  markersize=8, label='Fresnel-guided Pass 2')
v_p = plt.Line2D([0],[0], marker='s', color='w', markerfacecolor='#D32F2F',
                  markersize=8, label='Variance-guided Pass 2')
b_p = plt.Line2D([0],[0], color='#2E7D32', linestyle='--', linewidth=1.3,
                  label='60 fps budget')
ax.legend(handles=[f_p, v_p, b_p], fontsize=8.5, loc='upper left')

ax.set_xlabel('High-F(p) Coverage  A (%)', fontsize=10)
ax.set_ylabel('Pass 2 Cost (ms)', fontsize=10)
ax.set_title('Self-Regulation:\nPass 2 Cost vs F(p) Coverage', fontsize=10)
ax.set_xlim(-0.5, 32)
ax.set_ylim(-0.5, 28)
ax.yaxis.set_minor_locator(ticker.MultipleLocator(2))
ax.grid(axis='y', which='major', alpha=0.3)

plt.tight_layout()
plt.savefig('figures_paper/figure2_self_regulation.png', bbox_inches='tight')
plt.close()
print("figure2_self_regulation.png saved")


# ─────────────────────────────────────────────────────────────
# 그림 3: Scene 8 비교 — 3행 2열 (렌더 | diff)
#   Row 0: Baseline 100spp        | diff_baseline
#   Row 1: Fresnel-Guided 100spp  | diff_fresnel
#   Row 2: Reference 10000spp     | 수치 요약
# ─────────────────────────────────────────────────────────────
base100 = Image.open(os.path.join(S8, 'screenshot_baseline_00100spp.bmp'))
fres100 = Image.open(os.path.join(S8, 'screenshot_fresnel_00100spp.bmp'))
ref10k  = Image.open(os.path.join(S8, 'screenshot_baseline_10000spp.bmp'))
diff_b  = Image.open(os.path.join(S8, 'diff_baseline.png'))
diff_f  = Image.open(os.path.join(S8, 'diff_fresnel.png'))

fig, axes = plt.subplots(3, 2, figsize=(5.5, 8.5))
fig.patch.set_facecolor('#111')
fig.subplots_adjust(hspace=0.08, wspace=0.04,
                    top=0.93, bottom=0.02, left=0.02, right=0.98)

row_data = [
    (base100, diff_b,  'Baseline  (100 spp)',       'Error — Baseline'),
    (fres100, diff_f,  'Fresnel-Guided  (100 spp)', 'Error — Fresnel'),
    (ref10k,  None,    'Reference  (10 000 spp)',   ''),
]
for r, (img_l, img_r, t_l, t_r) in enumerate(row_data):
    axes[r][0].imshow(img_l)
    axes[r][0].set_title(t_l, color='white', fontsize=8.5, pad=4)
    axes[r][0].axis('off')
    if img_r is not None:
        axes[r][1].imshow(img_r)
        axes[r][1].set_title(t_r, color='#FFCC80', fontsize=8.5, pad=4)
        axes[r][1].axis('off')
    else:
        axes[r][1].set_facecolor('#111')
        axes[r][1].axis('off')
        summary = (
            "Scene 8  (A = 0.9%)\n\n"
            "wPSNR  Δ\n"
            "  Fresnel :  +0.81 dB\n"
            "  Variance:  +0.74 dB\n\n"
            "Pass 2 Cost\n"
            "  Fresnel :   0.52 ms\n"
            "  Variance:  14.28 ms"
        )
        axes[r][1].text(0.5, 0.5, summary, transform=axes[r][1].transAxes,
                        color='white', fontsize=8.5, va='center', ha='center',
                        fontfamily='monospace',
                        bbox=dict(boxstyle='round,pad=0.5',
                                  facecolor='#222', edgecolor='#555'))

fig.suptitle('Scene 8 (Metal + Glass, A = 0.9%) — Quality Comparison',
             color='white', fontsize=9.5, y=0.97)
plt.savefig('figures_paper/figure3_comparison.png', bbox_inches='tight',
            facecolor=fig.get_facecolor())
plt.close()
print("figure3_comparison.png saved")


# ─────────────────────────────────────────────────────────────
# 그림 4: GPU 타이밍 bar chart (세로형)
# ─────────────────────────────────────────────────────────────
scenes  = ['Scene 4', 'Scene 5', 'Scene 6', 'Scene 7', 'Scene 8']
f_mean  = [8.032,  4.309, 12.973, 5.234, 12.992]
f_std   = [0.170,  0.162,  0.934, 0.167,  0.907]
v_mean  = [71.165, 29.493, 13.601, 45.143, 42.528]
v_std   = [5.474,  2.741,  0.977,  1.117,  0.924]

x     = np.arange(len(scenes))
width = 0.35

fig, ax = plt.subplots(figsize=(4.5, 6.5))
ax.bar(x - width/2, f_mean, width, yerr=f_std, capsize=4,
       label='Fresnel-guided', color='#1976D2', zorder=3)
ax.bar(x + width/2, v_mean, width, yerr=v_std, capsize=4,
       label='Variance-guided', color='#D32F2F', zorder=3)
ax.axhline(16.67, color='#2E7D32', linestyle='--', linewidth=1.5,
           label='60 fps  (16.67 ms)', zorder=4)

for i, (xpos, fm, vm) in enumerate(zip(x, f_mean, v_mean)):
    ratio = vm / fm
    top   = max(fm + f_std[i], vm + v_std[i]) + 2.5
    ax.text(xpos, top, f'{ratio:.1f}×', ha='center', va='bottom',
            fontsize=8, color='#555', fontweight='bold')

ax.set_xlabel('Scene', fontsize=10)
ax.set_ylabel('Frame Time (ms)', fontsize=10)
ax.set_title('GPU Frame Timing:\nFresnel-guided vs Variance-guided\n(10-frame mean ± σ)', fontsize=10)
ax.set_xticks(x)
ax.set_xticklabels(scenes, fontsize=9)
ax.set_ylim(0, 95)
ax.yaxis.set_minor_locator(ticker.MultipleLocator(5))
ax.grid(axis='y', which='major', alpha=0.3, zorder=0)
ax.legend(fontsize=9, loc='upper right')

plt.tight_layout()
plt.savefig('figures_paper/figure4_gpu_timing.png', bbox_inches='tight')
plt.close()
print("figure4_gpu_timing.png saved")


# ─────────────────────────────────────────────────────────────
# 그림 5: 사용자 설문 bar chart (세로형)
# ─────────────────────────────────────────────────────────────
conditions  = ['Scene 5\nF vs Baseline', 'Scene 5\nF vs Variance',
               'Scene 7\nF vs Baseline', 'Scene 8\nF vs Baseline']
fresnel_pct = [69.2, 38.2, 47.1, 59.1]
other_pct   = [30.8, 61.8, 52.9, 40.9]
p_vals      = [0.038, 0.085, 0.597, 0.197]
decisive_n  = [26,    34,    17,    22]

x     = np.arange(len(conditions))
width = 0.35

fig, ax = plt.subplots(figsize=(4.5, 6.5))
ax.bar(x - width/2, fresnel_pct, width,
       label='Fresnel-guided preferred', color='#1976D2', zorder=3)
ax.bar(x + width/2, other_pct,   width,
       label='Counterpart preferred',    color='#D32F2F', zorder=3)
ax.axhline(50, color='gray', linestyle='--', linewidth=1.3,
           label='Chance (50%)', zorder=4)

for i, (xpos, pv, n, fp, op) in enumerate(
        zip(x, p_vals, decisive_n, fresnel_pct, other_pct)):
    sig = ' *' if pv < 0.05 else ''
    top = max(fp, op) + 2.5
    ax.text(xpos, top, f'p={pv}{sig}\n(n={n})',
            ha='center', va='bottom', fontsize=7.5, color='#333')

ax.axvspan(-0.5, 0.5, alpha=0.06, color='blue')
ax.axvspan( 0.5, 1.5, alpha=0.06, color='blue')
ax.axvspan( 1.5, 2.5, alpha=0.06, color='red')
ax.axvspan( 2.5, 3.5, alpha=0.06, color='blue')

ax.set_xlabel('Experimental Condition', fontsize=10)
ax.set_ylabel('Preference (%)', fontsize=10)
ax.set_title('2AFC User Study Results\n(n=40, decisive responses only)', fontsize=10)
ax.set_xticks(x)
ax.set_xticklabels(conditions, fontsize=8.5)
ax.set_ylim(0, 90)
ax.grid(axis='y', alpha=0.3, zorder=0)
ax.legend(fontsize=8.5, loc='upper right')

plt.tight_layout()
plt.savefig('figures_paper/figure5_user_study.png', bbox_inches='tight')
plt.close()
print("figure5_user_study.png saved")

print("\nAll figures saved to figures_paper/")
