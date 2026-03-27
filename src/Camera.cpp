#include "Camera.h"
#include <algorithm>

// 피치 제한 (±80도 → ±1.3963 rad)
static constexpr float k_pitchLimit = 80.0f * (3.14159265358979323846f / 180.0f);

void Camera::Init(float x, float y, float z, float yaw, float pitch)
{
    m_pos[0] = x;
    m_pos[1] = y;
    m_pos[2] = z;
    m_yaw    = yaw;
    m_pitch  = pitch;
    Rebuild();
}

void Camera::MoveForward(float d)
{
    // XZ 평면 기준 전진 (월드 Y 무관)
    m_pos[0] += m_forward[0] * d;
    m_pos[1] += m_forward[1] * d;
    m_pos[2] += m_forward[2] * d;
}

void Camera::MoveRight(float d)
{
    m_pos[0] += m_right[0] * d;
    m_pos[1] += m_right[1] * d;
    m_pos[2] += m_right[2] * d;
}

void Camera::MoveUp(float d)
{
    // 월드 Y 방향으로 이동
    m_pos[1] += d;
}

void Camera::AddYaw(float rad)
{
    m_yaw += rad;
    Rebuild();
}

void Camera::AddPitch(float rad)
{
    m_pitch += rad;
    // ±80도로 제한
    m_pitch = std::clamp(m_pitch, -k_pitchLimit, k_pitchLimit);
    Rebuild();
}

void Camera::Rebuild()
{
    // 요/피치 → 방향 벡터 계산
    // Forward: (sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch))
    float cosPitch = cosf(m_pitch);
    float sinPitch = sinf(m_pitch);
    float cosYaw   = cosf(m_yaw);
    float sinYaw   = sinf(m_yaw);

    m_forward[0] = sinYaw * cosPitch;
    m_forward[1] = -sinPitch;
    m_forward[2] = cosYaw * cosPitch;

    // Right = normalize(cross(forward, worldUp))
    // worldUp = (0, 1, 0)
    // cross(forward, (0,1,0)) = (forward.z * 0 - forward.y * 1,
    //                            forward.y * 0 - forward.z * 0,  [wrong]
    // cross(F, UP) = (Fy*UPz - Fz*UPy, Fz*UPx - Fx*UPz, Fx*UPy - Fy*UPx)
    //             = (Fy*1 - Fz*0,    Fz*0 - Fx*1,     Fx*0 - Fy*0)
    //   Wait: worldUp=(0,1,0), so:
    //   cross(F, UP).x = F.y*UP.z - F.z*UP.y = F.y*0 - F.z*1 = -F.z
    //   cross(F, UP).y = F.z*UP.x - F.x*UP.z = F.z*0 - F.x*0 = 0
    //   cross(F, UP).z = F.x*UP.y - F.y*UP.x = F.x*1 - F.y*0 = F.x
    // So right = normalize((-F.z, 0, F.x)) = (cosYaw, 0, -sinYaw)
    float len = sqrtf(m_forward[2] * m_forward[2] + m_forward[0] * m_forward[0]);
    if (len > 1e-6f)
    {
        m_right[0] = m_forward[2] / len;
        m_right[1] = 0.0f;
        m_right[2] = -m_forward[0] / len;
    }
    else
    {
        // 거의 수직으로 바라볼 때 예외 처리
        m_right[0] = cosYaw;
        m_right[1] = 0.0f;
        m_right[2] = -sinYaw;
    }

    // Up = cross(right, forward)
    // Up.x = R.y*F.z - R.z*F.y
    // Up.y = R.z*F.x - R.x*F.z
    // Up.z = R.x*F.y - R.y*F.x
    m_up[0] = m_right[1] * m_forward[2] - m_right[2] * m_forward[1];
    m_up[1] = m_right[2] * m_forward[0] - m_right[0] * m_forward[2];
    m_up[2] = m_right[0] * m_forward[1] - m_right[1] * m_forward[0];

    // 정규화
    float upLen = sqrtf(m_up[0]*m_up[0] + m_up[1]*m_up[1] + m_up[2]*m_up[2]);
    if (upLen > 1e-6f)
    {
        m_up[0] /= upLen;
        m_up[1] /= upLen;
        m_up[2] /= upLen;
    }
}
