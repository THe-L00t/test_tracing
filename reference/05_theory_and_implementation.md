# ReSTIR 이론 및 DXR 구현 상세

**출처**: Shubham Sachdeva Blog  
**URL**: https://gamehacker1999.github.io/posts/restir/

---

## 수학적 기반

### 렌더링 방정식

```
L_o(x, ω_o) = ∫ f(x, ω_i, ω_o) · L_i(x, ω_i) · |cos θ_i| dω_i
```

직접 조명에서 `L_i`는 각 광원의 기여. 광원이 N개라면:

```
L_o ≈ Σ_{k=1}^{N} f(x, ω_k) · L_e(y_k) · G(x, y_k) · V(x, y_k)
```

이를 몬테카를로로 추정할 때 광원 수 N이 많으면 샘플당 비용 폭증.

---

## Resampled Importance Sampling (RIS)

최적 샘플링 분포 `p*(y) ∝ f(y)·L_e(y)·G(y)·V(y)` 에서 직접 샘플링 불가.  
대신 source PDF `p(y)`에서 M개를 뽑고, 가중치로 target에 가까운 샘플 선택:

```
w(y_i) = p_hat(y_i) / p(y_i)

RIS 추정기: (1/M) · (1/p_hat(y)) · Σ w(y_i) · f(y)·L_e(y)·G(y)·V(y)
```

### 직접 조명에서의 p_hat

```
p_hat(y) = |f(y)| · |L_e(y)| · |cos θ|
```
가시성 V는 비용이 크므로 초기 p_hat에서 제외, 나중에 shadow ray로 처리.

---

## Weighted Reservoir Sampling (WRS)

스트리밍 방식으로 M개 처리. 메모리 O(1).

```
알고리즘:
  R = 빈 Reservoir
  for each sample x_i (i = 1..M):
      w_i = target(x_i) / source(x_i)
      R.W_sum += w_i
      R.M     += 1
      if rand() < w_i / R.W_sum:
          R.y = x_i   // 더 중요한 샘플로 교체
  
  R.W = R.W_sum / (R.M * target(R.y))
```

**보장**: `R.y`는 확률 `w_i / W_sum`으로 선택 → RIS와 수학적으로 동등.

---

## DXR 구현 세부사항 (Shubham의 구현)

### 파라미터 설정

```hlsl
#define M           32    // 초기 후보 광원 수
#define K           5     // 공간적 이웃 수
#define RADIUS      30    // 공간적 탐색 반경 (픽셀)
#define MAX_M_RATIO 20    // Temporal M clamp 비율
```

### Reservoir UAV 버퍼

```hlsl
// 현재 프레임 Reservoir
RWTexture2D<float4> gCurrentReservoirs   : register(u0);
// 이전 프레임 Reservoir (Temporal용)
Texture2D<float4>   gPreviousReservoirs  : register(t0);

// float4 패킹: (y_as_float, W_sum, M_as_float, W)
```

### 점광원 기여 계산 (p_hat)

```hlsl
float computeTargetPDF(uint lightIdx, float3 pos, float3 normal)
{
    float3 lightPos = gLights[lightIdx].position;
    float3 toLight  = lightPos - pos;
    float  dist2    = dot(toLight, toLight);
    float3 dir      = normalize(toLight);
    
    float3 Li = gLights[lightIdx].intensity / dist2;   // 역제곱 감쇠
    float  cosTheta = max(0, dot(normal, dir));
    
    return length(Li) * cosTheta;   // Lambertian p_hat
}
```

### Reservoir 업데이트

```hlsl
bool updateReservoir(inout Reservoir r, uint lightIdx,
                     float3 pos, float3 normal, inout RngState rng)
{
    float  p_hat = computeTargetPDF(lightIdx, pos, normal);
    float  p     = 1.0 / gNumLights;    // 균등 source PDF
    float  w     = p_hat / p;

    r.W_sum += w;
    r.M     += 1;

    if (nextRand(rng) < w / r.W_sum)
    {
        r.y = lightIdx;
        return true;
    }
    return false;
}
```

