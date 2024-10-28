#include "scene/Camera.h"

#include "log/LogSystem.h"
#include "math/Base.h"
#include "math/Constant.h"
#include "math/Function.h"
//
// namespace Moer {
//     void Camera::SetPerspective(float fov, float aspect, float zNear, float zFar) {
//         m_fov = fov;
//         m_aspect = aspect;
//         m_zNear = zNear;
//         m_zFar = zFar;
//
//         //todo
//         //m_perspective = (fov, aspect, zNear, zFar);
//     }
//
//     void Camera::SetRotation(Vector3f rotation) {
//         m_rotation = rotation;
//     }
//
//     void Camera::SetTranslation(Vector3f translation) {
//         m_position = translation;
//     }
//
//     void Camera::Translate(const Vector3f& delta) {
//         m_position = m_position + delta;
//     }
//
//     void Camera::Rotate(const Vector3f& delta) {
//         m_rotation = m_rotation + delta;
//     }
//
// }

namespace Moer {
    extern WindowInput& wndInput;

    float Camera::sensitivity       = 0.5f;
    float Camera::sensitivity_scale = 1.f;

    //camera space axis
    Vector3f Camera::X = Vector3f(1.f, 0.f, 0.f);
    Vector3f Camera::Y = Vector3f(0.f, 1.f, 0.f);
    Vector3f Camera::Z = Vector3f(0.f, 0.f, 1.f);

    static const float fov_min = 0.012f;
    static const float fov_max = 180.f;

    Camera::Camera() noexcept {
    }

    float    Camera::GetFov() const noexcept { return m_fov_y; }
    Vector3f Camera::GetPosition() const noexcept { return m_position; }

    // Matrix4x4f Camera::getSampleToCameraMatrix() noexcept {
    //     if (m_projection_dirty) {
    //         m_proj = MakePerspectiveMatrixRH(
    //             m_fov_y / 180.f * 3.14159265358979323846f, m_aspect_ratio, m_near_clip, m_far_clip);
    //         m_sample_to_camera = Inverse(
    //             MakeScaling(0.5f, 0.5f, 1.f) *
    //             MakeTranslation(1.f, 1.f, 0.f) *
    //             m_proj);
    //         m_projection_dirty = false;
    //     }
    //     return m_sample_to_camera;
    // }

    Matrix4x4f Camera::GetProjectionMatrix() noexcept {
        if (m_projection_dirty) {
            m_proj = MakePerspectiveMatrixRH(
                //Use Inverse Depth
                Angle::DegreeToRadian(m_fov_y),
                m_aspect_ratio,
                m_far_clip,
                m_near_clip);
            m_sample_to_camera = Inverse(
                MakeScaling(0.5f, 0.5f, 1.f) *
                MakeTranslation(1.f, 1.f, 0.f) *
                m_proj);
            m_projection_dirty = false;
        }
        return m_proj;//camera to screen
    }

    float Camera::GetNearClip() const noexcept { return m_near_clip; }
    float Camera::GetFarClip() const noexcept { return m_far_clip; }
    float Camera::GetTanHalfFov() const noexcept { return tan(m_fov_y / 180.f * HALF_PI); }
    float Camera::GetAspectRatio() const noexcept { return m_aspect_ratio; }

    Vector4f Camera::GetFrustum() const noexcept {
        return m_frustum;
    }

    Vector3f Camera::GetDirection() const noexcept {
        return m_dir;
    }

    Matrix4x4f Camera::GetToWorldMatrix() noexcept {
        if (m_to_world_dirty) {
            m_view           = m_rotate * MakeTranslation(-m_position.x, -m_position.y, -m_position.z);//world to camera
            m_to_world       = Inverse(m_view);
            m_to_world_dirty = false;
        }
        return m_to_world;//camera to world
    }

    Matrix4x4f Camera::GetViewMatrix() noexcept {
        if (m_to_world_dirty) {
            m_view           = m_rotate * MakeTranslation(-m_position.x, -m_position.y, -m_position.z);
            m_to_world       = Inverse(m_view);
            m_to_world_dirty = false;
        }
        return m_view;//world to camera
    }
    Matrix4x4f Camera::GetRotateMatrix() noexcept {
        return m_rotate;
    }
    Matrix4x4f Camera::GetTranslateMatrix() noexcept {
        return MakeTranslation(m_position.x, m_position.y, m_position.z);
    }

