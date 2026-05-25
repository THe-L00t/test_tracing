# NVIDIA DLSS SDK

**Version:** 310.6.0 (main branch as of 2026-05-24)
**Source:** https://github.com/NVIDIA/DLSS

## 폴더 구조

```
third_party/dlss/
  include/          헤더 파일 (git 추적)
  lib/              .lib 링크 라이브러리 (git 추적)
    nvsdk_ngx_d.lib       Release 빌드용 (동적 링크)
    nvsdk_ngx_d_dbg.lib   Debug 빌드용 (동적 링크)
  bin/              런타임 DLL (.gitignore — 별도 다운로드 필요)
    nvngx_dlss.dll        DLSS Super Resolution 런타임
    nvngx_dlssd.dll       DLSS Ray Reconstruction (RR) 런타임
```

## bin/ DLL 재다운로드 방법

`bin/` 폴더의 DLL은 크기 때문에 git에 포함되지 않는다.
아래 PowerShell 명령으로 재다운로드:

```powershell
Invoke-WebRequest -Uri "https://raw.githubusercontent.com/NVIDIA/DLSS/main/lib/Windows_x86_64/rel/nvngx_dlss.dll" `
    -OutFile "third_party\dlss\bin\nvngx_dlss.dll" -UseBasicParsing
Invoke-WebRequest -Uri "https://raw.githubusercontent.com/NVIDIA/DLSS/main/lib/Windows_x86_64/rel/nvngx_dlssd.dll" `
    -OutFile "third_party\dlss\bin\nvngx_dlssd.dll" -UseBasicParsing
```

## 사용 방법 (D3D12)

1. `include/` 경로를 프로젝트 Include 경로에 추가
2. `nvsdk_ngx_d.lib` (Release) 또는 `nvsdk_ngx_d_dbg.lib` (Debug) 를 추가 종속성에 추가
3. 빌드 후 `nvngx_dlss.dll`을 실행 파일과 같은 디렉터리에 복사

## 핵심 헤더

| 파일 | 용도 |
|------|------|
| `nvsdk_ngx.h` | NGX 초기화/종료, 기능 조회 |
| `nvsdk_ngx_defs.h` | 공용 타입·열거형 정의 |
| `nvsdk_ngx_helpers.h` | D3D12 DLSS 피처 생성·실행 헬퍼 |
| `nvsdk_ngx_params.h` | 파라미터 키 상수 |

## D3D12 통합 흐름

```
NVSDK_NGX_D3D12_Init()          // Device 초기화
NVSDK_NGX_D3D12_GetCapabilityParameters()  // 지원 여부 확인
NVSDK_NGX_DLSS_GetOptimalSettingsCallback() // 최적 해상도 쿼리
NVSDK_NGX_D3D12_CreateFeature()  // DLSS 피처 오브젝트 생성
  -- 매 프레임 --
NVSDK_NGX_D3D12_EvaluateFeature() // DLSS 업스케일 실행
  -- 종료 --
NVSDK_NGX_D3D12_ReleaseFeature()
NVSDK_NGX_D3D12_Shutdown()
```
