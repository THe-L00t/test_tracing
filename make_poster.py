"""make_poster.py
학술 연구 포스터 — PPT 스타일 리디자인
이태형 · 한국공학대학교 게임공학과
90 cm × 120 cm  @  120 DPI → 4252 × 5669 px
"""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from matplotlib.patches import Rectangle, FancyBboxPatch, Circle
from matplotlib.image import imread
import matplotlib.font_manager as fm
import numpy as np

# ── 폰트 설정 ─────────────────────────────────────────────────
_avail = {f.name for f in fm.fontManager.ttflist}
FONT_KO = 'DejaVu Sans'
for _kf in ('Malgun Gothic', 'NanumGothic', 'AppleGothic'):
    if _kf in _avail:
        FONT_KO = _kf
        break
plt.rcParams.update({
    'font.family':        FONT_KO,
    'axes.unicode_minus': False,
})

# ── 치수 ──────────────────────────────────────────────────────
W_IN = 90 / 2.54    # 35.43 in
H_IN = 120 / 2.54   # 47.24 in
DPI  = 120

# ── 색상 ──────────────────────────────────────────────────────
# 메인 팔레트
C_BG       = '#F0F4FA'   # 포스터 배경 (아이스 화이트)
C_NAVY     = '#0B1D3E'   # 타이틀 배경
C_BLUE     = '#1B4FCC'   # 섹션1 헤더
C_BLUE2    = '#1565C0'   # 진한 블루
C_TEAL     = '#0D6E5A'   # 섹션2 헤더
C_INDIGO   = '#283593'   # 초록/스탯 배경
C_PALE     = '#EBF1FC'   # 섹션 배경
C_PALE2    = '#F4FAF7'   # 메서드 섹션 배경
C_WHITE    = '#FFFFFF'
C_TEXT     = '#0D1B35'   # 본문 텍스트 (딥 네이비)
C_GRAY     = '#4A5568'   # 서브 텍스트
C_GOLD     = '#FFC107'   # 강조 골드
C_AMBER    = '#FF8F00'   # 짙은 골드
C_RED      = '#C0392B'
C_GREEN    = '#27AE60'
C_BORDER   = '#C5D3EE'   # 박스 테두리

# ── 인쇄 기준 폰트 크기 (pt) ──────────────────────────────────
# 90cm 포스터 / 120DPI 기준, 1m 시청거리 가독
FS_TITLE   = 54    # 제목
FS_AUTHOR  = 21    # 저자
FS_ABST    = 16    # 초록
FS_STAT_V  = 40    # 스탯 수치
FS_STAT_L1 = 13    # 스탯 라벨1
FS_STAT_L2 = 11    # 스탯 라벨2
FS_SEC_HDR = 20    # 섹션 헤더
FS_BODY    = 15    # 본문
FS_PIPE    = 12    # 파이프라인 박스
FS_FIG_T   = 17    # 그림 제목
FS_FIG_C   = 13    # 그림 캡션
FS_TBL_H   = 12    # 표 헤더
FS_TBL_B   = 11.5  # 표 본문
FS_REF     = 13    # 참고문헌


# ══════════════════════════════════════════════════════════════
# 유틸리티 함수
# ══════════════════════════════════════════════════════════════

def ax_card(ax, bg=C_WHITE, border=C_BORDER, radius=0.02, shadow=True):
    """라운드 카드 박스 + 그림자"""
    ax.set_xlim(0, 1); ax.set_ylim(0, 1); ax.axis('off')
    if shadow:
        ax.add_patch(FancyBboxPatch(
            (0.010, -0.012), 0.990, 0.998,
            boxstyle=f'round,pad={radius}',
            facecolor='#00000018', edgecolor='none',
            transform=ax.transAxes, clip_on=False, zorder=0))
    ax.add_patch(FancyBboxPatch(
        (0, 0), 1, 1,
        boxstyle=f'round,pad={radius}',
        facecolor=bg, edgecolor=border, linewidth=1.5,
        transform=ax.transAxes, clip_on=False, zorder=1))


