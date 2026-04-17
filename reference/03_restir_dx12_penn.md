# ReSTIR DX12 — UPenn CIS565 구현

**GitHub**: https://github.com/lindayukeyi/ReSTIR_DX12  
**과목**: University of Pennsylvania CIS565 (GPU Programming)  
**프레임워크**: NVIDIA Falcor 3.1.0  
**기반**: Chris Wyman's "A Gentle Introduction to DirectX Raytracing" 튜토리얼

---

## 프로젝트 개요

수백만 광원이 있는 씬에서 효율적인 광원 샘플링을 위해 ReSTIR를 DXR로 구현.  
테스트 환경: RTX 2070 Max-Q, "Pub" 씬 (삼각형 989,392개, 점광원 130개, 면광원 5개)

---

## 렌더링 패스 구조 (6패스)

```
패스 1: RayTraced G-Buffer & RIS
패스 2: Visibility Testing (Shadow Ray)
패스 3: Temporal Reuse
패스 4: Spatial Reuse (1회차)
패스 5: Spatial Reuse (2회차)   ← 선택적
패스 6: Final Shading
(선택): A-Trous Denoiser
```

### 패스별 상세

**패스 1 — RayTraced G-Buffer & RIS**  
레이트레이싱 셰이더 사용.
- 픽셀당 M=32개 점광원 후보를 균등 샘플링
- 각 후보의 p_hat(y) = |f(y) · L_e(y) · G(y)| 계산 (가시성 제외)
- WRS로 Reservoir 구성
- G-Buffer: 위치, 법선, diffuse albedo, 재질 정보 기록

**패스 2 — Visibility Testing**  
선택된 광원에 shadow ray 1개 발사.  
가시성이 0이면 Reservoir.W = 0으로 설정 (기여 없음).

**패스 3 — Temporal Reuse**  
픽셀 셰이더 사용.
- 모션 벡터로 이전 프레임 위치 계산
- 이전 Reservoir를 현재와 결합
- 법선 차이 < 25°, 깊이 차이 < 10% 조건 검사
- M clamp: 현재 M의 20배 상한

**패스 4, 5 — Spatial Reuse**  
픽셀 셰이더 사용.
- 반경 30픽셀 내에서 K=5개 이웃 무작위 선택
- 동일한 법선/깊이 호환성 검사 후 결합

**패스 6 — Final Shading**  
최종 Reservoir의 광원으로 BRDF 계산 및 쉐이딩.

---

## Reservoir 구조 (HLSL)

```hlsl
struct Reservoir
{
    int    y;      // 선택된 광원 인덱스 (-1: 없음)
    float  W_sum;  // 누적 가중치 합
    int    M;      // 처리한 후보 수
    float  W;      // 확률 보정 가중치
};
```

## 핵심 함수

```hlsl
// Reservoir 업데이트 (WRS 한 스텝)
bool updateReservoir(inout Reservoir r, int lightIdx, float weight, float rand)
{
    r.W_sum += weight;
    r.M += 1;
    if (rand < weight / r.W_sum)
    {
        r.y = lightIdx;
        return true;
    }
    return false;
}

// 두 Reservoir 결합
void combineReservoirs(inout Reservoir r, Reservoir r2, float p_hat_r2y, float rand)
{
    int M0 = r.M;
    updateReservoir(r, r2.y, p_hat_r2y * r2.W * r2.M, rand);
    r.M = M0 + r2.M;
}

// 최종 가중치 계산
void finalizeReservoir(inout Reservoir r, float p_hat)
{
    r.W = (p_hat > 0.0) ? (r.W_sum / (r.M * p_hat)) : 0.0;
}
```

---

## 성능 병목

- **패스 1 (Initial RIS)** 이 전체 실행 시간의 ~15ms 차지 (RTX 2070 Max-Q 기준)
- 광원 수 증가 시 무작위 광원 샘플링의 메모리 접근 패턴이 성능 저하 주요 원인
- Spatial Reuse 2회 적용 시 Temporal Reuse의 ~2배 시간 소요

## 한계

- 면광원/점광원만 지원 (메시 광원 미지원)
- Lambertian 재질만 지원 (GGX 등 복잡한 BRDF 미지원)
- 편향 알고리즘 (비편향 버전은 계산 비용이 더 큼)
- Global Illumination 미지원
