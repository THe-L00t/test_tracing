# NVIDIA RTXDI SDK

**GitHub**: https://github.com/NVIDIA-RTX/RTXDI  
**버전**: 3.0.0  
**지원 API**: D3D12 + Vulkan (NVRHI 추상화 레이어)

---

## 개요

RTXDI(RTX Dynamic Illumination)는 ReSTIR 알고리즘 기반의 NVIDIA 공식 실시간 동적 조명 SDK.  
D3D12와 Vulkan을 모두 지원하며, HLSL → SPIR-V 크로스 컴파일도 지원.

## 포함 알고리즘

| 알고리즘 | 버전 | 기반 논문 |
|---|---|---|
| ReSTIR DI | 1.0 | Bitterli et al. 2020 |
| ReSTIR GI | 2.0 | Ouyang et al. "ReSTIR GI" |
| ReSTIR PT | 3.0 | Lin et al. "Generalized Resampled Importance Sampling" |

## 디렉터리 구조

```
RTXDI/
├── Libraries/
│   └── Rtxdi/          # 핵심 라이브러리 (셰이더 include + 호스트 유틸리티)
│       ├── Include/
│       │   ├── Rtxdi/
│       │   │   ├── ReSTIRDI.hlsli      # DI 알고리즘 셰이더
│       │   │   ├── ReSTIRGI.hlsli      # GI 알고리즘 셰이더
│       │   │   └── Reservoir.hlsli     # Reservoir 구조체
│       │   └── ...
│       └── ...
├── Samples/
│   ├── MinimalSample/  # ★ 최소 단일 패스 ReSTIR DI 구현 (학습용)
│   └── FullSample/     # 전체 파이프라인 구현
└── External/
    ├── Donut/          # 렌더링 프레임워크
    ├── NRD/            # NVIDIA 디노이저
    └── DLSS/           # DLSS 지원
```

## MinimalSample 특징

학습용으로 설계된 단일 패스(single combined pass) ReSTIR DI 구현.  
- 최소한의 코드로 ReSTIR DI의 핵심만 구현
- D3D12 기준으로 작성된 주석 포함
- 셰이더 언어: HLSL (DXC 컴파일, SPIR-V 크로스 컴파일 지원)

## RTXDI 핵심 셰이더 헤더 활용법

자체 렌더러에 RTXDI를 통합할 때는 `Libraries/Rtxdi/Include/Rtxdi/` 아래 헤더들을  
직접 include하여 사용 가능:

```hlsl
#include "Rtxdi/ReSTIRDI.hlsli"

// Reservoir 초기화
RTXDI_Reservoir reservoir = RTXDI_EmptyReservoir();

// 후보 업데이트
RTXDI_StreamSample(reservoir, lightSample, random, targetPdf);

// Temporal/Spatial merge
RTXDI_CombineReservoirs(current, neighbor, random, neighborTargetPdf);

// 최종 가중치 계산
RTXDI_FinalizeResampling(reservoir, targetPdf, numSamples);
```

## 빌드 요구사항

- Windows 10+ / Linux
- Visual Studio 2019+
- CMake 3.18+
- Vulkan SDK (Vulkan 빌드 시)
- DirectX Shader Compiler (DXC)
- NVIDIA GPU (RTX 시리즈 권장)