def section_header(ax, title, color=C_BLUE, num=None,
                   hdr_h=0.088, title_fs=FS_SEC_HDR):
    """섹션 헤더 (번호 뱃지 + 타이틀 바)"""
    ax.add_patch(FancyBboxPatch(
        (0, 1 - hdr_h), 1, hdr_h,
        boxstyle='round,pad=0.0',
        facecolor=color, edgecolor='none',
        transform=ax.transAxes, clip_on=False, zorder=2))
    # 좌측 골드 라인
    ax.add_patch(Rectangle(
        (0, 1 - hdr_h), 0.008, hdr_h,
        facecolor=C_GOLD, transform=ax.transAxes,
        clip_on=False, zorder=3))
    # 번호 뱃지
    if num is not None:
        ax.add_patch(Circle(
            (0.046, 1 - hdr_h / 2), 0.026,
            facecolor='#FFFFFF30', edgecolor='none',
            transform=ax.transAxes, zorder=4))
        ax.text(0.046, 1 - hdr_h / 2, str(num),
                ha='center', va='center', transform=ax.transAxes,
                fontsize=title_fs - 2, fontweight='bold', color=C_WHITE, zorder=5)
    x_title = 0.085 if num is not None else 0.5
    ha_title = 'left' if num is not None else 'center'
    ax.text(x_title, 1 - hdr_h / 2, title,
            ha=ha_title, va='center', transform=ax.transAxes,
            fontsize=title_fs, fontweight='bold', color=C_WHITE, zorder=5)


def section_block(ax, title, body, num=None,
                  bg=C_PALE, hdr_color=C_BLUE,
                  hdr_h=0.088, title_fs=FS_SEC_HDR,
                  body_fs=FS_BODY, body_x=0.038, body_y=None,
                  linespacing=1.65):
    ax_card(ax, bg=bg)
    section_header(ax, title, color=hdr_color, num=num,
                   hdr_h=hdr_h, title_fs=title_fs)
    y0 = (1 - hdr_h - 0.020) if body_y is None else body_y
    ax.text(body_x, y0, body,
            ha='left', va='top', transform=ax.transAxes,
            fontsize=body_fs, color=C_TEXT, linespacing=linespacing,
            clip_on=True, zorder=2)


def img_card(ax, path, title=None, caption=None,
             title_fs=FS_FIG_T, cap_fs=FS_FIG_C):
    ax_card(ax, bg=C_WHITE)
    th = 0.068 if title   else 0.0
    ch = 0.058 if caption else 0.0
    if title:
        ax.text(0.50, 1.0 - th / 2, title,
                ha='center', va='center', transform=ax.transAxes,
                fontsize=title_fs, fontweight='bold', color=C_BLUE2)
    img = imread(path)
    ins = ax.inset_axes([0.006, ch + 0.006, 0.988, 1 - th - ch - 0.012])
    ins.imshow(img, aspect='auto'); ins.axis('off')
    if caption:
        ax.text(0.50, ch / 2, caption,
                ha='center', va='center', transform=ax.transAxes,
                fontsize=cap_fs, color=C_GRAY, style='italic')


def draw_pipeline(ax, y_bot=0.032, y_top=0.430):
    """2-Pass 파이프라인 다이어그램"""
    mid = (y_bot + y_top) / 2
    bh  = (y_top - y_bot) * 0.58
    bw  = 0.148

    # 배경 패널
    ax.add_patch(FancyBboxPatch(
        (0.008, y_bot - 0.008), 0.984, (y_top - y_bot) + 0.055,
        boxstyle='round,pad=0.010',
        facecolor='#DEEAFC', edgecolor='#93B5E8', linewidth=1.2,
        transform=ax.transAxes, clip_on=True, zorder=2))
    ax.text(0.50, y_top + 0.032, '[ 2-Pass 렌더링 파이프라인 ]',
            ha='center', va='bottom', transform=ax.transAxes,
            fontsize=FS_PIPE + 1, fontweight='bold', color=C_BLUE2, zorder=3)

    STEPS = [
        (0.020, '입력\n씬',           C_NAVY),
        (0.215, 'Pass 1\n저spp PT',   C_BLUE),
        (0.410, 'F(p)\n맵 생성',      C_TEAL),
        (0.605, 'Pass 2\n추가 레이',   C_BLUE),
        (0.800, '최종\n출력',          C_RED),
    ]
    for x0, lbl, color in STEPS:
        ax.add_patch(FancyBboxPatch(
            (x0, mid - bh / 2), bw, bh,
            boxstyle='round,pad=0.018',
            facecolor=color, edgecolor='white', linewidth=1.8,
            transform=ax.transAxes, clip_on=True, zorder=6))
        ax.text(x0 + bw / 2, mid, lbl,
                ha='center', va='center', transform=ax.transAxes,
                fontsize=FS_PIPE, color='white', fontweight='bold',
                linespacing=1.30, zorder=7)

    GAPS = [(0.168, 0.215), (0.363, 0.410), (0.558, 0.605), (0.753, 0.800)]
    for xs, xe in GAPS:
        ax.annotate('',
                    xy=(xe - 0.004, mid), xytext=(xs + 0.004, mid),
                    xycoords='axes fraction', textcoords='axes fraction',
                    arrowprops=dict(arrowstyle='->', color='#2C3E50', lw=2.4),
                    zorder=8)

    # F(p) > T 조건 레이블
    ax.text(0.681, mid + bh / 2 + 0.052, 'F(p) > T',
            ha='center', va='bottom', transform=ax.transAxes,
            fontsize=FS_PIPE + 0.5, color=C_RED,
            fontstyle='italic', fontweight='bold', zorder=8)
    ax.annotate('',
                xy=(0.681, mid + bh / 2 + 0.005),
                xytext=(0.681, mid + bh / 2 + 0.045),
                xycoords='axes fraction', textcoords='axes fraction',
                arrowprops=dict(arrowstyle='->', color=C_RED, lw=1.8), zorder=8)