    void Camera::SetProjectionFactor(float fov_y, float aspect_ratio, float near_clip, float far_clip) noexcept {
        if (fov_y < fov_min)
            fov_y = fov_min;
        else if (fov_y > fov_max)
            fov_y = fov_max;
        m_fov_y            = fov_y;
        m_aspect_ratio     = aspect_ratio;
        m_near_clip        = near_clip;
        m_far_clip         = far_clip;
        m_projection_dirty = true;
    }

    void Camera::SetFov(float fov) noexcept {
        if (fov < fov_min)
            fov = fov_min;
        else if (fov > fov_max)
            fov = fov_max;
        if (m_fov_y != fov) {
            m_fov_y            = fov;
            m_projection_dirty = true;
        }
    }

    void Camera::SetAspectRatio(float aspect_ratio) noexcept {
        if (m_aspect_ratio != aspect_ratio) {
            m_aspect_ratio     = aspect_ratio;
            m_projection_dirty = true;
        }
    }

    void Camera::SetNearClip(float near_clip) noexcept {
        if (m_near_clip != near_clip) {
            m_near_clip        = near_clip;
            m_projection_dirty = true;
        }
    }

    void Camera::SetFarClip(float far_clip) noexcept {
        if (m_far_clip != far_clip) {
            m_far_clip         = far_clip;
            m_projection_dirty = true;
        }
    }

    void Camera::SetWorldTransform(Transform to_world) noexcept {
        m_to_world   = to_world.GetMatrix4x4();
        m_position.x = m_to_world[0].w;
        m_position.y = m_to_world[1].w;
        m_position.z = m_to_world[2].w;

        m_rotate      = Transpose(m_to_world);
        m_rotate[3].x = 0.f;
        m_rotate[3].y = 0.f;
        m_rotate[3].z = 0.f;

        m_rotate_inv = Transpose(m_rotate);

        m_view = m_rotate * MakeTranslation(-m_position.x, -m_position.y, -m_position.z);
        //world to cam
        m_to_world       = Inverse(m_view);
        m_to_world_dirty = false;
    }

    void Camera::MoveForward(float delta) {
        Vector3f t       = Z * Vector3f(delta);                                                         //camera space
        t                = -Vector3f(m_rotate_inv * Vector4f(t * sensitivity * sensitivity_scale, 0.f));//into world space(considering -z instead of z)
        auto translation = MakeTranslation(t.x, t.y, t.z);
        m_position       = Vector3f(translation * Vector4f(m_position, 1.f));
        m_to_world_dirty = true;
    }

    void Camera::MoveRight(float delta) {
        Vector3f t       = X * Vector3f(delta);
        t                = Vector3f(m_rotate_inv * Vector4f(t * sensitivity * sensitivity_scale, 0.f));
        auto translation = MakeTranslation(t.x, t.y, t.z);
        m_position       = Vector3f(translation * Vector4f(m_position, 1.f));
        m_to_world_dirty = true;
    }

    void Camera::MoveUp(float _delta) {
        Vector3f t       = Y * Vector3f(_delta);
        t                = Vector3f(m_rotate_inv * Vector4f(t * sensitivity * sensitivity_scale, 0.f));
        auto translation = MakeTranslation(t.x, t.y, t.z);
        m_position       = Vector3f(translation * Vector4f(m_position, 1.f));
        m_to_world_dirty = true;
    }

    void Camera::UpdateRotation(float _delta_x, float _delta_y) {
        // Quaternion pitch(X, Angle::MakeFromDegree(delta_x * sensitivity * sensitivity_scale));
        // Quaternion yaw(Y, Angle::MakeFromDegree(delta_y * sensitivity * sensitivity_scale));
        total_pitch += _delta_y * 0.15f;
        if (total_pitch < 0.f)
            total_pitch += 360.f;
        if (total_pitch > 360.f)
            total_pitch = fmodf(total_pitch, 360.f);

        if (total_pitch < 90.f || total_pitch > 270.f)
            yaw_reverse = false;
        else
            yaw_reverse = true;

        Quaternion pitch(X, Angle::MakeFromDegree(_delta_y * 0.15f));                           //rotate around x-axis
        Quaternion yaw(Y, Angle::MakeFromDegree((yaw_reverse ? -1.f : 1.f) * _delta_x * 0.15f));//rotate around y-axis

        m_rotate =
            FillDiagonal4x4(pitch.GetRotation(), 1.f) *
            FillDiagonal4x4(yaw.GetRotation(), 1.f) * m_rotate;

        m_rotate_inv     = Transpose(m_rotate);
        m_to_world_dirty = true;
    }

