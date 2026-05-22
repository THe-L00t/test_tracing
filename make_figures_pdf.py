"""
논문 Figure PDF 생성 — 각 그림 A4 페이지에 최대 크기로 삽입
figure1~5 순서, 캡션 포함
"""

from reportlab.lib.pagesizes import A4
from reportlab.lib.units import cm
from reportlab.lib import colors
from reportlab.platypus import (
    SimpleDocTemplate, Image as RLImage, Spacer,
    Paragraph, PageBreak, HRFlowable
)
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from PIL import Image as PILImage
import os

# ── 폰트 설정 (한글 지원) ──────────────────────────────────────
FONT_REGULAR = None
FONT_BOLD    = None

candidates = [
    (r'C:\Windows\Fonts\malgun.ttf',   r'C:\Windows\Fonts\malgunbd.ttf'),
    (r'C:\Windows\Fonts\NanumGothic.ttf', r'C:\Windows\Fonts\NanumGothicBold.ttf'),
]
for reg, bold in candidates:
    if os.path.exists(reg):
        pdfmetrics.registerFont(TTFont('KorFont',     reg))
        FONT_REGULAR = 'KorFont'
        if os.path.exists(bold):
            pdfmetrics.registerFont(TTFont('KorFontBold', bold))
            FONT_BOLD = 'KorFontBold'
        break

if FONT_REGULAR is None:
    FONT_REGULAR = 'Helvetica'
    FONT_BOLD    = 'Helvetica-Bold'

# ── 스타일 ────────────────────────────────────────────────────
styles = getSampleStyleSheet()

caption_style = ParagraphStyle(
    'Caption',
    fontName=FONT_REGULAR,
    fontSize=10,
    leading=14,
    alignment=TA_CENTER,
    textColor=colors.HexColor('#222222'),
    spaceAfter=6,
)
caption_bold = ParagraphStyle(
    'CaptionBold',
    parent=caption_style,
    fontName=FONT_BOLD or FONT_REGULAR,
    fontSize=10,
)
section_style = ParagraphStyle(
    'Section',
    fontName=FONT_BOLD or FONT_REGULAR,
    fontSize=13,
    leading=18,
    alignment=TA_LEFT,
    textColor=colors.HexColor('#1565C0'),
    spaceBefore=8,
    spaceAfter=4,
)

# ── 유틸 ─────────────────────────────────────────────────────
PAGE_W, PAGE_H = A4
MARGIN = 1.8 * cm
MAX_W  = PAGE_W  - 2 * MARGIN
MAX_H  = PAGE_H  - 6 * cm   # 캡션 공간 확보

def fit_image(path, max_w=MAX_W, max_h=MAX_H):
    """원본 비율 유지하며 페이지에 맞게 축소"""
    with PILImage.open(path) as img:
        iw, ih = img.size
    scale = min(max_w / iw, max_h / ih)
    return RLImage(path, width=iw * scale, height=ih * scale)

# ── 그림 목록 ─────────────────────────────────────────────────
figures = [
    {
        'file':    'figure1_teaser.png',
        'num':     '그림 1',
        'caption': ('Scene 5 (Cornell Box, A = 0.2%) Teaser. '
                    '좌: F(p) 우선순위 맵 (밝을수록 Pass 2 추가 샘플 대상). '
                    '우: Fresnel-Guided PT 결과 (100 spp).'),
    },
    {
        'file':    'figure2_self_regulation.png',
        'num':     '그림 2',
        'caption': ('Pass 2 비용 자기조절 특성. '
                    'X축: 고-F(p) 픽셀 비율 A(%), Y축: Pass 2 단독 GPU 시간(ms). '
                    'Fresnel-Guided(파랑 원)는 A에 비례해 비용이 증가하나, '
                    'Variance-Guided(빨강 사각)는 A와 무관하게 9–24 ms를 소모한다. '
                    '파란 음영: Operating Envelope (A ≲ 5%), '
                    '붉은 음영: Envelope 외부 (A > 20%).'),
    },
    {
        'file':    'figure3_comparison.png',
        'num':     '그림 3',
        'caption': ('Scene 8 (Metal + Glass, A = 0.9%) 화질 비교. '
                    '상단: Baseline 100 spp / Fresnel-Guided 100 spp / Reference 10 000 spp. '
                    '하단: 각 방법의 오차 맵 (밝을수록 오차 큼).'),
    },
    {
        'file':    'figure4_gpu_timing.png',
        'num':     '그림 4',
        'caption': ('씬별 GPU 프레임 타이밍 비교 (10프레임 평균 ± σ). '
                    'Fresnel-Guided(파랑)는 5개 씬 모두 60 fps 기준(16.67 ms) 이하. '
                    'Variance-Guided(빨강)는 1/5 씬만 기준 충족. '
                    '숫자는 Variance 대비 Fresnel의 속도 배율.'),
    },
    {
        'file':    'figure5_user_study.png',
        'num':     '그림 5',
        'caption': ('2AFC 사용자 설문 결과 (n = 40, 결정적 응답 기준). '
                    'Scene 5 F vs Baseline: Fresnel 선호 69.2%, p = 0.038 (유의). '
                    '파란 음영: Operating Envelope 내부 (A ≲ 5%), '
                    '붉은 음영: 외부 (A > 20%). '
                    'Envelope 외부(Scene 7, A = 29.1%)에서는 유의한 차이 없음 — 예측과 일치.'),
    },
]

# ── PDF 빌드 ──────────────────────────────────────────────────
OUT = '논문_figures.pdf'
doc = SimpleDocTemplate(
    OUT,
    pagesize=A4,
    leftMargin=MARGIN, rightMargin=MARGIN,
    topMargin=MARGIN,  bottomMargin=MARGIN,
)

story = []

for i, fig in enumerate(figures):
    if not os.path.exists(fig['file']):
        print(f"  SKIP (not found): {fig['file']}")
        continue

    story.append(Paragraph(fig['num'], section_style))
    story.append(Spacer(1, 0.3 * cm))
    story.append(fit_image(fig['file']))
    story.append(Spacer(1, 0.4 * cm))
    story.append(HRFlowable(width='100%', thickness=0.5,
                             color=colors.HexColor('#CCCCCC')))
    story.append(Spacer(1, 0.25 * cm))
    caption_text = f"<b>{fig['num']}.</b>  {fig['caption']}"
    story.append(Paragraph(caption_text, caption_style))

    if i < len(figures) - 1:
        story.append(PageBreak())

doc.build(story)
print(f"saved: {OUT}")
