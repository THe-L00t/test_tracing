"""make_poster.py
학술 포스터 생성: 희소 Fresnel 경계 샘플링
크기: 90 cm × 120 cm  (DPI=120  →  4252 × 5669 px)
"""
import matplotlib
matplotlib.use('Agg')

import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from matplotlib.patches import Rectangle, FancyBboxPatch
from matplotlib.image import imread
import matplotlib.font_manager as fm
import numpy as np

# ── 한국어 폰트 ────────────────────────────────────────────────
_available = {f.name for f in fm.fontManager.ttflist}
for _kf in ('Malgun Gothic', 'NanumGothic', 'AppleGothic', 'UnDotum'):
    if _kf in _available:
        plt.rcParams['font.family'] = _kf
        break
plt.rcParams['axes.unicode_minus'] = False

# ── 치수 ──────────────────────────────────────────────────────
W_IN = 90 / 2.54    # 35.43 in
H_IN = 120 / 2.54   # 47.24 in
DPI  = 120

# ── 색상 팔레트 ────────────────────────────────────────────────
C_NAVY   = '#0D1B4B'
C_BLUE   = '#1565C0'
C_DBLUE  = '#0D47A1'
C_LBLUE  = '#1976D2'
C_PALE   = '#EAF1FB'
C_BG     = '#F2F6FC'
C_WHITE  = '#FFFFFF'
C_TEXT   = '#1A1A2E'
C_GRAY   = '#546E7A'
C_GOLD   = '#F9A825'
C_GREEN  = '#2E7D32'
C_LGREEN = '#388E3C'
C_RED    = '#C62828'
C_BORDER = '#BBDEFB'

# ── 유틸리티 ──────────────────────────────────────────────────

def ax_base(ax, bg=C_WHITE, border_color=C_BORDER):
    """axes 초기화: 배경색 + 테두리 사각형"""
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis('off')
    ax.add_patch(Rectangle((0, 0), 1, 1,
                            facecolor=bg, edgecolor=border_color,
                            linewidth=1.8, transform=ax.transAxes,
                            clip_on=False, zorder=0))


def section_block(ax, title, body,
                  hdr_color=C_BLUE, hdr_h=0.082,
                  title_fs=12.5, body_fs=9.5,
                  body_x=0.045, body_y=None,
                  linespacing=1.60):
    """제목 헤더 + 본문 텍스트 블록"""
    ax_base(ax, bg=C_PALE)
    # 헤더 배경
    ax.add_patch(Rectangle((0, 1 - hdr_h), 1, hdr_h,
                            facecolor=hdr_color, transform=ax.transAxes,
                            clip_on=False, zorder=2))
    ax.text(0.5, 1 - hdr_h / 2, title,
            ha='center', va='center', transform=ax.transAxes,
            fontsize=title_fs, fontweight='bold', color=C_WHITE, zorder=3)
    # 본문
    y0 = (1 - hdr_h - 0.018) if body_y is None else body_y
    ax.text(body_x, y0, body,
            ha='left', va='top', transform=ax.transAxes,
            fontsize=body_fs, color=C_TEXT, linespacing=linespacing,
            clip_on=True)


def img_block(ax, path, title=None, caption=None,
              title_fs=11.5, cap_fs=8.5, bg=C_WHITE):
    """이미지 + 제목/캡션"""
    ax_base(ax, bg=bg)
    th = 0.072 if title   else 0.0
    ch = 0.062 if caption else 0.0
    iy = ch + 0.010
    ih = 1.0 - th - ch - 0.018

    if title:
        ax.text(0.5, 1.0 - th / 2, title,
                ha='center', va='center', transform=ax.transAxes,
                fontsize=title_fs, fontweight='bold', color=C_DBLUE)

    img = imread(path)
    ins = ax.inset_axes([0.010, iy, 0.980, ih])
    ins.imshow(img, aspect='auto')
    ins.axis('off')

    if caption:
        ax.text(0.5, ch / 2, caption,
                ha='center', va='center', transform=ax.transAxes,
                fontsize=cap_fs, color=C_GRAY, style='italic')


# ── Figure / GridSpec ──────────────────────────────────────────
fig = plt.figure(figsize=(W_IN, H_IN), dpi=DPI, facecolor=C_BG)

# height_ratios: title(7%) stats(5.5%) row1(35%) row2(33%) footer(11%)
# width_ratios:  left(28%) center(40%) right(32%)
gs = gridspec.GridSpec(
    5, 3,
    figure=fig,
    left=0.020, right=0.980,
    top=0.990,  bottom=0.010,
    hspace=0.028, wspace=0.020,
    height_ratios=[0.070, 0.055, 0.352, 0.330, 0.113],
    width_ratios=[0.28, 0.40, 0.32],
)