    void Camera::GetAABB(Vector3f& _out_min, Vector3f& _out_max) {
        // if (IsDirty()) {
        //far plane points and near plane points
        Vector3f far_points[4];
        Vector3f near_points[4];
        Vector3f cam_pos = this->GetPosition();
        for (int i = 0; i < 4; i++) {
            far_points[i]  = cam_pos + Z * m_far_clip + X * (i % 2 == 0 ? 1.f : -1.f) * m_far_clip * m_aspect_ratio + Y * (i / 2 == 0 ? 1.f : -1.f) * m_far_clip;
            near_points[i] = cam_pos + Z * m_near_clip + X * (i % 2 == 0 ? 1.f : -1.f) * m_near_clip * m_aspect_ratio + Y * (i / 2 == 0 ? 1.f : -1.f) * m_near_clip;
        }
        for (int i = 0; i < 4; i++) {
            p_min = Min(p_min, far_points[i]);
            p_min = Min(p_min, near_points[i]);
        }
        // }
        _out_min = p_min;
        _out_max = p_max;
    }

    void Camera::GetPlanes(Vector4f _out_planes[6]) {
        auto vp        = GetProjectionMatrix() * GetViewMatrix();
        _out_planes[0] = vp.r3 + vp.r0;//left
        _out_planes[1] = vp.r3 - vp.r0;//right
        _out_planes[2] = vp.r3 + vp.r1;//top
        _out_planes[3] = vp.r3 - vp.r1;//bottom
        _out_planes[4] = vp.r2;        //near
        _out_planes[5] = vp.r3 - vp.r2;//far
        //normalize
        for (int i = 0; i < 6; i++) {
            auto length    = Length(Vector3f(_out_planes[i]));
            _out_planes[i] = Vector4f(Normalizef(Vector3f(_out_planes[i])), _out_planes[i].w / length);
        }
    }

    //if changed cam_pos / cam_direction : m_to_world_dirty->true
    //if changed fov, clips, aspect_ratio：m_projection_dirty->true
    //currently not used
    bool Camera::IsDirty() const {
        return m_projection_dirty || m_to_world_dirty;
    }

    void Camera::UpdateCalculatedValues() {

        m_view     = m_rotate * MakeTranslation(-m_position.x, -m_position.y, -m_position.z);
        m_to_world = Inverse(m_view);

        if (m_projection_dirty) {
            m_proj = MakePerspectiveMatrixRH(
                //Use Inverse Depth
                Angle::DegreeToRadian(m_fov_y),
                m_aspect_ratio,
                m_far_clip,
                m_near_clip);
            m_sample_to_camera = Inverse(
                MakeScaling(0.5f, 0.5f, 1.f) *
                MakeTranslation(1.f, 1.f, 0.f) *
                m_proj);
            m_projection_dirty = false;
        }
        m_to_world_dirty = false;

        auto vp = GetProjectionMatrix();

        m_planes[0] = vp.r3 + vp.r0;//left
        m_planes[1] = vp.r3 - vp.r0;//right
        m_planes[2] = vp.r3 + vp.r1;//bottom
        m_planes[3] = vp.r3 - vp.r1;//top
        m_planes[4] = vp.r2;        //near
        m_planes[5] = vp.r3 - vp.r2;//far
        //normalize
        for (int i = 0; i < 6; i++) {
            auto length = Length(Vector3f(m_planes[i]));
            m_planes[i] = Vector4f(Normalizef(Vector3f(m_planes[i])), m_planes[i].w / length);
        }

        auto inv_proj = Inverse(vp);
        auto left     = inv_proj * Vector4f(-1.f, 1.f, -1.f, 1.f);

        //frustum
        float x0 = m_planes[FRUSTUM_LEFT].z / m_planes[FRUSTUM_LEFT].x;
        float x1 = m_planes[FRUSTUM_RIGHT].z / m_planes[FRUSTUM_RIGHT].x;
        float y0 = m_planes[FRUSTUM_BOTTOM].z / m_planes[FRUSTUM_BOTTOM].y;
        float y1 = m_planes[FRUSTUM_TOP].z / m_planes[FRUSTUM_TOP].y;

        m_frustum.x = -x0;
        m_frustum.y = -y0;
        m_frustum.z = x0 - x1;
        m_frustum.w = y0 - y1;

        //direction
        // m_dir = Normalizef(Vector3f(m_rotate_inv * Vector4f(0.f, 0.f, 1.f, 0.f)));
        m_dir = Vector3f(0.f, 0.f, 1.f);
        // LOG_INFO("dir: {} {} {}", m_dir.x, m_dir.y, m_dir.z);
        // auto     test_dir  = Vector2f(0.f, 1.f) * Vector2f(m_frustum.z, m_frustum.w) + Vector2f(m_frustum.x, m_frustum.y);
        // Vector3f test_dir3 = Vector3f(test_dir.x, test_dir.y, 1.f);
        // test_dir3          = Normalizef(Vector3f(m_rotate_inv * Vector4f(test_dir3, 0.f)));
        // LOG_INFO("test_dir3: {} {} {}", test_dir3.x, test_dir3.y, test_dir3.z);
        // int k = 0;
    }