### Temporal Reuse 패스

```hlsl
[shader("pixel")]
float4 temporalReusePS(VS_OUT input) : SV_Target
{
    // 현재 Reservoir 로드
    Reservoir curr = loadReservoir(gCurrentReservoirs, input.uv);
    
    // 모션 벡터로 이전 픽셀 위치 계산
    float2 prevUV = input.uv - gMotionVectors.Sample(input.uv);
    Reservoir prev = loadReservoir(gPreviousReservoirs, prevUV);

    // 호환성 검사
    float3 prevNormal = gPrevNormals.Sample(prevUV);
    float  prevDepth  = gPrevDepth.Sample(prevUV);

    if (dot(currNormal, prevNormal) > 0.9063  // cos(25°)
     && abs(currDepth - prevDepth) / currDepth < 0.1)
    {
        // M clamp
        prev.M = min(prev.M, MAX_M_RATIO * curr.M);

        // 결합
        Reservoir combined = curr;
        float p_hat = computeTargetPDF(prev.y, currPos, currNormal);
        combineReservoirs(combined, prev, p_hat, rng);
        curr = combined;
    }

    storeReservoir(gCurrentReservoirs, input.uv, curr);
    return 0;
}
```

### Spatial Reuse 패스

```hlsl
[shader("pixel")]
float4 spatialReusePS(VS_OUT input) : SV_Target
{
    Reservoir combined = loadReservoir(gCurrentReservoirs, input.uv);
    int acceptedNeighbors = 0;

    for (int i = 0; i < K; i++)
    {
        float2 offset    = sampleDisk(RADIUS, rng);
        float2 neighborUV = input.uv + offset / gScreenSize;
        
        if (neighborUV.x < 0 || neighborUV.x > 1 || ...) continue;

        float3 nNormal = gNormals.Sample(neighborUV);
        float  nDepth  = gDepth.Sample(neighborUV);

        if (dot(currNormal, nNormal) < 0.9063) continue;
        if (abs(currDepth - nDepth) / currDepth > 0.1) continue;

        Reservoir neighbor = loadReservoir(gCurrentReservoirs, neighborUV);
        float p_hat = computeTargetPDF(neighbor.y, currPos, currNormal);

        combineReservoirs(combined, neighbor, p_hat, rng);
        acceptedNeighbors++;
    }

    // 비편향 보정 (선택)
    // combined.W_sum /= (float)(acceptedNeighbors + 1);

    storeReservoir(gCurrentReservoirs, input.uv, combined);
    return 0;
}
```

---

## 구현 결과

| 씬 | 광원 수 | 방법 | 품질 |
|---|---|---|---|
| 테스트 씬 | 2,000+ 점광원 | 기본 1-sample | 극심한 노이즈 |
| 테스트 씬 | 2,000+ 점광원 | Temporal only | 노이즈 대폭 감소 |
| 테스트 씬 | 2,000+ 점광원 | Temporal+Spatial | 디노이저 수준 접근 |

---

## 중요 구현 주의사항

1. **Double Buffering 필수**: Spatial reuse 시 같은 버퍼에서 읽고 쓰면 안 됨.  
   현재 패스용 버퍼와 이전 패스용 버퍼를 분리.

2. **p_hat 재평가**: 이웃의 Reservoir를 결합할 때 이웃 표면이 아닌  
   **현재 표면 기준**으로 p_hat 재계산 필수. 그렇지 않으면 편향.

3. **가시성 처리 시점**: 가시성(V)은 초기 RIS 단계가 아닌 별도 패스에서  
   shadow ray로 처리. 가시성 실패 시 `W = 0`.

4. **Temporal 안정성**: M이 너무 커지면 씬 변화에 반응 못함.  
   M clamp는 반드시 적용 (20× 정도).
