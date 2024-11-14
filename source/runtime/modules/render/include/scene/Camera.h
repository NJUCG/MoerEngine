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
        const static Vector3f X;
        const static Vector3f Y;
        const static Vector3f Z;
        const static Vector3f UP_IN_WORLD;

        const static float k_pitch_min;
        const static float k_pitch_max;
        const static float k_fov_min;
        const static float k_fov_max;

        Camera() noexcept;

        // MARK: Getter

        Vector3f GetPosition() const noexcept;
        float    GetYaw() const noexcept;
        float    GetPitch() const noexcept;

        float GetFov() const noexcept;
        float GetAspectRatio() const noexcept;
        float GetNearClip() const noexcept;
        float GetFarClip() const noexcept;
        float GetTanHalfFov() const noexcept;

        Vector3f GetDirection() const noexcept;// direction == -Z; strange; TODO: check if it's correct
        Vector3f GetUp() const noexcept;
        Vector3f GetRight() const noexcept;
        // Note: front vs. forward: forward parallel to the XZ plane; front is the direction the camera is facing
        Vector3f GetFront() const noexcept;
        // Note: front vs. forward: forward parallel to the XZ plane; front is the direction the camera is facing
        Vector3f GetForward() const noexcept;

        Matrix4x4f GetViewMatrix() noexcept;
        Matrix4x4f GetViewMatrixInv() noexcept;
        Matrix4x4f GetToWorldMatrix() noexcept;
        // Note: ToWorldMatrix == InverseViewMatrix
        Matrix4x4f GetRotateMatrix() noexcept;
        Matrix4x4f GetTranslateMatrix() noexcept;

        Matrix4x4f GetProjectionMatrix() noexcept;
        Matrix4x4f GetProjectionMatrixInv() noexcept;
        Matrix4x4f GetViewProjectionMatrix() noexcept;
        Matrix4x4f GetViewProjectionMatrixInv() noexcept;

        void     GetAABB(Vector3f&, Vector3f&);
        void     GetPlanes(Vector4f _planes[6]);
        Vector4f GetFrustum() noexcept;

        // MARK: Setter

        void SetWorldTransform(const Transform& to_world_transform) noexcept;

        void SetProjectionFactor(float fov_y, float aspect_ratio, float near_clip, float far_clip) noexcept;
        void SetFov(float f) noexcept;
        void SetAspectRatio(float aspect_ratio) noexcept;
        void SetNearClip(float near_clip) noexcept;
        void SetFarClip(float far_clip) noexcept;

        // MARK: Others

        bool IsDirty() const;//judge if camera changed compared to last frame

        void Tick();//update camera per frame

        static CountableRef<Camera> CreateDefaultCamera();// Create a default camera for the scene. Usually called in resource loader

        InputStream&  operator>>(InputStream& _stream);
        OutputStream& operator<<(OutputStream& _stream) const;

        std::string ToString();

    private:
        // void UpdateCalculatedValues();

        void UpdateDerivedProperties();
        void UpdateViewMatrix();
        void UpdateProjectionMatrix();
        void UpdateViewProjectionMatrix();
        void UpdatePlanesAndFrustum();

        void MoveForward(float);
        void MoveRight(float);
        void MoveUp(float);
        void UpdateRotation(float, float);

    private:
        /**
         * origin properties + options => derived properties
         * 
         * position/yaw/pitch -> front/right/up/forward -> view_matrix & planes/frustum
         *                                                 view_matrix -> view_projection_matrix
         * fov/aspect_ratio/near_clip/far_clip -> projection_matrix
         *                                        projection_matrix -> view_projection_matrix
         */

        // MARK: origin properties
        // (human readable properties)

        Vector3f m_position;
        float    m_yaw;  // degree
        float    m_pitch;// degree

        // MARK: options

        float       m_fov_y;// degree
        float       m_aspect_ratio;
        float       m_near_clip;
        float       m_far_clip;
        const float m_move_speed        = 0.002f;
        const float m_mouse_sensitivity = 0.2f;

        // bool yaw_reverse   = false;// reverse left and right
        // bool pitch_reverse = false;// reverse up and down
        // TODO: implement these two options about reverse

        // MARK: derived vectors

        Vector3f m_front;  // = normalize(Vec3f(cos(rad(yaw))*cos(rad(pitch)), sin(rad(pitch)), sin(rad(yaw))*cos(rad(pitch)))))
        Vector3f m_up;     // = normalize(cross(m_front, UP_IN_WORLD))
        Vector3f m_right;  // = normalize(cross(m_right, m_front))
        Vector3f m_forward;// = normalize(cross(UP_IN_WORLD, m_right))
        // front vs. forward: forward parallel to the XZ plane; front is the direction the camera is facing

        bool     m_is_planes_and_frustum_dirty = true;
        Vector4f m_planes[6];
        Vector4f m_frustum;

        // MARK: derived matrices

        bool       m_is_view_matrix_dirty = true;
        Matrix4x4f m_view_matrix;
        Matrix4x4f m_view_matrix_inv;
        Matrix4x4f m_view_matrix_rotate_submatrix;
        Matrix4x4f m_view_matrix_rotate_submatrix_inv;
        // Matrix4x4f m_view_matrix_translate_submatrix;     // no need
        // Matrix4x4f m_view_matrix_translate_submatrix_inv; // no need

        bool       m_is_projection_matrix_dirty = true;
        Matrix4x4f m_projection_matrix;
        Matrix4x4f m_projection_matrix_inv;

        bool       m_is_view_projection_matrix_dirty = true;
        Matrix4x4f m_view_projection_matrix;
        Matrix4x4f m_view_projection_matrix_inv;
    };

    using CameraRef = CountableRef<Camera>;

}// namespace Moer