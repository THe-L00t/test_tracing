# A Gentler Introduction to ReSTIR

**출처**: Interplay of Light 블로그  
**URL**: https://interplayoflight.wordpress.com/2023/12/17/a-gentler-introduction-to-restir/  
**작성일**: 2023-12-17

---

## 핵심 문제 정의

씬에 수백 개의 그림자 드리우는 광원(shadowed lights)이 있을 때  
픽셀당 1개의 shadow ray만 사용하면서 고품질 결과를 얻는 방법.

전통적 방법:
- 모든 광원에 shadow ray → 400개 광원 = 400 rays/pixel → 비현실적
- 무작위 1개 광원 샘플링 → 노이즈 극심

ReSTIR의 해법: **Weighted Reservoir Sampling**으로 "가장 중요한" 광원을 선택하고,  
이전 프레임 및 이웃 픽셀과 결과를 공유.

---

## Reservoir 구조 (HLSL)

```hlsl
struct Reservoir
{
    uint  Y;       // 현재 선택된 광원 인덱스
    float W_y;     // 이 광원의 기여 가중치 (최종 셰이딩에 사용)
    float W_sum;   // 지금까지 처리한 가중치 합
    float M;       // 처리한 후보 수
};
```

---

## Stage 1: Initial Reservoir Sampling

픽셀당 M개의 광원을 무작위로 선택하고 WRS로 Reservoir 구성.

```hlsl
Reservoir r = (Reservoir)0;

for (int i = 0; i < M; i++)
{
    uint lightIdx = selectRandomLight(rand());
    float3 Li = sampleLight(lightIdx, surfacePos);   // 방사도
    float p_hat = length(Li);                         // target PDF 근사
    float p     = 1.0 / numLights;                   // source PDF (균등)
    float w     = p_hat / p;

    // WRS 업데이트
    r.W_sum += w;
    r.M     += 1;
    if (rand() < w / r.W_sum)
        r.Y = lightIdx;
}

// 선택된 광원의 최종 가중치
float p_hat_y = length(sampleLight(r.Y, surfacePos));
r.W_y = (p_hat_y > 0) ? (r.W_sum / (r.M * p_hat_y)) : 0;

// Shadow ray 1개 발사
if (!isVisible(surfacePos, getLightPos(r.Y)))
    r.W_y = 0;
```

---

## Stage 2: Temporal Reuse

이전 프레임의 Reservoir를 현재 Reservoir와 결합.  
모션 벡터로 이전 프레임의 픽셀 위치를 추적.

```hlsl
// 이전 프레임 Reservoir 로드
Reservoir prev = loadPrevReservoir(reprojectedUV);

// 호환성 검사
if (dot(prevNormal, currNormal) > cos(25°) && abs(prevDepth - currDepth) / currDepth < 0.1)
{
    // M clamp: 폭주 방지
    prev.M = min(prev.M, 20 * curr.M);

    // 두 Reservoir 결합
    Reservoir combined = curr;
    float p_hat_prev = computeTargetPDF(prev.Y, currSurface); // 현재 표면 기준 재평가
    combineReservoirs(combined, prev, p_hat_prev, rand());

    curr = combined;
}
```

**M clamp 이유**: 시간이 흐를수록 M이 무한정 증가하면 새로운 정보(광원 변화)에 반응하지 못함.

---

## Stage 3: Spatial Reuse

이웃 픽셀 K개의 Reservoir를 결합. 보통 30픽셀 반경, K=5개 이웃.

```hlsl
Reservoir combined = curr;

for (int i = 0; i < K; i++)
{
    float2 neighborUV = curr.uv + sampleDisk(30px, rand());
    Reservoir neighbor = loadReservoir(neighborUV);

    // 호환성 검사
    if (!isCompatible(neighborNormal, neighborDepth, currNormal, currDepth))
        continue;

    // 이웃 Reservoir의 광원을 현재 표면에서 재평가
    float p_hat = computeTargetPDF(neighbor.Y, currSurface);
    combineReservoirs(combined, neighbor, p_hat, rand());
}

curr = combined;
```

---

## Reservoir 결합 함수

```hlsl
void combineReservoirs(inout Reservoir r, Reservoir r2, float p_hat_r2y, float rand)
{
    int savedM = r.M;

    float w = p_hat_r2y * r2.W_y * r2.M;
    r.W_sum += w;
    r.M     += r2.M;

    if (rand < w / r.W_sum)
        r.Y = r2.Y;

    // 최종 가중치 갱신
    float p_hat_y = computeTargetPDF(r.Y, currSurface);
    r.W_y = (p_hat_y > 0) ? (r.W_sum / (r.M * p_hat_y)) : 0;
}
```

---

## 결과 비교 (400개 점광원)

| 방법 | shadow rays/pixel | 결과 품질 |
|---|---|---|
| 기준 (32샘플 stochastic) | 32 | 노이즈 있지만 고품질 |
| single shadow ray | 1 | 노이즈 많음 |
| + Temporal Reuse | 1 | 노이즈 크게 감소 |
| + Spatial Reuse | 1 | 추가 개선 |
| + TAA | 1 | 깔끔한 최종 결과 |

---

## 구현 팁

1. **버퍼 배치**: Reservoir를 UAV(RWTexture2D 또는 RWStructuredBuffer)로 저장.  
   더블 버퍼링으로 현재/이전 프레임 분리.

2. **타겟 PDF 선택**: `p_hat(y) = |L_e(y) · f(y) · |cos θ||` 형태가 일반적.  
   가시성 항은 초기 Reservoir에서는 제외, 이후 별도 shadow ray로 처리.

3. **Temporal 안정성**: M clamp 없으면 ghost 아티팩트 발생.  
   보통 `prev.M = min(prev.M, 20 * curr.M)` 수준.

4. **Spatial 편향**: 이웃 픽셀의 광원을 현재 표면에서 재평가해야 함.  
   이웃의 `W_y` 그대로 사용하면 편향 발생.