def draw_results_table(ax):
    """실험 결과 표"""
    ax_card(ax, bg=C_PALE)
    section_header(ax, '3.  실험 결과', color=C_BLUE, num=3,
                   hdr_h=0.100, title_fs=FS_SEC_HDR)

    TABLE = [
        ('씬',       'Fresnel',      'Variance',     '배율',   'wPSNR'),
        ('Scene 4',  '8.0±0.17 ms',  '71.2±5.47 ms', '8.9x',  '—'),
        ('Scene 5',  '4.3±0.16 ms',  '29.5±2.74 ms', '6.8x',  '+4.86 dB'),
        ('Scene 6',  '13.0±0.93 ms', '13.6±0.98 ms', '1.0x',  '—'),
        ('Scene 7',  '5.2±0.17 ms',  '45.1±1.12 ms', '8.6x',  '—'),
        ('Scene 8',  '13.0±0.91 ms', '42.5±0.92 ms', '3.3x',  '+0.81 dB'),
        ('평균',     '8.7 ms',        '40.4 ms',      '5.73x', '+1.23 dB'),
    ]
    COL_X = [0.030, 0.230, 0.450, 0.680, 0.815]
    N     = len(TABLE)
    TOP   = 0.870
    ROW_H = (TOP - 0.060) / N

    for r, row in enumerate(TABLE):
        yc  = TOP - (r + 0.5) * ROW_H
        yb  = TOP - (r + 1)   * ROW_H

        if r == 0:
            ax.add_patch(Rectangle(
                (0.010, yb + 0.003), 0.980, ROW_H - 0.005,
                facecolor='#1B4FCC', alpha=0.88,
                transform=ax.transAxes, clip_on=True, zorder=3))
            fc, fw, fs = C_WHITE, 'bold', FS_TBL_H
        elif r == N - 1:
            ax.add_patch(Rectangle(
                (0.010, yb + 0.003), 0.980, ROW_H - 0.005,
                facecolor='#C7D9F9',
                transform=ax.transAxes, clip_on=True, zorder=3))
            fc, fw, fs = C_NAVY, 'bold', FS_TBL_H
        else:
            if r % 2 == 0:
                ax.add_patch(Rectangle(
                    (0.010, yb + 0.003), 0.980, ROW_H - 0.005,
                    facecolor='#F4F8FF',
                    transform=ax.transAxes, clip_on=True, zorder=3))
            fc, fw, fs = C_TEXT, 'normal', FS_TBL_B

        for c, (cell, cx) in enumerate(zip(row, COL_X)):
            ax.text(cx, yc, cell,
                    ha=('left' if c == 0 else 'center'), va='center',
                    transform=ax.transAxes,
                    fontsize=fs, color=fc, fontweight=fw, zorder=4)

    ax.text(0.50, 0.030,
            '60 fps 달성:  Fresnel-guided  5 / 5   vs   Variance-guided  1 / 5',
            ha='center', va='center', transform=ax.transAxes,
            fontsize=FS_TBL_H, color=C_BLUE2, fontweight='bold', zorder=4)


# ══════════════════════════════════════════════════════════════
# Figure & GridSpec
# ══════════════════════════════════════════════════════════════
fig = plt.figure(figsize=(W_IN, H_IN), dpi=DPI, facecolor=C_BG)

