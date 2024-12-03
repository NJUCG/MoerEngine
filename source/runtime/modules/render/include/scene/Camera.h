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
        const static float k_mouse_sensitivity;
        const static float k_camera_speed_multiplier;

        const static float k_camera_speed_default;
        const static float k_camera_speed_max;
        const static float k_camera_speed_min;
        const static float k_camera_speed_up_delta;
        const static float k_camera_speed_down_delta;

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

        void SetProjectionFactor(float fov_y, float aspect_ratio, float near_clip, float far_clip) noexcept;
        void SetFov(float f) noexcept;
        void SetAspectRatio(float aspect_ratio) noexcept;
        void SetNearClip(float near_clip) noexcept;
        void SetFarClip(float far_clip) noexcept;

        // Jitter Matrix only affect the view_projection_matrix
        void       SetJitterMatrix(const Matrix4x4f& jitter_matrix) noexcept;
        void       SetJitterMatrix(const Vector2f& jitter) noexcept;
        Matrix4x4f GetJitterMatrix() const noexcept;
        void       ResetJitterMatrix() noexcept;

        void SetWorldTransform(const Transform& to_world_transform) noexcept;

        /**
         * ## Initialize the camera
         */
        void Initialize(const Transform& to_world_transform, float fov_y, float aspect_ratio, float near_clip, float far_clip);

        /**
         * ## Initialize the camera
         */
        void Initialize(Vector3f position, float yaw, float pitch, float fov_y, float aspect_ratio, float near_clip, float far_clip);

        /**
         * ## Update the camera based on input per frame
         */
        void Tick();//update camera per frame

        bool IsDirty() const;//judge if camera changed compared to last frame

        static CountableRef<Camera> CreateDefaultCamera();// Create a default camera for the scene. Usually called in resource loader

        InputStream&  operator>>(InputStream& _stream);
        OutputStream& operator<<(OutputStream& _stream) const;

        std::string ToString();

    private:
        /**
         * ## Update the camera based on position/rotation/fov/aspect_ratio/near_clip/far_clip
         * 
         * If you want to update all derived properties, set `is_ignore_dirty_flags_and_update_all` to true
         * If you want a better performance and dirty flags are correct, set `is_ignore_dirty_flags_and_update_all` to false
         * You can call `UpdateAllDerivedProperties(true)` to initialize the camera
         */
        void UpdateAllDerivedProperties();

        void UpdateVectors();
        void UpdateViewMatrix();
        void UpdateProjectionMatrix();
        void UpdateViewProjectionMatrix();
        void UpdatePlanesAndFrustum();

        void MoveForward(float);
        void MoveRight(float);
        void MoveUp(float);
        void ApplyRotation(float, float);

    private:
        // MARK: camera control
        float camera_speed = k_camera_speed_default;

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

        bool     m_is_position_modified = false;
        Vector3f m_position;
        bool     m_is_rotation_modified = false;
        float    m_yaw;  // degree
        float    m_pitch;// degree

        // MARK: options

        bool  m_is_options_modified = false;
        float m_fov_y;// degree
        float m_aspect_ratio;
        float m_near_clip;
        float m_far_clip;

        bool       m_is_jittered_matrix_modified = false;
        Matrix4x4f m_jittered_matrix             = Matrix4x4f::Identity();// only affect view_projection_matrix

        // bool yaw_reverse   = false;// reverse left and right
        // bool pitch_reverse = false;// reverse up and down
        // TODO: implement these two options about reverse

        // MARK: derived vectors

        Vector3f m_front;  // = normalize(Vec3f(cos(rad(yaw))*cos(rad(pitch)), sin(rad(pitch)), sin(rad(yaw))*cos(rad(pitch)))))
        Vector3f m_up;     // = normalize(cross(m_front, UP_IN_WORLD))
        Vector3f m_right;  // = normalize(cross(m_right, m_front))
        Vector3f m_forward;// = normalize(cross(UP_IN_WORLD, m_right))
        // front vs. forward: forward parallel to the XZ plane; front is the direction the camera is facing

        Vector4f m_planes[6];
        Vector4f m_frustum;

        // MARK: derived matrices

        Matrix4x4f m_view_matrix;
        Matrix4x4f m_view_matrix_inv;
        Matrix4x4f m_view_matrix_rotate_submatrix;
        Matrix4x4f m_view_matrix_rotate_submatrix_inv;
        // Matrix4x4f m_view_matrix_translate_submatrix;     // no need
        // Matrix4x4f m_view_matrix_translate_submatrix_inv; // no need

        Matrix4x4f m_projection_matrix;
        Matrix4x4f m_projection_matrix_inv;

        Matrix4x4f m_view_projection_matrix;
        Matrix4x4f m_view_projection_matrix_inv;
    };

    using CameraRef = CountableRef<Camera>;

}// namespace Moer