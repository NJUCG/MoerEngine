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
        float      GetNearClip() const noexcept;
        float      GetFarClip() const noexcept;
        float      GetTanHalfFov() const noexcept;
        float      GetAspectRatio() const noexcept;

        void MoveForward(float);
        void MoveRight(float);
        void MoveUp(float);
        void UpdateRotation(float, float);
        void GetAABB(Vector3f&, Vector3f&);
        void GetPlanes(Vector4f _planes[6]);

        bool IsDirty() const;//judge if camera changed compared to last frame

        void Tick();//update camera per frame

    private:
        Matrix4x4f m_view_matrix;

        float m_fov_y;//degree
        float m_aspect_ratio;
        float m_near_clip;
        float m_far_clip;

        Vector3f m_position;

        Matrix4x4f m_rotate;    //world to cam
        Matrix4x4f m_rotate_inv;//cam to world

        bool       m_to_world_dirty = true;
        Matrix4x4f m_to_world;// camera to world
        Matrix4x4f m_view;    // world to camera

        bool       m_projection_dirty = true;
        Matrix4x4f m_sample_to_camera;// screen to camera
        Matrix4x4f m_proj;            // camera to screen

        Vector3f p_min;
        Vector3f p_max;

        // bool pitchLock = true;
        float totalPitch = 0.f;  //limited within (0, 360)
        float yawReverse = false;//reverse left and right
    };
}// namespace Moer