# 프로젝트 개요

**DXR Path Tracer** — Windows DirectX 12 기반 레이트레이싱 렌더러.

- **언어**: C++23, HLSL (Shader Model 6.x)
- **API**: D3D12 + DXR (DirectX Raytracing)
- **빌드**: Visual Studio (`test_tracing.sln`)

## 렌더링 기능

1. **경로 추적 (Path Tracing)** — GGX BRDF + MIS
2. **A-trous 디노이저** — D키 런타임 토글
3. **ReSTIR DI** (`feature/restir` 브랜치, 진행 중) — R키 토글, Bitterli 2020 기반

## 씬

- `1` Outdoor / `2` Indoor / `3` PBR Showcase (투명·반투명 구)

## ReSTIR 패스 순서

`GBuffer → Initial RIS → Temporal Reuse → Spatial Reuse → Shade`

---

# 브랜치 구성

앞으로 기능이 추가될수록 브랜치가 확장될 수 있다.

| 브랜치 | 용도 |
|---|---|
| `master` | 안정 버전. 검증된 기능만 머지 |
| `feature/pbr-path-tracing` | GGX BRDF + MIS 경로 추적, A-trous 디노이저 |
| `feature/restir` | ReSTIR DI 구현 (현재 작업 브랜치) |
| `중앙-포커스-샘플링` | 가우시안 확률 분포 기반 중앙 포커스 샘플링 실험 |
| *(추가 예정)* | 새 기능·실험은 별도 브랜치로 분리하여 개발 |

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

## 2. 매 수정 후 커밋

코드 수정이 완료될 때마다 반드시 git commit을 생성한다:

- 커밋 메시지는 `fix:` / `feat:` / `refactor:` 등 conventional commit 형식 사용
- 커밋 단위는 논리적으로 완결된 하나의 변경 (버그 하나 수정, 기능 하나 추가 등)
- 커밋 전 `git diff`로 의도치 않은 변경이 포함되지 않았는지 확인

## 3. 진행 상황 파일 갱신

작업을 진행할 때마다 `project_restir.md`를 최신 상태로 갱신한다:

- 완료된 항목은 "구현 완료" 섹션으로 이동
- 새로운 버그·이슈 발견 시 "남은 작업"에 추가
- 힙 레이아웃·상수버퍼 구조 등 아키텍처 변경 반영
- 날짜 기준으로 세션을 구분하여 기록
