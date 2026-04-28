#pragma once
#include "Common.h"

// 1인칭 카메라: 요(yaw)/피치(pitch) 기반 자유 이동
class Camera
{
public:
    // 초기 위치와 회전 설정
    void Init(float x, float y, float z,
              float yaw = 0.0f, float pitch = 0.0f);

    // 이동 (로컬 방향 기준)
    void MoveForward(float d);
    void MoveRight(float d);
    void MoveUp(float d);    // 월드 Y 방향

    // 회전 (라디안)
    void AddYaw(float rad);
    void AddPitch(float rad);

    // 접근자
    const float* Pos()     const noexcept { return m_pos; }
    const float* Right()   const noexcept { return m_right; }
    const float* Up()      const noexcept { return m_up; }
    const float* Forward() const noexcept { return m_forward; }
    float        Yaw()     const noexcept { return m_yaw; }
    float        Pitch()   const noexcept { return m_pitch; }

private:
    // 요/피치에서 방향 벡터 재계산
    void Rebuild();

    float m_pos[3]     = {};
    float m_forward[3] = {0.0f, 0.0f, 1.0f};
    float m_right[3]   = {1.0f, 0.0f, 0.0f};
    float m_up[3]      = {0.0f, 1.0f, 0.0f};
    float m_yaw   = 0.0f;
    float m_pitch = 0.0f;
};
