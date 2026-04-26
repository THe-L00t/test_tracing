# 프로젝트 개요

**DXR Path Tracer** — Windows DirectX 12 기반 레이트레이싱 렌더러.

- **언어**: C++23, HLSL (Shader Model 6.x)
- **API**: D3D12 + DXR (DirectX Raytracing)
- **빌드**: Visual Studio (`test_tracing.sln`)

## 렌더링 기능

1. **경로 추적 (Path Tracing)** — GGX BRDF + MIS (8-bounce, NEE, Glass/TIR)
2. **A-trous 디노이저** — F키 런타임 토글
3. **ReSTIR DI** (`pure-restir-di` 브랜치) — GBuffer 5패스 파이프라인
4. **Fresnel-Guided Perceptual Sampling** (`feature/fresnel-guided-pt`, 논문 연구 브랜치)

## 씬

- `1` Outdoor / `2` Indoor / `3` PBR Showcase (투명·반투명 구)

## ReSTIR 패스 순서 (pure-restir-di 전용)

`GBuffer → Initial RIS → Temporal Reuse → Spatial Reuse → Shade`

---

# 브랜치 구성

| 브랜치 | 용도 | 상태 |
|---|---|---|
| `master` | 안정 버전. 검증된 기능만 머지 | 유지 |
| `feature/pbr-path-tracing` | GGX BRDF + MIS 경로 추적, A-trous 디노이저 | 완료 |
| `pure-restir-di` | ReSTIR DI 단독 구현 (GBuffer 5패스) | 완료 |
| `pure-restir-gi` | ReSTIR GI 구현 | 완료 |
| `feature/fresnel-guided-pt` | **논문 연구 브랜치** — Fresnel Prior 기반 adaptive sampling | **진행 중** |
| `중앙-포커스-샘플링` | 가우시안 분포 기반 중앙 포커스 샘플링 실험 | 실험 완료 |

## ⚠️ feature/fresnel-guided-pt 브랜치 주의사항

- **base**: `feature/pbr-path-tracing` 에서 분기 (`pure-restir-di` 아님)
- **이유**: pure-restir-di는 ReSTIR 토글이 없어 Pure PT baseline 측정 불가
- **분기 명령**:
  ```bash
  git checkout feature/pbr-path-tracing
  git checkout -b feature/fresnel-guided-pt
  ```

---

# 진행 상황 파일

상세 구현 현황 및 남은 작업은 아래 메모리 파일에 기록된다:

```
C:\Users\sigun\.claude\projects\C--Users-sigun-University------test-tracing\memory\project_progress.md
```

---

# 작업 규칙

## 1. 작업 후 보고

작업을 완료한 뒤 반드시 다음 항목을 사용자에게 요약 보고한다:

- **변경한 파일** 및 변경 내용 요약
- **해결된 문제** 또는 구현된 기능
- **남은 작업** (다음에 해야 할 것)

## 2. 빌드 테스트는 사용자에게 위임

**빌드(MSBuild, cmake 등) 및 런타임 테스트를 직접 실행하지 않는다.**  
코드 작성 완료 후 "빌드하여 확인해 주세요"로 사용자에게 넘긴다.

## 4. 매 수정 후 커밋

코드 수정이 완료될 때마다 반드시 git commit을 생성한다:

- 커밋 메시지는 `fix:` / `feat:` / `refactor:` 등 conventional commit 형식 사용
- 커밋 단위는 논리적으로 완결된 하나의 변경 (버그 하나 수정, 기능 하나 추가 등)
- 커밋 전 `git diff`로 의도치 않은 변경이 포함되지 않았는지 확인

## 5. 진행 상황 파일 갱신

작업을 진행할 때마다 `project_progress.md`를 최신 상태로 갱신한다:

- 완료된 항목은 "구현 완료" 섹션으로 이동
- 새로운 버그·이슈 발견 시 "남은 작업"에 추가
- 힙 레이아웃·상수버퍼 구조 등 아키텍처 변경 반영
- 날짜 기준으로 세션을 구분하여 기록
