# Spatiotemporal Reservoir Resampling for Real-Time Ray Tracing with Dynamic Direct Lighting

**저자**: Benedikt Bitterli, Chris Wyman, Matt Pharr, Peter Shirley, Aaron Lefohn, Wojciech Jarosz  
**발표**: ACM Transactions on Graphics (SIGGRAPH 2020)  
**DOI**: 10.1145/3386569.3392481

## 링크

- 프로젝트 페이지: https://benedikt-bitterli.me/restir/
- NVIDIA Research: https://research.nvidia.com/labs/rtr/publication/bitterli2020spatiotemporal/
- PDF: https://research.nvidia.com/sites/default/files/pubs/2020-07_Spatiotemporal-reservoir-resampling/ReSTIR.pdf

---

## 핵심 기여

수백만 개의 동적 광원이 있는 씬에서 실시간 직접 조명을 렌더링하는 알고리즘.  
복잡한 자료구조 없이 GPU에서 인터랙티브하게 동작.

## 성능

- Zero Day 씬: 동적 삼각형 광원 11,000개 → 15ms @ 1920×1080
- Amusement Park 씬: 동적 삼각형 광원 340만 개 → 50ms @ 1920×1080
- 동일 오차 기준 최신 기법 대비 **6~60배 빠름** (비편향 추정기)
- 편향 변형: **35~65배 빠름** (약간의 에너지 손실 허용)
- 픽셀당 최대 8개 ray만 사용

## 알고리즘 개요

### 1. Resampled Importance Sampling (RIS)

렌더링 방정식의 피적분 함수(BSDF × 방사도 × 가시성 × 기하 항)를 직접 샘플링하기 어렵기 때문에,  
서브옵티말한 source PDF에서 M개의 후보를 생성한 뒤 target PDF 비율로 하나를 선택.

```
target = f(y) · L_e(y) · G(y) · V(y)   // 이상적 샘플링 대상
source = p(y)                            // 실제 샘플링 분포 (예: 균등 분포)
weight = target(y) / source(y)
```

### 2. Weighted Reservoir Sampling (WRS)

스트리밍 방식으로 M개의 후보를 한 번에 처리.  
각 후보를 순서대로 보면서 누적 가중치 대비 확률로 현재 후보를 선택.

```
Reservoir:
  y     = 현재 선택된 광원 인덱스
  W_sum = 지금까지 본 가중치의 합
  M     = 지금까지 본 후보 수
  W     = 확률 보정 가중치 (= W_sum / (M * p_hat(y)))
```

**Reservoir 결합**: 두 Reservoir R1, R2를 합칠 때

```
R.update(R2.y, p_hat(R2.y) * R2.W * R2.M)
R.M += R2.M
R.W = R.W_sum / (R.M * p_hat(R.y))
```

### 3. 시공간 재사용 (Spatiotemporal Reuse)

**Temporal Reuse (시간적 재사용)**  
- 모션 벡터로 이전 프레임에서 같은 픽셀의 Reservoir를 찾아 현재 Reservoir와 결합.
- 시간이 쌓일수록 효과적인 샘플 수 M이 증가 → 노이즈 감소.
- M의 무한 증가를 막기 위해 clamp 적용 (보통 20×M_current).

**Spatial Reuse (공간적 재사용)**  
- 화면 공간에서 반경 r 내의 이웃 픽셀 K개를 샘플링하여 Reservoir 결합.
- 호환성 검사: 법선 벡터 차이 < 25°, 깊이 차이 < 10% 인 경우만 결합.
- 보통 2회 반복 적용.

### 4. 편향 문제

공간적 재사용 시, 이웃 픽셀의 Reservoir는 다른 기하 조건에서 생성됐으므로  
그대로 결합하면 편향(bias) 발생.

**편향 보정법:**
- 기하 조건이 크게 다른 이웃은 reject (정규/깊이 임계값)
- 비편향 보정: 결합 시 M값 대신 실제 유효한 이웃 수로 나눔 (계산 비용 증가)

---

## ReSTIR DI 렌더링 패스 순서

```
1. G-Buffer 생성 (위치, 법선, 재질)
2. Initial RIS (픽셀당 M개 광원 후보 → Reservoir 1개 선택)
3. 가시성 검사 (선택된 광원에 shadow ray 1개)
4. Temporal Reuse (이전 프레임 Reservoir 결합)
5. Spatial Reuse × 2 (이웃 픽셀 Reservoir 결합)
6. Final Shading (최종 Reservoir로 셰이딩)
```

---

## 관련 후속 연구

- **ReSTIR GI** (Ouyang et al.) — 간접 조명으로 확장
- **ReSTIR PT** (Lin et al.) — 전체 경로 추적으로 확장
- **RTXDI** — NVIDIA 프로덕션 SDK