    void Camera::Tick() {
        if (wndInput.mouseEnterScreen) {
            // fov & aspect_ratio
            // this->SetFov(wndInput.fov);
            // this->SetAspectRatio(wndInput.aspect_ratio);

            // camera speed
            if (wndInput.speedUp) {
                wndInput.cameraSpeed += 5.0f;
            }
            if (wndInput.speedDown) {
                wndInput.cameraSpeed -= 2.5f;
                if (wndInput.cameraSpeed < 0.f)
                    wndInput.cameraSpeed = 0.f;
            }
            if (wndInput.resetSpeed) {
                wndInput.cameraSpeed = 25.f;
            }

            // movement
            if (wndInput.camera_forward)
                this->MoveForward(wndInput.cameraSpeed * wndInput.deltaTime);
            if (wndInput.camera_backward)
                this->MoveForward(-wndInput.cameraSpeed * wndInput.deltaTime);
            if (wndInput.camera_left)
                this->MoveRight(-wndInput.cameraSpeed * wndInput.deltaTime);
            if (wndInput.camera_right)
                this->MoveRight(wndInput.cameraSpeed * wndInput.deltaTime);
            if (wndInput.camera_up)
                this->MoveUp(wndInput.cameraSpeed * wndInput.deltaTime);
            if (wndInput.camera_down)
                this->MoveUp(-wndInput.cameraSpeed * wndInput.deltaTime);

            // rotation
            if ((wndInput.deltaX || wndInput.deltaY) && wndInput.mouseButtonState[MouseButtons::Left]) {

                this->UpdateRotation(wndInput.deltaX, wndInput.deltaY);
                wndInput.deltaX = 0;
                wndInput.deltaY = 0;
            }
        }

        if (IsDirty()) {
            UpdateCalculatedValues();
        }
    }

    CameraRef Camera::CreateDefaultCamera() {
        CameraRef default_camera = MoerNew(Camera)();
        default_camera->SetFov(36.f);
        Transform transform = Transform(Vector3f(0.0f, 0.0f, 0.0f), Vector3f(0.0f, 0.0f, 1.0f), Vector3f(0.0f, 1.0f, 0.0f));
        default_camera->SetWorldTransform(transform);
        default_camera->SetNearClip(0.1f);
        default_camera->SetFarClip(1000.0f);
        default_camera->SetAspectRatio(16.0f / 9.0f);
        return default_camera;
    }

    InputStream& Camera::operator>>(InputStream& _stream) {
        _stream >> m_position >> m_rotate >> m_fov_y >> m_aspect_ratio >> m_near_clip >> m_far_clip;
        UpdateCalculatedValues();
        return _stream;
    }

    OutputStream& Camera::operator<<(OutputStream& _stream) const {
        _stream << m_position << m_rotate << m_fov_y << m_aspect_ratio << m_near_clip << m_far_clip;
        //calculate values
        return _stream;
    }

}// namespace Moer
