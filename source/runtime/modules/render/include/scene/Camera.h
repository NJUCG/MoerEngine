#pragma once
#include "math/Math.h"
#include "math/Transform.h"
#include "misc/CountableRef.h"
#include "RenderAPI.h"
#include "window/WindowInput.h"

namespace Moer {
    class RENDER_API Camera : public CountableResource {
    public:
        static float sensitivity;
        static float sensitivity_scale;

        static Vector3f X;
        static Vector3f Y;
        static Vector3f Z;

        Camera() noexcept;

        void SetProjectionFactor(float fov_y, float aspect_ratio, float near_clip, float far_clip) noexcept;
        void SetWorldTransform(Transform to_world) noexcept;
        void SetFov(float f) noexcept;
        void SetAspectRatio(float aspect_ratio) noexcept;
        void SetNearClip(float near_clip) noexcept;
        void SetFarClip(float far_clip) noexcept;

        float      GetFov() const noexcept;
        Vector3f   GetPosition() const noexcept;
        Matrix4x4f GetProjectionMatrix() noexcept;
        Matrix4x4f GetToWorldMatrix() noexcept;
        Matrix4x4f GetViewMatrix() noexcept;

        void MoveForward(float);
        void MoveRight(float);
        void MoveUp(float);
        void UpdateRotation(float, float);
        void SpinZ(float);

        bool IsDirty() const;       //脏标记，用于判断是否发生了变化，未变化的话直接使用上一帧缓存的变换

        void Tick();    //动态更新camera

    private:
        Matrix4x4f m_view_matrix;   //视图变换矩阵

        float m_fov_y;//degree
        float m_aspect_ratio;
        float m_near_clip;
        float m_far_clip;

        Vector3f m_position;

        Matrix4x4f m_rotate;    //world 2 cam 的旋转
        Matrix4x4f m_rotate_inv;    //cam 2 world

        bool       m_to_world_dirty = true;
        Matrix4x4f m_to_world;// camera to world
        Matrix4x4f m_view;    // world to camera

        bool       m_projection_dirty = true;
        Matrix4x4f m_sample_to_camera;// screen to camera
        Matrix4x4f m_proj;            // camera to screen
    };
}// namespace Moer