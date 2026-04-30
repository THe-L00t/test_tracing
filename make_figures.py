"""
논문 Figure 생성 스크립트
- 그림 4: GPU 타이밍 bar chart (표 4 데이터 기반)
- 그림 5: 사용자 설문 bar chart (표 6 데이터 기반)
- 그림 2: Pass 2 자기조절 scatter plot (추가 측정 필요 — 하단 주석 참조)
"""

import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

plt.rcParams.update({
    'font.family': 'DejaVu Sans',
    'axes.spines.top': False,
    'axes.spines.right': False,
    'figure.dpi': 150,
})

# ─────────────────────────────────────────────────────────────
# 그림 4: GPU 프레임 타이밍 bar chart
# ─────────────────────────────────────────────────────────────
scenes      = ['Scene 4', 'Scene 5', 'Scene 6', 'Scene 7', 'Scene 8']
f_mean      = [8.032,  4.309,  12.973, 5.234,  12.992]
f_std       = [0.170,  0.162,  0.934,  0.167,  0.907]
v_mean      = [71.165, 29.493, 13.601, 45.143, 42.528]
v_std       = [5.474,  2.741,  0.977,  1.117,  0.924]
fps60_line  = 16.67

x     = np.arange(len(scenes))
width = 0.35

fig, ax = plt.subplots(figsize=(9, 5))

bars_f = ax.bar(x - width / 2, f_mean, width, yerr=f_std, capsize=4,
                label='Fresnel-guided', color='#1976D2', zorder=3)
bars_v = ax.bar(x + width / 2, v_mean, width, yerr=v_std, capsize=4,
                label='Variance-guided', color='#D32F2F', zorder=3)
ax.axhline(fps60_line, color='#2E7D32', linestyle='--', linewidth=1.6,
           label='60 fps threshold (16.67 ms)', zorder=4)

# 배율 표기
for i, (xpos, fm, vm) in enumerate(zip(x, f_mean, v_mean)):
    ratio = vm / fm
    top   = max(fm + f_std[i], vm + v_std[i]) + 2.0
    ax.text(xpos, top, f'{ratio:.1f}×', ha='center', va='bottom',
            fontsize=8.5, color='#555', fontweight='bold')

ax.set_xlabel('Scene', fontsize=12)
ax.set_ylabel('Frame Time (ms)', fontsize=12)
ax.set_title('GPU Frame Timing: Fresnel-guided vs Variance-guided\n'
             '(10-frame mean ± σ)', fontsize=12)
ax.set_xticks(x)
ax.set_xticklabels(scenes)
ax.set_ylim(0, 90)
ax.yaxis.set_minor_locator(ticker.MultipleLocator(5))
ax.grid(axis='y', which='major', alpha=0.3, zorder=0)
ax.legend(fontsize=10, loc='upper right')

plt.tight_layout()
plt.savefig('figure4_gpu_timing.png', bbox_inches='tight')
plt.close()
print("✓ figure4_gpu_timing.png 저장 완료")


# ─────────────────────────────────────────────────────────────
# 그림 5: 사용자 설문 2AFC bar chart
# ─────────────────────────────────────────────────────────────
conditions   = ['Scene 5\nF vs Baseline', 'Scene 5\nF vs Variance',
                'Scene 7\nF vs Baseline', 'Scene 8\nF vs Baseline']
fresnel_pct  = [69.2, 38.2, 47.1, 59.1]
other_pct    = [30.8, 61.8, 52.9, 40.9]
other_label  = ['Baseline', 'Variance', 'Baseline', 'Baseline']
p_vals       = [0.038,  0.085,  0.597,  0.197]
decisive_n   = [26,     34,     17,     22]
# A(%) 커버리지 — envelope 구분에 사용
a_pct        = [0.2,    0.2,    29.1,   0.9]

x     = np.arange(len(conditions))
width = 0.35

fig, ax = plt.subplots(figsize=(9, 5))

bars_f = ax.bar(x - width / 2, fresnel_pct, width,
                label='Fresnel-guided preferred', color='#1976D2', zorder=3)
bars_o = ax.bar(x + width / 2, other_pct,   width,
                label='Counterpart preferred',    color='#D32F2F', zorder=3)
ax.axhline(50, color='gray', linestyle='--', linewidth=1.4,
           label='Chance level (50%)', zorder=4)

# p값 + 유의성 표기
for i, (xpos, pv, n, fp, op) in enumerate(
        zip(x, p_vals, decisive_n, fresnel_pct, other_pct)):
    sig  = ' *' if pv < 0.05 else ''
    top  = max(fp, op) + 2.5
    ax.text(xpos, top, f'p={pv}{sig}\n(n={n})',
            ha='center', va='bottom', fontsize=8, color='#333')