# 6 rows: title / abstract / stats / row1 / row2 / footer
# 3 cols: left(28%) / center(40%) / right(32%)
gs = gridspec.GridSpec(
    6, 3,
    figure=fig,
    left=0.018, right=0.982,
    top=0.990,  bottom=0.010,
    hspace=0.026, wspace=0.020,
    height_ratios=[0.092, 0.042, 0.052, 0.338, 0.310, 0.122],
    width_ratios=[0.28, 0.40, 0.32],
)


# ══════════════════════════════════════════════════════════════
# Row 0 : TITLE BANNER
# ══════════════════════════════════════════════════════════════
ax_t = fig.add_subplot(gs[0, :])
ax_t.set_facecolor(C_NAVY); ax_t.axis('off')

# 배경 레이어 (깊이감)
for yy, cc in [(0.00, '#0A1830'), (0.18, '#0D2040'), (0.55, C_NAVY)]:
    ax_t.add_patch(Rectangle((0, yy), 1, 0.20,
        facecolor=cc, transform=ax_t.transAxes,
        clip_on=False, zorder=1))
# 골드 상하 라인
ax_t.add_patch(Rectangle((0, 0.000), 1, 0.028,
    facecolor=C_GOLD, alpha=0.8,
    transform=ax_t.transAxes, clip_on=False, zorder=2))
ax_t.add_patch(Rectangle((0, 0.972), 1, 0.028,
    facecolor=C_GOLD, alpha=0.8,
    transform=ax_t.transAxes, clip_on=False, zorder=2))
# 좌우 수직 골드 바
for xx in [0.000, 0.993]:
    ax_t.add_patch(Rectangle((xx, 0), 0.007, 1,
        facecolor=C_GOLD,
        transform=ax_t.transAxes, clip_on=False, zorder=3))

ax_t.text(0.500, 0.70,
          '희소 Fresnel 경계 샘플링:\n'
          '실시간 경로 추적을 위한 자기조절형 지각 레이 예산 전략',
          ha='center', va='center', transform=ax_t.transAxes,
          fontsize=FS_TITLE, fontweight='bold', color=C_WHITE,
          linespacing=1.22, zorder=4)

ax_t.text(0.500, 0.11,
          '이 태 형   ·   한국공학대학교 게임공학과   ·   한국게임학회 2026 추계학술발표대회',
          ha='center', va='center', transform=ax_t.transAxes,
          fontsize=FS_AUTHOR, color='#A8C4E8', zorder=4)


# ══════════════════════════════════════════════════════════════
# Row 1 : ABSTRACT
# ══════════════════════════════════════════════════════════════
ax_ab = fig.add_subplot(gs[1, :])
ax_ab.set_facecolor('#1D2E7A'); ax_ab.axis('off')

ax_ab.add_patch(Rectangle((0, 0), 0.006, 1,
    facecolor=C_GOLD, transform=ax_ab.transAxes,
    clip_on=False, zorder=2))
ax_ab.add_patch(Rectangle((0.994, 0), 0.006, 1,
    facecolor=C_GOLD, transform=ax_ab.transAxes,
    clip_on=False, zorder=2))

ax_ab.text(0.014, 0.56, 'ABSTRACT',
           ha='left', va='center', transform=ax_ab.transAxes,
           fontsize=FS_ABST - 1, fontweight='bold',
           color=C_GOLD, zorder=3)
ax_ab.add_patch(Rectangle((0.086, 0.30), 0.0015, 0.44,
    facecolor='#5577CC', transform=ax_ab.transAxes,
    clip_on=False, zorder=2))
ax_ab.text(0.093, 0.56,
           'Fresnel 반사율 기반 2-Pass 자기조절형 레이 예산 전략을 제안한다. '
           'F(p) 우선순위 맵으로 고우선순위 픽셀에만 추가 레이를 투사하여, '
           'Variance-guided 대비 평균 5.73x 가속과 wPSNR +1.23 dB 향상을 달성하였다. '
           '사용자 실험(n=40, 2AFC)에서 Operating Envelope 내 씬의 유의미한 시각 선호(p=0.038)를 확인하였다.',
           ha='left', va='center', transform=ax_ab.transAxes,
           fontsize=FS_ABST, color='#DCE9FF', linespacing=1.20, zorder=3)


# ══════════════════════════════════════════════════════════════
# Row 2 : KEY STATS
# ══════════════════════════════════════════════════════════════
ax_s = fig.add_subplot(gs[2, :])
ax_s.set_facecolor(C_INDIGO); ax_s.axis('off')

