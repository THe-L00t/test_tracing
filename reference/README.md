# ReSTIR DI 레퍼런스 모음

DirectX 12 기반 ReSTIR DI 구현 시 참고할 수 있는 자료들을 정리한 폴더입니다.

## 파일 목록

| 파일 | 내용 |
|---|---|
| `01_paper_bitterli2020.md` | 원본 논문 요약 (Bitterli et al., SIGGRAPH 2020) |
| `02_rtxdi_nvidia.md` | NVIDIA RTXDI SDK 구조 및 MinimalSample 설명 |
| `03_restir_dx12_penn.md` | UPenn CIS565 ReSTIR DX12 구현 (lindayukeyi) |
| `04_gentler_introduction.md` | ReSTIR 실전 구현 가이드 (HLSL 코드 포함) |
| `05_theory_and_implementation.md` | RIS + WRS 이론 및 DXR 구현 상세 |
| `06_other_implementations.md` | 기타 구현체 목록 및 링크 |

## 핵심 GitHub 레포지토리

- **lindayukeyi/ReSTIR_DX12** — DX12 + DXR 직접 구현 (Falcor 3.1 기반)
  https://github.com/lindayukeyi/ReSTIR_DX12

- **NVIDIA-RTX/RTXDI** — NVIDIA 공식 ReSTIR DI/GI/PT SDK (D3D12 + Vulkan)
  https://github.com/NVIDIA-RTX/RTXDI

- **karel-tomanec/Falcor-ReSTIR** — 석사 논문용 ReSTIR DI (Falcor 5.x)
  https://github.com/karel-tomanec/Falcor-ReSTIR

- **tatran5/Reservoir-Spatio-Temporal-Importance-Resampling-ReSTIR** — 팀 구현체
  https://github.com/tatran5/Reservoir-Spatio-Temporal-Importance-Resampling-ReSTIR

## 원본 논문

- Bitterli et al. 2020 (SIGGRAPH): https://benedikt-bitterli.me/restir/
- NVIDIA Research: https://research.nvidia.com/labs/rtr/publication/bitterli2020spatiotemporal/
- PDF 직링크: https://research.nvidia.com/sites/default/files/pubs/2020-07_Spatiotemporal-reservoir-resampling/ReSTIR.pdf