# Operating envelope 구분 배경
ax.axvspan(-0.5, 0.5,  alpha=0.06, color='blue',  label='Envelope 내부 (A≲5%)')
ax.axvspan( 0.5, 1.5,  alpha=0.06, color='blue')
ax.axvspan( 1.5, 2.5,  alpha=0.06, color='red',   label='Envelope 외부 (A>20%)')
ax.axvspan( 2.5, 3.5,  alpha=0.06, color='blue')

ax.set_xlabel('Experimental Condition', fontsize=12)
ax.set_ylabel('Preference (%)', fontsize=12)
ax.set_title('2AFC User Study Results (n=40, decisive responses only)', fontsize=12)
ax.set_xticks(x)
ax.set_xticklabels(conditions, fontsize=9.5)
ax.set_ylim(0, 88)
ax.grid(axis='y', alpha=0.3, zorder=0)
ax.legend(fontsize=9, loc='upper right')

plt.tight_layout()
plt.savefig('figure5_user_study.png', bbox_inches='tight')
plt.close()
print("✓ figure5_user_study.png 저장 완료")


# ─────────────────────────────────────────────────────────────
# 그림 2: Self-Regulation scatter plot
# ─────────────────────────────────────────────────────────────
# 현재 확보된 Pass 2 단독 데이터:
#   Scene 5만 있음 → Fresnel: 0.048 ms,  Variance: 25.278 ms,  A=0.2%
#
# 나머지 씬(4, 6, 7, 8)은 T키/Y키가 Pass1/Pass2/Total을 출력하므로
# 앱에서 각 씬 진입 후 T키를 눌러 Pass 2 단독 ms를 기록해야 한다.
#
# 측정 절차:
#   1. 씬 진입 → 카메라 정지 → Y키 (10프레임 평균)
#      → 콘솔 출력 "Pass1=X ms / Pass2=Y ms / Total=Z ms" 에서 Y 기록
#   2. N키로 Variance 모드 전환 → Y키 재측정
#   3. 씬 4, 6, 7, 8 반복
#
# 아래는 Scene 5 실측값 + 나머지 placeholder 예시 코드.
# 측정값 입력 후 주석 해제하여 실행.

A_pct = {
    'Scene 4': 29.1,
    'Scene 5': 0.2,
    'Scene 6': 0.0,
    'Scene 7': 29.1,
    'Scene 8': 0.9,
}

# ↓ Pass 2 단독 타이밍 (ms). Scene 5만 실측, 나머지는 None으로 표시.
pass2_fresnel  = {'Scene 4': None,  'Scene 5': 0.048,  'Scene 6': None,
                  'Scene 7': None,  'Scene 8': None}
pass2_variance = {'Scene 4': None,  'Scene 5': 25.278, 'Scene 6': None,
                  'Scene 7': None,  'Scene 8': None}

measured_scenes = [s for s in A_pct if pass2_fresnel[s] is not None]

if len(measured_scenes) < 2:
    print("\n그림 2: Pass 2 단독 타이밍 데이터 부족 — 앱에서 나머지 씬 Y키 측정 후 위 dict 채워주세요.")
else:
    fig, ax = plt.subplots(figsize=(7, 5))
    for scene in measured_scenes:
        ax.scatter(A_pct[scene], pass2_fresnel[scene],
                   color='#1976D2', s=80, zorder=4)
        ax.scatter(A_pct[scene], pass2_variance[scene],
                   color='#D32F2F', s=80, marker='s', zorder=4)
        ax.annotate(scene,
                    (A_pct[scene], max(pass2_fresnel[scene], pass2_variance[scene])),
                    textcoords='offset points', xytext=(4, 4), fontsize=8)

    f_patch = plt.Line2D([0], [0], marker='o', color='w',
                          markerfacecolor='#1976D2', markersize=9,
                          label='Fresnel-guided Pass 2')
    v_patch = plt.Line2D([0], [0], marker='s', color='w',
                          markerfacecolor='#D32F2F', markersize=9,
                          label='Variance-guided Pass 2')
    ax.legend(handles=[f_patch, v_patch], fontsize=10)
    ax.set_xlabel('High-F(p) Coverage A (%)', fontsize=12)
    ax.set_ylabel('Pass 2 Cost (ms)', fontsize=12)
    ax.set_title('Self-Regulation: Pass 2 Cost vs F(p) Coverage', fontsize=12)
    ax.set_xlim(-1, 32)
    ax.grid(alpha=0.3)

    plt.tight_layout()
    plt.savefig('figure2_self_regulation.png', bbox_inches='tight')
    plt.close()
    print("✓ figure2_self_regulation.png 저장 완료")
