"""make_poster.py
학술 포스터 최종본 — 이태형, 한국공학대학교 게임공학과
90 cm × 120 cm  @  120 DPI  →  4252 × 5669 px
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
_avail = {f.name for f in fm.fontManager.ttflist}
for _kf in ('Malgun Gothic', 'NanumGothic', 'AppleGothic'):
    if _kf in _avail:
        plt.rcParams['font.family'] = _kf
        break
plt.rcParams['axes.unicode_minus'] = False

# ── 치수 ──────────────────────────────────────────────────────
W_IN = 90 / 2.54
H_IN = 120 / 2.54
DPI  = 120

# ── 색상 팔레트 ────────────────────────────────────────────────
C_NAVY    = '#0D1B4B'
C_NAVY2   = '#16275E'
C_BLUE    = '#1565C0'
C_DBLUE   = '#0D47A1'
C_LBLUE   = '#1976D2'
C_PALE    = '#E8F0FE'
C_PALE2   = '#F0F5FF'
C_GREEN   = '#2E7D32'
C_TEAL    = '#00695C'
C_BG      = '#EEF3FB'
C_WHITE   = '#FFFFFF'
C_TEXT    = '#1A1A2E'
C_GRAY    = '#546E7A'
C_GOLD    = '#F9A825'
C_RED     = '#C62828'
C_BORDER  = '#BBDEFB'
C_ROWALT  = '#F5F9FF'
C_ROWHEAD = '#1565C0'
C_ROWSUM  = '#DCEEFB'

# ══════════════════════════════════════════════════════════════
# 유틸리티
# ══════════════════════════════════════════════════════════════

def ax_base(ax, bg=C_WHITE, border=C_BORDER, lw=1.6):
    ax.set_xlim(0, 1); ax.set_ylim(0, 1); ax.axis('off')
    ax.add_patch(Rectangle((0, 0), 1, 1,
        facecolor=bg, edgecolor=border, linewidth=lw,
        transform=ax.transAxes, clip_on=False, zorder=0))


def section_block(ax, title, body,
                  hdr_color=C_BLUE, hdr_h=0.080,
                  title_fs=13.5, body_fs=10.0,
                  body_x=0.040, body_y=None,
                  linespacing=1.62):
    ax_base(ax, bg=C_PALE)
    # 헤더
    ax.add_patch(Rectangle((0, 1 - hdr_h), 1, hdr_h,
        facecolor=hdr_color, transform=ax.transAxes,
        clip_on=False, zorder=2))
    # 헤더 좌측 accent 라인
    ax.add_patch(Rectangle((0, 1 - hdr_h), 0.007, hdr_h,
        facecolor=C_GOLD, transform=ax.transAxes,
        clip_on=False, zorder=3))
    ax.text(0.50, 1 - hdr_h / 2, title,
        ha='center', va='center', transform=ax.transAxes,
        fontsize=title_fs, fontweight='bold', color=C_WHITE, zorder=4)
    y0 = (1 - hdr_h - 0.018) if body_y is None else body_y
    ax.text(body_x, y0, body,
        ha='left', va='top', transform=ax.transAxes,
        fontsize=body_fs, color=C_TEXT, linespacing=linespacing,
        clip_on=True, zorder=1)


def img_block(ax, path, title=None, caption=None,
              title_fs=12.5, cap_fs=9.5):
    ax_base(ax, bg=C_WHITE)
    th = 0.067 if title   else 0.0
    ch = 0.060 if caption else 0.0
    iy = ch + 0.008
    ih = 1.0 - th - ch - 0.016
    if title:
        ax.text(0.5, 1.0 - th / 2, title,
            ha='center', va='center', transform=ax.transAxes,
            fontsize=title_fs, fontweight='bold', color=C_DBLUE)
    img = imread(path)
    ins = ax.inset_axes([0.006, iy, 0.988, ih])
    ins.imshow(img, aspect='auto')
    ins.axis('off')
    if caption:
        ax.text(0.5, ch / 2, caption,
            ha='center', va='center', transform=ax.transAxes,
            fontsize=cap_fs, color=C_GRAY, style='italic')


def draw_pipeline(ax, y_bot=0.035, y_top=0.435):
    """2-Pass 렌더링 파이프라인 다이어그램"""
    mid = (y_bot + y_top) / 2
    bh  = (y_top - y_bot) * 0.60
    bw  = 0.148

    # 파이프라인 영역 배경
    ax.add_patch(Rectangle((0.01, y_bot - 0.005), 0.98, y_top - y_bot + 0.045,
        facecolor='#E3F0FF', edgecolor='#90CAF9', linewidth=1.0,
        transform=ax.transAxes, clip_on=True, zorder=2, alpha=0.85))

    # 타이틀
    ax.text(0.50, y_top + 0.025, '[ 2-Pass 렌더링 파이프라인 ]',
        ha='center', va='bottom', transform=ax.transAxes,
        fontsize=9.0, color=C_BLUE, fontweight='bold', zorder=5)

    # 5개 박스
    STEPS = [
        (0.020, '입력\n씬',           C_NAVY),
        (0.215, 'Pass 1\n저spp PT',   C_LBLUE),
        (0.410, 'F(p)\n맵 생성',      C_GREEN),
        (0.605, 'Pass 2\n추가 레이',   C_LBLUE),
        (0.800, '최종\n출력',          C_RED),
    ]
    for x0, lbl, color in STEPS:
        ax.add_patch(FancyBboxPatch(
            (x0, mid - bh / 2), bw, bh,
            boxstyle='round,pad=0.016',
            facecolor=color, edgecolor='white', linewidth=1.5,
            transform=ax.transAxes, clip_on=True, zorder=6))
        ax.text(x0 + bw / 2, mid, lbl,
            ha='center', va='center', transform=ax.transAxes,
            fontsize=8.5, color='white', fontweight='bold',
            linespacing=1.25, zorder=7)

    # 화살표 (각 gap 중간점)
    GAPS = [(0.168, 0.215), (0.363, 0.410), (0.558, 0.605), (0.753, 0.800)]
    for xs, xe in GAPS:
        ax.annotate('',
            xy=(xe - 0.004, mid), xytext=(xs + 0.004, mid),
            xycoords='axes fraction', textcoords='axes fraction',
            arrowprops=dict(arrowstyle='->', color='#37474F', lw=2.2),
            zorder=8)

    # F(p) > T 레이블
    ax.text(0.681, mid + bh / 2 + 0.048, 'F(p) > T',
        ha='center', va='bottom', transform=ax.transAxes,
        fontsize=8.5, color=C_RED, fontstyle='italic',
        fontweight='bold', zorder=8)
    # 작은 하향 화살표
    ax.annotate('',
        xy=(0.681, mid + bh / 2 + 0.004), xytext=(0.681, mid + bh / 2 + 0.042),
        xycoords='axes fraction', textcoords='axes fraction',
        arrowprops=dict(arrowstyle='->', color=C_RED, lw=1.5), zorder=8)


def draw_results_table(ax):
    """실험 결과 표"""
    ax_base(ax, bg=C_PALE)
    # 헤더
    ax.add_patch(Rectangle((0, 0.878), 1, 0.122,
        facecolor=C_BLUE, transform=ax.transAxes, clip_on=False, zorder=2))
    ax.add_patch(Rectangle((0, 0.878), 0.007, 0.122,
        facecolor=C_GOLD, transform=ax.transAxes, clip_on=False, zorder=3))
    ax.text(0.50, 0.939, '3.  실험 결과',
        ha='center', va='center', transform=ax.transAxes,
        fontsize=13.5, fontweight='bold', color=C_WHITE, zorder=4)

    TABLE = [
        ('씬',       'Fresnel (ms)', 'Variance (ms)', '배율',  'wPSNR'),
        ('Scene 4',  '8.0 ± 0.17',  '71.2 ± 5.47',  '8.9x',  '—'),
        ('Scene 5',  '4.3 ± 0.16',  '29.5 ± 2.74',  '6.8x',  '+4.86 dB'),
        ('Scene 6',  '13.0 ± 0.93', '13.6 ± 0.98',  '1.0x',  '—'),
        ('Scene 7',  '5.2 ± 0.17',  '45.1 ± 1.12',  '8.6x',  '—'),
        ('Scene 8',  '13.0 ± 0.91', '42.5 ± 0.92',  '3.3x',  '+0.81 dB'),
        ('평균',     '8.7 ms',       '40.4 ms',       '5.73x', '+1.23 dB'),
    ]
    COL_X  = [0.030, 0.220, 0.445, 0.675, 0.810]
    N      = len(TABLE)
    ROW_T  = 0.862
    ROW_H  = (ROW_T - 0.012) / N

    for r, row in enumerate(TABLE):
        yc  = ROW_T - (r + 0.5) * ROW_H
        yt  = ROW_T - r * ROW_H
        yb  = yt - ROW_H

        if r == 0:
            ax.add_patch(Rectangle((0.01, yb + 0.003), 0.98, ROW_H - 0.004,
                facecolor=C_ROWHEAD, alpha=0.90,
                transform=ax.transAxes, clip_on=True, zorder=1))
            fc, fw, fs = C_WHITE, 'bold', 8.8
        elif r == N - 1:
            ax.add_patch(Rectangle((0.01, yb + 0.003), 0.98, ROW_H - 0.004,
                facecolor=C_ROWSUM,
                transform=ax.transAxes, clip_on=True, zorder=1))
            fc, fw, fs = C_NAVY, 'bold', 8.8
        else:
            if r % 2 == 0:
                ax.add_patch(Rectangle((0.01, yb + 0.003), 0.98, ROW_H - 0.004,
                    facecolor=C_ROWALT,
                    transform=ax.transAxes, clip_on=True, zorder=1))
            fc, fw, fs = C_TEXT, 'normal', 8.8

        for c, (cell, cx) in enumerate(zip(row, COL_X)):
            ax.text(cx, yc, cell,
                ha=('left' if c == 0 else 'center'), va='center',
                transform=ax.transAxes,
                fontsize=fs, color=fc, fontweight=fw, zorder=2)

    # 60fps 주석
    ax.text(0.50, 0.010, '60 fps 달성: Fresnel-guided 5/5   vs   Variance-guided 1/5',
        ha='center', va='bottom', transform=ax.transAxes,
        fontsize=9.0, color=C_BLUE, fontweight='bold', zorder=2)


# ══════════════════════════════════════════════════════════════
# Figure 생성
# ══════════════════════════════════════════════════════════════
fig = plt.figure(figsize=(W_IN, H_IN), dpi=DPI, facecolor=C_BG)

# height_ratios: title(9%) abstract(4%) stats(5%) row1(34%) row2(31%) footer(12%)
gs = gridspec.GridSpec(
    6, 3,
    figure=fig,
    left=0.016, right=0.984,
    top=0.990, bottom=0.010,
    hspace=0.022, wspace=0.018,
    height_ratios=[0.090, 0.040, 0.050, 0.340, 0.310, 0.120],
    width_ratios=[0.28, 0.40, 0.32],
)

# ══════════════════════════════════════════════════════════════
# Row 0: TITLE BANNER
# ══════════════════════════════════════════════════════════════
ax_t = fig.add_subplot(gs[0, :])
ax_t.set_facecolor(C_NAVY); ax_t.axis('off')

# 배경 레이어 (깊이감)
ax_t.add_patch(Rectangle((0, 0.00), 1, 0.22, facecolor='#131F55',
    transform=ax_t.transAxes, clip_on=False, zorder=1))
ax_t.add_patch(Rectangle((0, 0.22), 1, 0.34, facecolor='#0F1D50',
    transform=ax_t.transAxes, clip_on=False, zorder=1))
# 좌우 accent bar
ax_t.add_patch(Rectangle((0.000, 0), 0.006, 1, facecolor=C_GOLD,
    transform=ax_t.transAxes, clip_on=False, zorder=3))
ax_t.add_patch(Rectangle((0.994, 0), 0.006, 1, facecolor=C_GOLD,
    transform=ax_t.transAxes, clip_on=False, zorder=3))
# 하단 구분선
ax_t.add_patch(Rectangle((0.006, 0.00), 0.988, 0.025, facecolor='#F9A82588',
    transform=ax_t.transAxes, clip_on=False, zorder=2, alpha=0.5))

ax_t.text(0.500, 0.71,
    '희소 Fresnel 경계 샘플링:\n'
    '실시간 경로 추적을 위한 자기조절형 지각 레이 예산 전략',
    ha='center', va='center', transform=ax_t.transAxes,
    fontsize=30, fontweight='bold', color=C_WHITE, linespacing=1.20, zorder=4)

ax_t.text(0.500, 0.12,
    '이태형   ·   한국공학대학교 게임공학과   ·   한국게임학회 2026 추계학술발표대회',
    ha='center', va='center', transform=ax_t.transAxes,
    fontsize=14.0, color='#90CAF9', zorder=4)

# ══════════════════════════════════════════════════════════════
# Row 1: ABSTRACT STRIP
# ══════════════════════════════════════════════════════════════
ax_ab = fig.add_subplot(gs[1, :])
ax_ab.set_facecolor('#1D2D82'); ax_ab.axis('off')

ax_ab.add_patch(Rectangle((0, 0), 0.005, 1, facecolor=C_GOLD,
    transform=ax_ab.transAxes, clip_on=False, zorder=2))

ax_ab.text(0.013, 0.56, '초 록',
    ha='left', va='center', transform=ax_ab.transAxes,
    fontsize=11.5, fontweight='bold', color=C_GOLD, zorder=3)
ax_ab.add_patch(Rectangle((0.056, 0.42), 0.001, 0.30, facecolor='#4A6BD8',
    transform=ax_ab.transAxes, clip_on=False, zorder=2))
ax_ab.text(0.064, 0.56,
    'Fresnel 반사율을 지각적 우선순위 신호로 활용하는 2-Pass 자기조절형 레이 예산 전략을 제안한다. '
    'GPU에서 F(p) 우선순위 맵을 산출한 뒤 고우선순위 픽셀에만 추가 레이를 투사하여, '
    'Variance-guided 대비 평균 5.73x 가속, wPSNR +1.23 dB 향상을 달성하였으며, '
    '사용자 실험(n=40, 2AFC)에서 operating envelope(A ~< 5%) 내 씬의 유의미한 시각 선호(p=0.038)를 확인하였다.',
    ha='left', va='center', transform=ax_ab.transAxes,
    fontsize=10.8, color='#E3F2FD', linespacing=1.22, zorder=3)

# ══════════════════════════════════════════════════════════════
# Row 2: KEY STATS
# ══════════════════════════════════════════════════════════════
ax_s = fig.add_subplot(gs[2, :])
ax_s.set_facecolor('#1A237E'); ax_s.axis('off')

STATS = [
    ('5.73x',    '평균 속도 향상',         'vs. Variance-guided'),
    ('5 / 5',    '60 fps 달성 씬 수',      'Fresnel-guided 전 씬'),
    ('+1.23 dB', '평균 wPSNR 향상',        'Scene 5 최고: +4.86 dB'),
    ('p = 0.038','사용자 선호 유의성',      'n=26, 2AFC, Scene 5'),
]
for i, (val, lbl1, lbl2) in enumerate(STATS):
    xc = (i + 0.5) / len(STATS)
    if i:
        ax_s.axvline(i / len(STATS), color='#3949AB', lw=1.0)
    ax_s.text(xc, 0.70, val,
        ha='center', va='center', transform=ax_s.transAxes,
        fontsize=24, fontweight='bold', color=C_GOLD)
    ax_s.text(xc, 0.33, lbl1,
        ha='center', va='center', transform=ax_s.transAxes,
        fontsize=10.5, color='#E3F2FD', fontweight='bold')
    ax_s.text(xc, 0.10, lbl2,
        ha='center', va='center', transform=ax_s.transAxes,
        fontsize=9.2, color='#9FA8DA')

# ══════════════════════════════════════════════════════════════
# Row 3: Introduction+Method (left) | Fig 3 (center) | Fig 4 (right)
# ══════════════════════════════════════════════════════════════
gs_r3l = gridspec.GridSpecFromSubplotSpec(
    2, 1, subplot_spec=gs[3, 0],
    hspace=0.020, height_ratios=[0.43, 0.57],
)

# ── Introduction ───────────────────────────────────────────────
ax_intro = fig.add_subplot(gs_r3l[0])
section_block(ax_intro,
    '1.  서론  (Introduction)',
    '▶ GPU DXR 가속으로 실시간 경로 추적이 게임에\n'
    '   확산되고 있으나, 유리·금속 경계면(그레이징\n'
    '   각도)에서 레이당 분산이 폭발적으로 증가.\n\n'
    '▶ 기존 Variance-guided [SVGF, Schied \'17] 한계:\n'
    '  - 분산 추정을 위한 추가 렌더 패스 필요\n'
    '  - 씬 의존적 비용 폭발 (8.9x 격차)\n'
    '  - Scene 4: 71.2 ms → 60 fps 불가\n\n'
    '▶ 핵심 통찰:\n'
    '  Fresnel 방정식은 레이 분산이 클 픽셀을\n'
    '  렌더링 전에 사전 예측 가능.\n'
    '  분산 계산 없이 단일 경량 pass로\n'
    '  지각적 우선순위 맵 F(p)를 산출.',
    body_fs=10.0)

# ── Method ─────────────────────────────────────────────────────
ax_meth = fig.add_subplot(gs_r3l[1])
section_block(ax_meth,
    '2.  방법  (Fresnel-Guided 2-Pass PT)',
    'F(p) 우선순위 맵  [Schlick, 1994]:\n'
    '  Fterm = (1 - N*V)^5\n'
    '  Fsurf = 1 - roughness\n'
    '  Fspec = metallic + 2 * glass\n'
    '  F(p)  = Fterm * Fsurf * Fspec\n\n'
    'A = |{p : F(p) > T}| / N  (커버리지 비율)\n'
    '  A ~< 5%    Operating Envelope (유효)\n'
    '  A  > 20%  Envelope 외부 (품질 중립)',
    hdr_color=C_GREEN, body_fs=10.0, body_y=0.885)
# 파이프라인 다이어그램
draw_pipeline(ax_meth, y_bot=0.038, y_top=0.440)

# ── Fig 3 ──────────────────────────────────────────────────────
ax_f3 = fig.add_subplot(gs[3, 1])
img_block(ax_f3,
    'figures_paper/figure3_comparison.png',
    title='Fig. 3  |  품질 비교 — Scene 8  (Metal + Glass,  A = 0.9%)',
    caption='Fresnel-Guided: wPSNR +0.81 dB, Pass 2 = 0.52 ms   '
            'vs   Variance-Guided: +0.74 dB, Pass 2 = 14.28 ms')

# ── Fig 4 ──────────────────────────────────────────────────────
ax_f4 = fig.add_subplot(gs[3, 2])
img_block(ax_f4,
    'figures_paper/figure4_gpu_timing.png',
    title='Fig. 4  |  GPU 프레임 타이밍  (10-frame mean ± σ)',
    caption='Fresnel-Guided: 전 씬 60 fps 달성   |   '
            'Variance-Guided: 4/5 씬에서 16.67 ms 초과')

# ══════════════════════════════════════════════════════════════
# Row 4: Fig 1 (left) | Fig 2 (center) | Fig 5 (right)
# ══════════════════════════════════════════════════════════════
ax_f1 = fig.add_subplot(gs[4, 0])
img_block(ax_f1,
    'figures_paper/figure1_teaser.png',
    title='Fig. 1  |  F(p) 우선순위 맵  (Scene 5,  A = 0.2%)',
    caption='상: F(p) 맵 — 금속 구 경계면 픽셀 강조   |   '
            '하: Fresnel-Guided PT 렌더 결과 (100 spp)')

ax_f2 = fig.add_subplot(gs[4, 1])
img_block(ax_f2,
    'figures_paper/figure2_self_regulation.png',
    title='Fig. 2  |  자기조절: Pass 2 비용 vs F(p) 커버리지 A',
    caption='A ~< 5% (파란 영역): Fresnel 압도적 효율   |   '
            'A > 20% (빨간 영역): 비용 급증·성능 중립')

ax_f5 = fig.add_subplot(gs[4, 2])
img_block(ax_f5,
    'figures_paper/figure5_user_study.png',
    title='Fig. 5  |  2AFC 사용자 선호도 실험  (n = 40)',
    caption='Scene 5 vs Baseline: 69.2%  p=0.038 *   |   '
            'A > 20% 씬: 중립 결과 — Operating Envelope 이론과 일치')

# ══════════════════════════════════════════════════════════════
# Row 5: FOOTER — Results table | Conclusion | References
# ══════════════════════════════════════════════════════════════
gs_foot = gridspec.GridSpecFromSubplotSpec(
    1, 3, subplot_spec=gs[5, :],
    wspace=0.018, width_ratios=[0.36, 0.35, 0.29],
)

# ── 결과 표 ────────────────────────────────────────────────────
ax_res = fig.add_subplot(gs_foot[0])
draw_results_table(ax_res)

# ── 결론 ───────────────────────────────────────────────────────
ax_conc = fig.add_subplot(gs_foot[1])
section_block(ax_conc,
    '4.  결론 및 향후 연구',
    '성과 요약:\n'
    '  - F(p) 사전 예측으로 분산 계산 패스 완전 제거\n'
    '  - A ~< 5% 씬에서 5-9x 속도 향상, 전 씬 60 fps\n'
    '  - wPSNR +1.23 dB, 유의미한 사용자 시각 선호\n\n'
    '한계 및 향후 과제:\n'
    '  - A > 20% 씬 (유리 비중 높음) 지원 제한\n'
    '    → 커버리지 적응형 임계값 T 자동 조정\n'
    '  - 동적 씬에서의 시간적 안정성 검증 필요\n'
    '  - ReSTIR DI/GI와의 통합 가능성 탐색',
    hdr_color=C_GREEN, title_fs=13.5, body_fs=9.8,
    hdr_h=0.122, linespacing=1.62)

# ── 참고문헌 ────────────────────────────────────────────────────
ax_ref = fig.add_subplot(gs_foot[2])
section_block(ax_ref,
    '참고문헌',
    '[1] Bitterli et al. ReSTIR DI. SIGGRAPH 2020\n'
    '[6] Schlick. BRDF Approximation. EG 1994\n'
    '[7] Schied et al. SVGF. SIGGRAPH 2017\n'
    '[8] Andersson et al. FLIP. HPG 2020\n'
    '[9] Walter et al. Microfacet. EGSR 2007\n'
    '[11] Veach & Guibas. MIS. SIGGRAPH 1995\n'
    '[12] Pharr et al. PBRT 4th ed. 2023\n'
    '[13] Akenine-Moller et al. RTR 4th. 2018',
    hdr_color=C_NAVY, title_fs=13.5, body_fs=9.8,
    hdr_h=0.122, linespacing=1.68)

# ══════════════════════════════════════════════════════════════
# 저장
# ══════════════════════════════════════════════════════════════
OUT = 'poster_90x120.png'
fig.savefig(OUT, dpi=DPI, bbox_inches='tight', facecolor=C_BG, format='png')
plt.close(fig)
print(f'Saved: {OUT}  ({W_IN*DPI:.0f} x {H_IN*DPI:.0f} px @ {DPI} dpi)')