# ══════════════════════════════════════════════════════════════
# Row 0: TITLE BANNER
# ══════════════════════════════════════════════════════════════
ax_t = fig.add_subplot(gs[0, :])
ax_t.set_facecolor(C_NAVY)
ax_t.axis('off')
ax_t.text(0.5, 0.71,
    '희소 Fresnel 경계 샘플링:\n'
    '실시간 경로 추적을 위한 자기조절형 지각 레이 예산 전략',
    ha='center', va='center', transform=ax_t.transAxes,
    fontsize=29, fontweight='bold', color=C_WHITE, linespacing=1.22)
ax_t.text(0.5, 0.12,
    '[저자명]   ·   [소속]   ·   한국게임학회 2026 추계학술발표대회',
    ha='center', va='center', transform=ax_t.transAxes,
    fontsize=13.5, color='#90CAF9')

# ══════════════════════════════════════════════════════════════
# Row 1: KEY STATS BAR
# ══════════════════════════════════════════════════════════════
ax_s = fig.add_subplot(gs[1, :])
ax_s.set_facecolor('#1A237E')
ax_s.axis('off')

STATS = [
    ('5.73×',    '평균 속도 향상\nvs. Variance-guided'),
    ('5 / 5',    '60 fps 달성 씬\nFresnel-guided'),
    ('+1.23 dB', '평균 wPSNR 향상\n(Scene 5: +4.86 dB)'),
    ('p = 0.038', '사용자 선호 유의성\n(Scene 5, n=26, 2AFC)'),
]
for i, (val, lbl) in enumerate(STATS):
    xc = (i + 0.5) / len(STATS)
    ax_s.text(xc, 0.67, val, ha='center', va='center',
              transform=ax_s.transAxes,
              fontsize=24, fontweight='bold', color=C_GOLD)
    ax_s.text(xc, 0.19, lbl, ha='center', va='center',
              transform=ax_s.transAxes,
              fontsize=10.5, color='#E3F2FD', linespacing=1.30)
    if i:
        ax_s.axvline(i / len(STATS), color='#3949AB', lw=1.2)

# ══════════════════════════════════════════════════════════════
# Row 2: Introduction+Method (left) | Fig 3 (center) | Fig 4 (right)
# ══════════════════════════════════════════════════════════════

# ── Left: nested 2×1 grid ──────────────────────────────────
gs_r2l = gridspec.GridSpecFromSubplotSpec(
    2, 1, subplot_spec=gs[2, 0],
    hspace=0.028, height_ratios=[0.46, 0.54],
)
ax_intro = fig.add_subplot(gs_r2l[0])
section_block(
    ax_intro,
    '1.  서론  (Introduction)',
    '▶ 실시간 경로 추적(Path Tracing)은 물리 기반\n'
    '   렌더링의 표준이지만, 유리·금속 경계면에서\n'
    '   고노이즈 픽셀이 집중 발생함.\n\n'
    '▶ 기존 Variance-guided 적응 샘플링의 한계:\n'
    '  • 분산 계산 자체에 수십 ms 소요\n'
    '  • 씬 의존적 비용 폭발\n'
    '    (Scene 4: 71.2 ms → 실시간 불가)\n\n'
    '▶ 핵심 통찰:\n'
    '  Fresnel 항은 고주파 반사·굴절 픽셀을\n'
    '  렌더링 이전에 미리 예측 가능.\n'
    '  → 별도 계산 없이 얻을 수 있는\n'
    '    지각적 우선순위 신호로 활용.',
    body_fs=9.8,
)

ax_meth = fig.add_subplot(gs_r2l[1])
section_block(
    ax_meth,
    '2.  방법  (F(p) 기반 2-Pass PT)',
    'F(p) 우선순위 맵 정의:\n'
    '  Fterm = (1 - N*V)^5     [Schlick \'94]\n'
    '  Fsurf = 1 - roughness\n'
    '  Fspec = metallic + 2 * glass\n'
    '  F(p)  = Fterm * Fsurf * Fspec\n\n'
    '2-Pass 알고리즘:\n'
    '  Pass 1   전체 화면 저-spp 경로 추적\n'
    '  Pass 2   F(p) > T 픽셀에만 추가 레이\n\n'
    '자기조절 (Self-Regulation):\n'
    '  A = |{ p : F(p)>T }| / N  (커버리지)\n'
    '  A ~< 5%    → Pass 2 비용 극소\n'
    '  A  > 20%  → 비용 급증 (중립 예측)',
    hdr_color=C_LGREEN,
    body_fs=9.8,
)

# ── Center: Figure 3 ───────────────────────────────────────
ax_f3 = fig.add_subplot(gs[2, 1])
img_block(
    ax_f3,
    'figures_paper/figure3_comparison.png',
    title='Fig. 3  |  품질 비교  (Scene 8: Metal + Glass,  A = 0.9%)',
    caption='Fresnel: wPSNR +0.81 dB, Pass 2 = 0.52 ms     vs     Variance: +0.74 dB, 14.28 ms',
)

