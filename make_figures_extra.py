"""
추천 비주얼 4종 생성
  extra1_render_comparison.png  — 4열 비교: Baseline|Variance|Fresnel|GT (Scene 5)
  extra2_pass2_timing.png       — Pass 2 타이밍 Bar Chart (5씬, 60fps 기준선)
  extra3_wpsnr.png              — wPSNR 개선량 Bar Chart (씬별 ΔdB)
  extra4_user_study.png         — 사용자 설문 Bar Chart (p-value, 유의/비유의 구분)
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
os.makedirs('figures_paper', exist_ok=True)


# ═════════════════════════════════════════════════════════════
# Extra 1: Scene 5 — 4열 렌더링 비교
#   Baseline (100spp) | Variance-Guided (100spp) | Fresnel-Guided (100spp) | GT (10000spp)
# ═════════════════════════════════════════════════════════════
img_base = Image.open(os.path.join(S5, 'screenshot_baseline_00100spp.bmp'))
img_var  = Image.open(os.path.join(S5, 'screenshot_variance_00100spp.bmp'))
img_fres = Image.open(os.path.join(S5, 'screenshot_fresnel_00100spp.bmp'))
img_gt   = Image.open(os.path.join(S5, 'screenshot_baseline_10000spp.bmp'))

fig, axes = plt.subplots(1, 4, figsize=(18, 4.5))
fig.patch.set_facecolor('#111')
fig.subplots_adjust(wspace=0.04, top=0.86, bottom=0.03, left=0.01, right=0.99)

panels = [
    (img_base, '(a)  Baseline\n(100 spp)',         'white'),
    (img_var,  '(b)  Variance-Guided\n(100 spp)',  '#FFCC80'),
    (img_fres, '(c)  Fresnel-Guided\n(100 spp)',   '#90CAF9'),
    (img_gt,   '(d)  Reference\n(10 000 spp)',     '#A5D6A7'),
]
for ax, (img, title, color) in zip(axes, panels):
    ax.imshow(img)
    ax.set_title(title, color=color, fontsize=10, pad=5, linespacing=1.5)
    ax.axis('off')

fig.suptitle('Scene 5 (Cornell Box, A = 0.2%)  —  Rendering Quality Comparison at Equal Sample Budget',
             color='white', fontsize=11, y=0.98)
plt.savefig('figures_paper/extra1_render_comparison.png', bbox_inches='tight',
            facecolor=fig.get_facecolor())
plt.close()
print("extra1_render_comparison.png saved")


# ═════════════════════════════════════════════════════════════
# Extra 2: Pass 2 타이밍 Bar Chart
#   5씬 × Fresnel/Variance, 60fps 기준선, 배율 표기
# ═════════════════════════════════════════════════════════════
scenes = ['Scene 4', 'Scene 5', 'Scene 6', 'Scene 7', 'Scene 8']
A_pct  = [29.1, 0.2, 0.0, 29.1, 0.9]   # Envelope 여부 판단용

f_mean = [7.392, 0.562, 0.042, 6.109, 0.522]
f_std  = [0.310, 0.065, 0.007, 0.280, 0.048]
v_mean = [17.287, 23.924, 11.611, 9.075, 14.279]
v_std  = [ 1.520,  2.683,  0.870, 0.890,  1.150]

x     = np.arange(len(scenes))
width = 0.35

fig, ax = plt.subplots(figsize=(11, 5.5))

# Envelope(A≤5%) vs Outside 색상 구분
f_colors = ['#1565C0' if a <= 5 else '#5C9BD6' for a in A_pct]
v_colors = ['#B71C1C' if a <= 5 else '#E57373' for a in A_pct]

bars_f = ax.bar(x - width/2, f_mean, width, yerr=f_std, capsize=5,
                color=f_colors, zorder=3, label='_nolegend_')
bars_v = ax.bar(x + width/2, v_mean, width, yerr=v_std, capsize=5,
                color=v_colors, zorder=3, label='_nolegend_')

ax.axhline(16.67, color='#2E7D32', linestyle='--', linewidth=1.8,
           label='60 fps budget  (16.67 ms)', zorder=4)

# 배율 표기
for i, (xpos, fm, vm, fs, vs) in enumerate(zip(x, f_mean, v_mean, f_std, v_std)):
    ratio = vm / fm
    top   = max(fm + fs, vm + vs) + 1.2
    ax.text(xpos, top, f'{ratio:.1f}×', ha='center', va='bottom',
            fontsize=9, color='#333', fontweight='bold')

# 범례 수동 구성
leg_handles = [
    mpatches.Patch(color='#1565C0', label='Fresnel-guided  (A ≤ 5%, envelope)'),
    mpatches.Patch(color='#5C9BD6', label='Fresnel-guided  (A > 5%, outside)'),
    mpatches.Patch(color='#B71C1C', label='Variance-guided  (A ≤ 5%)'),
    mpatches.Patch(color='#E57373', label='Variance-guided  (A > 5%)'),
    plt.Line2D([0],[0], color='#2E7D32', linestyle='--', linewidth=1.8,
               label='60 fps budget  (16.67 ms)'),
]
ax.legend(handles=leg_handles, fontsize=9, loc='upper right',
          framealpha=0.9, edgecolor='#ccc')

ax.set_xlabel('Scene', fontsize=11)
ax.set_ylabel('Pass 2 Cost (ms)', fontsize=11)
ax.set_title('Pass 2 Cost: Fresnel-guided vs Variance-guided\n'
             '(10-frame mean ± σ,  ratio = Variance / Fresnel)',
             fontsize=11)
ax.set_xticks(x)
ax.set_xticklabels(
    [f'{s}\n(A={a}%)' for s, a in zip(scenes, A_pct)], fontsize=9.5)
ax.set_ylim(0, 35)
ax.yaxis.set_minor_locator(ticker.MultipleLocator(2))
ax.grid(axis='y', which='major', alpha=0.3, zorder=0)

plt.tight_layout()
plt.savefig('figures_paper/extra2_pass2_timing.png', bbox_inches='tight')
plt.close()
print("extra2_pass2_timing.png saved")


# ═════════════════════════════════════════════════════════════
# Extra 3: wPSNR 개선량 수평 Bar Chart  (논문 표 2 실제 데이터)
#   수평 배치로 스케일 차이(-0.09 ~ +4.86) 시각화 문제 해결
# ═════════════════════════════════════════════════════════════
wp_scenes = ['Scene 5\n(A=0.2%, Envelope)',
             'Scene 8\n(A=0.9%, Envelope)',
             'Scene 7\n(A=29.1%, Outside)',
             'Scene 4\n(A=29.1%, Outside)']
wp_delta  = [+4.86, +1.62, -0.30, -0.09]
wp_env    = [True,  True,  False, False]

wp_colors = []
for dv, env in zip(wp_delta, wp_env):
    if dv >= 0:
        wp_colors.append('#1565C0' if env else '#5C9BD6')
    else:
        wp_colors.append('#C62828' if not env else '#EF9A9A')

y = np.arange(len(wp_scenes))

fig, ax = plt.subplots(figsize=(10, 5))
fig.subplots_adjust(left=0.28, right=0.82, top=0.88, bottom=0.18)

ax.barh(y, wp_delta, height=0.5, color=wp_colors, zorder=3)
ax.axvline(0, color='black', linewidth=1.2, zorder=2)

for ypos, dv, env in zip(y, wp_delta, wp_env):
    sign = '+' if dv >= 0 else ''
    mark = '  ✓' if (dv > 0 and env) else ''
    xoff = 0.10 if dv >= 0 else -0.10
    ha   = 'left'  if dv >= 0 else 'right'
    col  = '#0D47A1' if (dv > 0 and env) else '#333'
    fw   = 'bold' if env else 'normal'
    ax.text(dv + xoff, ypos, f'{sign}{dv:.2f} dB{mark}',
            va='center', ha=ha, fontsize=10.5, color=col, fontweight=fw)

ax.axvline(1.23, color='#2E7D32', linewidth=1.5, linestyle='--', zorder=4)
ax.text(1.23 + 0.08, y[-1] - 0.42, 'Avg.\n+1.23 dB',
        va='top', ha='left', fontsize=8.5, color='#2E7D32')

ax.set_yticks(y)
ax.set_yticklabels(wp_scenes, fontsize=10)
ax.set_xlabel('Δ wPSNR  [Fresnel − Baseline]  (dB)  ↑ better', fontsize=10.5)
ax.set_title('wPSNR Improvement: Fresnel-guided vs Baseline  (F(p)-weighted)', fontsize=11)
ax.set_xlim(-1.2, 6.5)
ax.xaxis.set_minor_locator(ticker.MultipleLocator(0.5))
ax.grid(axis='x', which='major', alpha=0.3, zorder=0)

leg_handles = [
    mpatches.Patch(color='#1565C0', label='Positive Δ  (Envelope, A ≤ 5%)'),
    mpatches.Patch(color='#C62828', label='Negative Δ  (Outside,  A > 5%)'),
    plt.Line2D([0],[0], color='#2E7D32', linestyle='--', linewidth=1.5,
               label='4-scene average  (+1.23 dB)'),
]
ax.legend(handles=leg_handles, fontsize=9.5, ncol=3,
          loc='upper center', bbox_to_anchor=(0.5, -0.18),
          framealpha=0.9, edgecolor='#ccc')

plt.savefig('figures_paper/extra3_wpsnr.png', bbox_inches='tight')
plt.close()
print("extra3_wpsnr.png saved")


# ═════════════════════════════════════════════════════════════
# Extra 4: 사용자 설문 Bar Chart (유의/비유의 구분 강화)
# ═════════════════════════════════════════════════════════════
conditions  = ['Scene 5\nF vs Baseline', 'Scene 5\nF vs Variance',
               'Scene 7\nF vs Baseline', 'Scene 8\nF vs Baseline']
fresnel_pct = [69.2, 38.2, 47.1, 59.1]
other_pct   = [30.8, 61.8, 52.9, 40.9]
p_vals      = [0.038, 0.085, 0.597, 0.197]
decisive_n  = [26,    34,    17,    22]
sig         = [p < 0.05 for p in p_vals]

x     = np.arange(len(conditions))
width = 0.35

fig, ax = plt.subplots(figsize=(11, 5.5))

# 유의 조건은 진한 색, 비유의는 연한 색
f_colors = ['#1565C0' if s else '#90CAF9' for s in sig]
o_colors = ['#B71C1C' if s else '#EF9A9A' for s in sig]

for i, (xpos, fp, op, fc, oc) in enumerate(
        zip(x, fresnel_pct, other_pct, f_colors, o_colors)):
    ax.bar(xpos - width/2, fp, width, color=fc, zorder=3)
    ax.bar(xpos + width/2, op, width, color=oc, zorder=3)

ax.axhline(50, color='gray', linestyle='--', linewidth=1.5,
           label='Chance level (50%)', zorder=4)

# p-value + 유의 표기
for i, (xpos, pv, n, fp, op, s) in enumerate(
        zip(x, p_vals, decisive_n, fresnel_pct, other_pct, sig)):
    star = ' *' if s else ' n.s.'
    top  = max(fp, op) + 2.5
    color = '#1A237E' if s else '#555'
    ax.text(xpos, top, f'p={pv}{star}\n(n={n})',
            ha='center', va='bottom', fontsize=8.5, color=color,
            fontweight='bold' if s else 'normal')

# 유의/비유의 배경
sig_spans   = [(-0.5, 0.5)]
insig_spans = [(0.5, 1.5), (1.5, 2.5), (2.5, 3.5)]
for lo, hi in sig_spans:
    ax.axvspan(lo, hi, alpha=0.08, color='blue', zorder=0)
for lo, hi in insig_spans:
    ax.axvspan(lo, hi, alpha=0.04, color='gray', zorder=0)

# 범례
leg_handles = [
    mpatches.Patch(color='#1565C0', label='Fresnel-guided preferred  (p < 0.05 *)'),
    mpatches.Patch(color='#90CAF9', label='Fresnel-guided preferred  (n.s.)'),
    mpatches.Patch(color='#B71C1C', label='Counterpart preferred  (p < 0.05 *)'),
    mpatches.Patch(color='#EF9A9A', label='Counterpart preferred  (n.s.)'),
    plt.Line2D([0],[0], color='gray', linestyle='--', linewidth=1.5,
               label='Chance level (50%)'),
]
ax.legend(handles=leg_handles, fontsize=8.5, loc='upper right',
          framealpha=0.9, edgecolor='#ccc')

ax.set_xlabel('Experimental Condition', fontsize=11)
ax.set_ylabel('Preference (%)', fontsize=11)
ax.set_title('2AFC User Study Results  (n=40, decisive responses only)\n'
             'Dark = statistically significant (p < 0.05),  Light = n.s.',
             fontsize=11)
ax.set_xticks(x)
ax.set_xticklabels(conditions, fontsize=10)
ax.set_ylim(0, 92)
ax.yaxis.set_minor_locator(ticker.MultipleLocator(5))
ax.grid(axis='y', which='major', alpha=0.3, zorder=0)

plt.tight_layout()
plt.savefig('figures_paper/extra4_user_study.png', bbox_inches='tight')
plt.close()
print("extra4_user_study.png saved")


print("\nAll extra figures saved to figures_paper/")