STATS = [
    ('5.73x',    '평균 속도 향상',       'vs. Variance-guided'),
    ('5 / 5',    '60 fps 달성',          '전 씬 (Fresnel-guided)'),
    ('+1.23 dB', '평균 wPSNR 향상',      'Scene 5 최고 +4.86 dB'),
    ('p=0.038',  '사용자 선호 유의성',    'n=26, 2AFC *'),
]
for i, (val, lbl1, lbl2) in enumerate(STATS):
    xc = (i + 0.5) / len(STATS)
    if i:
        ax_s.axvline(i / len(STATS), color='#3B5BC8', lw=0.9)
    ax_s.text(xc, 0.70, val,
              ha='center', va='center', transform=ax_s.transAxes,
              fontsize=FS_STAT_V, fontweight='bold', color=C_GOLD)
    ax_s.text(xc, 0.32, lbl1,
              ha='center', va='center', transform=ax_s.transAxes,
              fontsize=FS_STAT_L1, color='#E8EFFF', fontweight='bold')
    ax_s.text(xc, 0.10, lbl2,
              ha='center', va='center', transform=ax_s.transAxes,
              fontsize=FS_STAT_L2, color='#9CAFD8')


# ══════════════════════════════════════════════════════════════
# Row 3 : Introduction + Method (left) | Fig 3 (center) | Fig 4 (right)
# ══════════════════════════════════════════════════════════════
gs_r3l = gridspec.GridSpecFromSubplotSpec(
    2, 1, subplot_spec=gs[3, 0],
    hspace=0.022, height_ratios=[0.42, 0.58],
)

# ── 서론 ───────────────────────────────────────────────────────
ax_intro = fig.add_subplot(gs_r3l[0])
section_block(ax_intro,
    '서론  (Introduction)', num=1,
    body=(
        '▷  GPU DXR 가속으로 실시간 경로 추적(PT)이\n'
        '   게임 그래픽스의 새로운 표준으로 부상.\n\n'
        '▷  핵심 병목 — 그레이징 경계면\n'
        '   유리 · 금속에서 레이당 분산 폭발\n'
        '   Scene 4: Variance-guided  71.2 ms\n'
        '             60 fps 예산(16.67 ms) 4.3× 초과\n\n'
        '▷  기존 Variance-guided [Schied \'17]의 한계\n'
        '   분산 추정 패스 자체에 수십 ms 소요\n'
        '   씬 의존적 비용 폭발 → 실시간 불가\n\n'
        '▷  핵심 통찰\n'
        '   Fresnel 항은 레이 분산이 클 픽셀을\n'
        '   렌더링 없이 사전 예측 가능\n'
        '   → 단일 경량 패스로 지각적 우선순위 맵 산출'
    ),
    bg=C_PALE, body_fs=FS_BODY, linespacing=1.62)

# ── 방법 ───────────────────────────────────────────────────────
ax_meth = fig.add_subplot(gs_r3l[1])
section_block(ax_meth,
    '방법  (Fresnel-Guided 2-Pass PT)', num=2,
    hdr_color=C_TEAL,
    body=(
        'F(p) 우선순위 맵  [Schlick, 1994 기반]\n'
        '  Fterm = (1 - N*V)^5\n'
        '  Fsurf = 1 - roughness\n'
        '  Fspec = metallic + 2 * glass\n'
        '  F(p)  = Fterm * Fsurf * Fspec\n\n'
        '자기조절 커버리지\n'
        '  A = |{p : F(p) > T}| / N\n'
        '  A ~< 5%    Operating Envelope (유효)\n'
        '  A  > 20%  Envelope 외부 (품질 중립)'
    ),
    bg=C_PALE2, body_fs=FS_BODY, body_y=0.875, linespacing=1.62)
draw_pipeline(ax_meth, y_bot=0.035, y_top=0.425)

# ── Fig 3 ──────────────────────────────────────────────────────
ax_f3 = fig.add_subplot(gs[3, 1])
img_card(ax_f3,
    'figures_paper/figure3_comparison.png',
    title='Fig. 3  |  품질 비교 — Scene 8  (Metal + Glass,  A = 0.9%)',
    caption='Fresnel-Guided: wPSNR +0.81 dB, Pass 2 = 0.52 ms   '
            'vs   Variance-Guided: +0.74 dB, 14.28 ms')