# ── Right: Figure 4 ────────────────────────────────────────
ax_f4 = fig.add_subplot(gs[2, 2])
img_block(
    ax_f4,
    'figures_paper/figure4_gpu_timing.png',
    title='Fig. 4  |  GPU 프레임 타이밍  (10-frame mean ± σ)',
    caption='Fresnel-guided: 5/5 씬 60 fps 달성   |   Variance-guided: 4/5 씬 60 fps 초과',
)

# ══════════════════════════════════════════════════════════════
# Row 3: Fig 1 (left) | Fig 2 (center) | Fig 5 (right)
# ══════════════════════════════════════════════════════════════
ax_f1 = fig.add_subplot(gs[3, 0])
img_block(
    ax_f1,
    'figures_paper/figure1_teaser.png',
    title='Fig. 1  |  F(p) 우선순위 맵  (Scene 5,  A = 0.2%)',
    caption='상: F(p) 우선순위 맵 — 경계면 픽셀 강조   |   하: Fresnel-Guided PT 100spp 결과',
)

ax_f2 = fig.add_subplot(gs[3, 1])
img_block(
    ax_f2,
    'figures_paper/figure2_self_regulation.png',
    title='Fig. 2  |  자기조절: Pass 2 비용 vs F(p) 커버리지 A',
    caption='A ~< 5% (파란 영역): Fresnel이 압도적 효율   |   A > 20% (빨간): 비용 급증·성능 중립',
)

ax_f5 = fig.add_subplot(gs[3, 2])
img_block(
    ax_f5,
    'figures_paper/figure5_user_study.png',
    title='Fig. 5  |  사용자 선호도  (2AFC,  n = 40)',
    caption='Scene 5 vs Baseline: 69.2%  p=0.038 *   |   A > 20% 씬: 중립 (이론 일치)',
)

# ══════════════════════════════════════════════════════════════
# Row 4: FOOTER — Results | Conclusion | References
# ══════════════════════════════════════════════════════════════
gs_foot = gridspec.GridSpecFromSubplotSpec(
    1, 3, subplot_spec=gs[4, :],
    wspace=0.022, width_ratios=[0.34, 0.36, 0.30],
)

ax_res = fig.add_subplot(gs_foot[0])
section_block(
    ax_res,
    '3.  주요 실험 결과',
    'GPU 타이밍  (표 4,  Fresnel vs Variance):\n'
    '  Scene 4:   8.0 ms  vs  71.2 ms   (8.9×)\n'
    '  Scene 5:   4.3 ms  vs  29.5 ms   (6.8×)\n'
    '  Scene 7:   5.2 ms  vs  45.1 ms   (8.6×)\n'
    '  평균:      8.7 ms  vs  40.4 ms   (5.73×)\n'
    '  60 fps:   Fresnel 5/5 [OK]   Variance 1/5',
    title_fs=11.5, body_fs=9.5, hdr_h=0.125, linespacing=1.65,
)

ax_conc = fig.add_subplot(gs_foot[1])
section_block(
    ax_conc,
    '4.  결론 및 향후 연구',
    '[OK]  F(p) 사전 예측 → 분산 계산 불필요\n'
    '[OK]  A ~< 5% 씬에서 5-9x 속도 향상, 60 fps 달성\n'
    '[OK]  wPSNR +1.23 dB,  유의미한 사용자 선호\n\n'
    '한계 및 향후 과제:\n'
    '  * A > 20% (유리 비중 높은 씬) 적용 제한\n'
    '  * 동적 씬의 T 자동 조정 연구 필요\n'
    '  * ReSTIR DI/GI와 통합 가능성 탐색',
    hdr_color=C_LGREEN, title_fs=11.5, body_fs=9.5, hdr_h=0.125, linespacing=1.65,
)

ax_ref = fig.add_subplot(gs_foot[2])
section_block(
    ax_ref,
    '참고문헌',
    '[1] Bitterli et al., ReSTIR, 2020\n'
    '[6] Schlick, BRDF Model, 1994\n'
    '[7] Schied et al., SVGF, 2017\n'
    '[8] Andersson et al., FLIP, 2020\n'
    '[9] Walter et al., Microfacet, 2007\n'
    '[11] Veach & Guibas, MIS, 1995\n'
    '[12] Pharr et al., PBR 4th ed., 2023\n'
    '[13] Akenine-Möller, RTR 4th, 2018',
    hdr_color=C_NAVY, title_fs=11.5, body_fs=9.2, hdr_h=0.125, linespacing=1.62,
)

# ══════════════════════════════════════════════════════════════
# 저장
# ══════════════════════════════════════════════════════════════
OUT = 'poster_90x120.png'
fig.savefig(OUT, dpi=DPI, bbox_inches='tight', facecolor=C_BG, format='png')
plt.close(fig)
print(f'Saved: {OUT}  ({W_IN*DPI:.0f} × {H_IN*DPI:.0f} px @ {DPI} dpi)')
