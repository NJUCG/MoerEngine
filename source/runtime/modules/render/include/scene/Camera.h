#pragma once
#include "math/Math.h"
#include "math/Transform.h"
#include "misc/CountableRef.h"
#include "RenderAPI.h"
#include "serialize/Serializer.h"
#include "window/WindowInput.h"

namespace Moer {

    class RENDER_API Camera : public CountableResource {
        enum {
            FRUSTUM_LEFT = 0,
            FRUSTUM_RIGHT,
            FRUSTUM_BOTTOM,
            FRUSTUM_TOP,
            FRUSTUM_NEAR,
            FRUSTUM_FAR
        };

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
        Matrix4x4f GetRotateMatrix() noexcept;
        Matrix4x4f GetTranslateMatrix() noexcept;
        float      GetNearClip() const noexcept;
        float      GetFarClip() const noexcept;
        float      GetTanHalfFov() const noexcept;
        float      GetAspectRatio() const noexcept;
        Vector4f   GetFrustum() const noexcept;
        Vector3f   GetDirection() const noexcept;

        void MoveForward(float);
        void MoveRight(float);
        void MoveUp(float);
        void UpdateRotation(float, float);
        void GetAABB(Vector3f&, Vector3f&);
        void GetPlanes(Vector4f _planes[6]);

        bool IsDirty() const;//judge if camera changed compared to last frame

        void Tick();//update camera per frame

        static CountableRef<Camera> CreateDefaultCamera();// Create a default camera for the scene. Usually called in resource loader

        InputStream&  operator>>(InputStream& _stream);
        OutputStream& operator<<(OutputStream& _stream) const;

    private:
        void UpdateCalculatedValues();

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

        Vector4f m_frustum;
        Vector4f m_planes[6];
        Vector3f m_dir;

        // bool pitchLock = true;
        float total_pitch = 0.f;  //limited within (0, 360)
        float yaw_reverse = false;//reverse left and right
    };

    using CameraRef = CountableRef<Camera>;

}// namespace Moer