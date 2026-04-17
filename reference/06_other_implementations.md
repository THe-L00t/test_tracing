# 기타 ReSTIR 구현체 및 레퍼런스

---

## GitHub 구현체 목록

### 1. tatran5/Reservoir-Spatio-Temporal-Importance-Resampling-ReSTIR

**URL**: https://github.com/tatran5/Reservoir-Spatio-Temporal-Importance-Resampling-ReSTIR  
**특징**: 팀 프로젝트, DirectX Raytracing 기반, Global Illumination 포함

**구현 내용**:
- Candidate Generation: 32개 후보 중 1개 선택 + visibility 검사
- Temporal Reuse: M clamp = 20× 적용
- Spatial Reuse: 지정 반경 내 이웃 Reservoir 결합
- Final Shading: shadow ray + BRDF 계산

**Reservoir 구조**:
```
- W_sum: 누적 가중치 합
- M:     처리한 후보 수
- y:     선택된 광원 인덱스
- W:     정규화 가중치 (= W_sum / (M * p_hat(y)))
```

**가중치 계산**:
```
w(x) = p_hat(x) / p(x)
     = (BSDF · L_e · |cos θ| / dist²) / (1/N_lights)
```

**빌드 요구사항**:
- Windows 10 RS5+ (Developer Mode)
- DirectX Raytracing 지원 GPU (NVIDIA 416.xx+ 드라이버)
- Visual Studio 2019
- Windows SDK 10.0.17763.0

---

### 2. karel-tomanec/Falcor-ReSTIR

**URL**: https://github.com/karel-tomanec/Falcor-ReSTIR  
**특징**: 석사 논문 프로젝트, Falcor 5.x 기반, Render Pass로 캡슐화

**구현 위치**: `Source/RenderPasses/ReSTIRPass`

**실행 방법**:
```
1. Mogwai (Falcor 뷰어) 실행
2. Source/Mogwai/Data/ReSTIR.py 로드
3. 씬 임포트 (ORCA 또는 테스트 씬)
4. ReSTIR 파라미터 조정
```

**요구사항**:
- Windows 10 build 20H2+
- Visual Studio 2019 or 2022
- DXR 지원 GPU
- NVIDIA 드라이버 466.11+

---

### 3. meganr28 — DXR Path Tracer with ReSTIR

**URL**: https://meganr28.github.io/code/dxrpathtracer/  
**특징**: Falcor 기반 DXR 경로 추적기에 ReSTIR 통합, A-Trous 디노이저 포함

**파이프라인**:
```
G-Buffer (raytraced) → Initial RIS → Visibility Reuse → Temporal Reuse → Spatial Reuse → A-Trous Denoiser
```

---

### 4. Alegruz/Screen-Space-ReSTIR-GI

**URL**: https://github.com/Alegruz/Screen-Space-ReSTIR-GI  
**특징**: ReSTIR GI (간접 조명) 구현, Falcor 5.1 기반  
*직접 조명이 아닌 GI 확장이므로 참고용*

---

## 유용한 글 및 튜토리얼

### A Gentler Introduction to ReSTIR
**URL**: https://interplayoflight.wordpress.com/2023/12/17/a-gentler-introduction-to-restir/  
→ `04_gentler_introduction.md` 참조

### Spatiotemporal Reservoir Resampling (ReSTIR) - Theory and Basic Implementation
**URL**: https://gamehacker1999.github.io/posts/restir/  
→ `05_theory_and_implementation.md` 참조

### A Gentle Introduction to ReSTIR Path Reuse in Real-Time (Chris Wyman)
**URL**: https://par.nsf.gov/servlets/purl/10519651  
ReSTIR 개념을 처음 접하는 사람을 위한 입문 강의 자료.

### Wikipedia - Spatiotemporal Reservoir Resampling
**URL**: https://en.wikipedia.org/wiki/Spatiotemporal_reservoir_resampling  
개요, 역사, 변형 알고리즘 정리.

---

## 원본 논문 및 공식 자료

| 논문 | 링크 |
|---|---|
| Bitterli et al. 2020 (ReSTIR DI) | https://benedikt-bitterli.me/restir/ |
| NVIDIA Research 페이지 | https://research.nvidia.com/labs/rtr/publication/bitterli2020spatiotemporal/ |
| 논문 PDF | https://research.nvidia.com/sites/default/files/pubs/2020-07_Spatiotemporal-reservoir-resampling/ReSTIR.pdf |
| ACM DL | https://dl.acm.org/doi/abs/10.1145/3386569.3392481 |
| ReSTIR GI 논문 | https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing |

---

## 현재 프로젝트와의 비교

| 항목 | 현재 프로젝트 | lindayukeyi/ReSTIR_DX12 |
|---|---|---|
| API | D3D12 + DXR (raw) | Falcor 3.1 |
| 재질 | GGX BRDF | Lambertian only |
| 패스 구조 | GBuffer → RIS → Temporal → Spatial → Shade | 동일 (6패스) |
| 디노이저 | A-Trous | A-Trous (선택) |
| 광원 종류 | (구현 중) | 점광원 + 면광원 |