# ── Fig 4 ──────────────────────────────────────────────────────
ax_f4 = fig.add_subplot(gs[3, 2])
img_card(ax_f4,
    'figures_paper/figure4_gpu_timing.png',
    title='Fig. 4  |  GPU 프레임 타이밍  (10-frame mean ± σ)',
    caption='Fresnel-Guided: 전 씬 60 fps 달성   |   '
            'Variance-Guided: 4/5 씬 16.67 ms 초과')


# ══════════════════════════════════════════════════════════════
# Row 4 : Fig 1 (left) | Fig 2 (center) | Fig 5 (right)
# ══════════════════════════════════════════════════════════════
ax_f1 = fig.add_subplot(gs[4, 0])
img_card(ax_f1,
    'figures_paper/figure1_teaser.png',
    title='Fig. 1  |  F(p) 우선순위 맵  (Scene 5,  A = 0.2%)',
    caption='상: F(p) 맵 — 경계면 픽셀 강조   |   '
            '하: Fresnel-Guided PT 결과 (100 spp)')

ax_f2 = fig.add_subplot(gs[4, 1])
img_card(ax_f2,
    'figures_paper/figure2_self_regulation.png',
    title='Fig. 2  |  자기조절: Pass 2 비용 vs  F(p) 커버리지 A',
    caption='A ~< 5% (파란 영역): Fresnel 압도적 효율   |   '
            'A > 20% (빨간 영역): 비용 급증·성능 중립')

ax_f5 = fig.add_subplot(gs[4, 2])
img_card(ax_f5,
    'figures_paper/figure5_user_study.png',
    title='Fig. 5  |  2AFC 사용자 선호도 실험  (n = 40)',
    caption='Scene 5 vs Baseline: 69.2%  p=0.038 *   |   '
            'A > 20% 씬: 중립 결과 — 이론과 일치')


# ══════════════════════════════════════════════════════════════
# Row 5 : FOOTER — Results table | Conclusion | References
# ══════════════════════════════════════════════════════════════
gs_foot = gridspec.GridSpecFromSubplotSpec(
    1, 3, subplot_spec=gs[5, :],
    wspace=0.020, width_ratios=[0.36, 0.34, 0.30],
)

# ── 결과 표 ────────────────────────────────────────────────────
ax_res = fig.add_subplot(gs_foot[0])
draw_results_table(ax_res)

# ── 결론 ───────────────────────────────────────────────────────
ax_conc = fig.add_subplot(gs_foot[1])
section_block(ax_conc,
    '결론 및 향후 연구', num=4,
    hdr_color=C_TEAL,
    body=(
        '성과 요약\n'
        '  - 분산 계산 패스 완전 제거\n'
        '  - A ~< 5%: 5–9× 가속, 전 씬 60 fps\n'
        '  - wPSNR +1.23 dB, 유의미한 사용자 선호\n\n'
        '한계 및 향후 과제\n'
        '  - A > 20% 씬 (유리 비중 높음) 지원 제한\n'
        '    → 커버리지 적응형 임계값 T 자동 조정\n'
        '  - 동적 씬 시간적 안정성 검증 필요\n'
        '  - ReSTIR DI / GI와 통합 가능성 탐색'
    ),
    bg=C_PALE2, body_fs=FS_BODY, hdr_h=0.100, linespacing=1.65)

# ── 참고문헌 ────────────────────────────────────────────────────
ax_ref = fig.add_subplot(gs_foot[2])
section_block(ax_ref,
    '참고문헌',
    body=(
        '[1]  Bitterli et al. ReSTIR DI. SIG 2020\n'
        '[6]  Schlick. BRDF Approx. EG 1994\n'
        '[7]  Schied et al. SVGF. SIG 2017\n'
        '[8]  Andersson et al. FLIP. HPG 2020\n'
        '[9]  Walter et al. Microfacet. EGSR 2007\n'
        '[11] Veach & Guibas. MIS. SIG 1995\n'
        '[12] Pharr et al. PBRT 4th ed. 2023\n'
        '[13] Akenine-Moller et al. RTR 4th. 2018'
    ),
    hdr_color=C_NAVY, bg=C_PALE,
    body_fs=FS_REF, hdr_h=0.100, linespacing=1.70)


# ══════════════════════════════════════════════════════════════
# 저장
# ══════════════════════════════════════════════════════════════
OUT = 'poster_90x120.png'
fig.savefig(OUT, dpi=DPI, bbox_inches='tight', facecolor=C_BG, format='png')
plt.close(fig)
print(f'Saved: {OUT}  ({W_IN*DPI:.0f} x {H_IN*DPI:.0f} px @ {DPI} dpi)')
