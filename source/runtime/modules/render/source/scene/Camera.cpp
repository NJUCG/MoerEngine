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

    //camera space axis
    const Vector3f Camera::X           = Vector3f(1.f, 0.f, 0.f);
    const Vector3f Camera::Y           = Vector3f(0.f, 1.f, 0.f);
    const Vector3f Camera::Z           = Vector3f(0.f, 0.f, 1.f);
    const Vector3f Camera::UP_IN_WORLD = Camera::Y;

    const float Camera::k_pitch_min = -89.5f;
    const float Camera::k_pitch_max = 89.5f;
    const float Camera::k_fov_min   = 0.012f;
    const float Camera::k_fov_max   = 180.f;

    Camera::Camera() noexcept {
    }

    Vector3f Camera::GetPosition() const noexcept { return m_position; }
    float    Camera::GetYaw() const noexcept { return m_yaw; }
    float    Camera::GetPitch() const noexcept { return m_pitch; }

    float Camera::GetFov() const noexcept { return m_fov_y; }
    float Camera::GetNearClip() const noexcept { return m_near_clip; }
    float Camera::GetFarClip() const noexcept { return m_far_clip; }
    float Camera::GetTanHalfFov() const noexcept { return tan(m_fov_y / 180.f * HALF_PI); }
    float Camera::GetAspectRatio() const noexcept { return m_aspect_ratio; }

    Vector3f Camera::GetDirection() const noexcept { return Vector3f(0.f, 0.f, -1.f); }
    Vector3f Camera::GetUp() const noexcept { return m_up; }
    Vector3f Camera::GetRight() const noexcept { return m_right; }
    Vector3f Camera::GetFront() const noexcept { return m_front; }
    Vector3f Camera::GetForward() const noexcept { return m_forward; }

    Matrix4x4f Camera::GetViewMatrix() noexcept {
        UpdateViewMatrix();
        return m_view_matrix;// world to camera
    }

    Matrix4x4f Camera::GetViewMatrixInv() noexcept {
        UpdateViewMatrix();
        return m_view_matrix_inv;
    }

    Matrix4x4f Camera::GetToWorldMatrix() noexcept {
        // same with GetViewMatrixInv
        return GetViewMatrixInv();
        // UpdateViewMatrix();
        // return m_view_matrix_inv;
    }

    Matrix4x4f Camera::GetRotateMatrix() noexcept {
        UpdateViewMatrix();
        return m_view_matrix_rotate_submatrix;
    }

    Matrix4x4f Camera::GetTranslateMatrix() noexcept {
        return MakeTranslation(m_position.x, m_position.y, m_position.z);
    }

    Matrix4x4f Camera::GetProjectionMatrix() noexcept {
        UpdateProjectionMatrix();
        return m_projection_matrix;
    }

    Matrix4x4f Camera::GetProjectionMatrixInv() noexcept {
        UpdateProjectionMatrix();
        return m_projection_matrix_inv;
    }

    Matrix4x4f Camera::GetViewProjectionMatrix() noexcept {
        UpdateViewProjectionMatrix();
        return m_view_projection_matrix;
    }

    Matrix4x4f Camera::GetViewProjectionMatrixInv() noexcept {
        UpdateViewProjectionMatrix();
        return m_view_projection_matrix_inv;
    }

    void Camera::SetProjectionFactor(float fov_y, float aspect_ratio, float near_clip, float far_clip) noexcept {
        if (fov_y < k_fov_min)
            fov_y = k_fov_min;
        else if (fov_y > k_fov_max)
            fov_y = k_fov_max;
        m_fov_y                      = fov_y;
        m_aspect_ratio               = aspect_ratio;
        m_near_clip                  = near_clip;
        m_far_clip                   = far_clip;
        m_is_projection_matrix_dirty = true;
    }

    void Camera::SetFov(float fov) noexcept {
        if (fov < k_fov_min)
            fov = k_fov_min;
        else if (fov > k_fov_max)
            fov = k_fov_max;
        if (m_fov_y != fov) {
            m_fov_y                      = fov;
            m_is_projection_matrix_dirty = true;
        }
    }

    void Camera::SetAspectRatio(float aspect_ratio) noexcept {
        if (m_aspect_ratio != aspect_ratio) {
            m_aspect_ratio               = aspect_ratio;
            m_is_projection_matrix_dirty = true;
        }
    }

    void Camera::SetNearClip(float near_clip) noexcept {
        if (m_near_clip != near_clip) {
            m_near_clip                  = near_clip;
            m_is_projection_matrix_dirty = true;
        }
    }

    void Camera::SetFarClip(float far_clip) noexcept {
        if (m_far_clip != far_clip) {
            m_far_clip                   = far_clip;
            m_is_projection_matrix_dirty = true;
        }
    }

    void Camera::SetWorldTransform(const Transform& to_world_transform) noexcept {
        // to_world == camera_to_world == view_matrix_inv

        auto mat4_to_str = [](const Matrix4x4f& mat) {
            std::string str = "Matrix4x4f {\n";
            for (int i = 0; i < 4; i++) {
                str += "  ";
                for (int j = 0; j < 4; j++) {
                    str += std::to_string(mat[i][j]) + " ";
                }
                str += "\n";
            }
            str += "}";
            return str;
        };

        auto vec3_to_str = [](const Vector3f& vec) {
            return "Vector3f {" + std::to_string(vec.x) + ", " + std::to_string(vec.y) + ", " + std::to_string(vec.z) + "}";
        };

        auto to_world = to_world_transform.GetMatrix4x4();
        m_position.x  = to_world[0].w;
        m_position.y  = to_world[1].w;
        m_position.z  = to_world[2].w;

        LOG_INFO("ToWorldTransform    : {}", mat4_to_str(to_world));

        auto rotate = Transpose(to_world);
        rotate[3].x = 0.f;
        rotate[3].y = 0.f;
        rotate[3].z = 0.f;
        LOG_INFO("Rotate after: {}", mat4_to_str(rotate));

        auto rotate_inv = Inverse(rotate);
        LOG_INFO("Rotate Inv: {}", mat4_to_str(rotate_inv));

        UpdateDerivedProperties();

        auto view_matrix_v0 = GetViewMatrix();
        LOG_INFO("ViewMatrix v0: {}", mat4_to_str(view_matrix_v0));

        auto to_world_2 = MakeTranslation(m_position.x, m_position.y, m_position.z) * rotate_inv;
        LOG_INFO("ToWorldTransform 2: {}", mat4_to_str(to_world_2));

        auto view_matrix = Inverse(to_world_2);
        LOG_INFO("ToWorldTransform 2 Inv (View Matrix): {}", mat4_to_str(view_matrix));

        auto forward_v0 = Vector3f(-rotate[0][2], -rotate[1][2], -rotate[2][2]);
        LOG_INFO("Forward v0: {}", vec3_to_str(forward_v0));
        m_yaw   = Angle::RadianToDegree(atan2(forward_v0.x, forward_v0.z));
        m_pitch = Angle::RadianToDegree(asin(-forward_v0.y));

        Matrix4x4f view_matrix_f1;
        {
            auto forward = Vector3f(view_matrix[0][2], view_matrix[1][2], view_matrix[2][2]);

            LOG_INFO("Forward v1: {}", vec3_to_str(forward));

            m_yaw   = Angle::RadianToDegree(atan2(forward.x, forward.z));
            m_pitch = Angle::RadianToDegree(asin(-forward.y));

            UpdateDerivedProperties();

            view_matrix_f1 = GetViewMatrix();
            LOG_INFO("ViewMatrix: {}", mat4_to_str(view_matrix_f1));
        }

        {
            auto forward = Vector3f(view_matrix_f1[0][2], view_matrix_f1[1][2], view_matrix_f1[2][2]);

            LOG_INFO("Forward v2: {}", vec3_to_str(forward));

            m_yaw   = Angle::RadianToDegree(atan2(forward.x, forward.z));
            m_pitch = Angle::RadianToDegree(asin(-forward.y));

            UpdateDerivedProperties();

            auto view_matrix_f = GetViewMatrix();
            LOG_INFO("ViewMatrix: {}", mat4_to_str(view_matrix_f));
        }
    }

    void Camera::MoveForward(float delta) {
        if (Abs(delta) < EPS) return;

        float velocity = m_move_speed * delta;
        m_position += m_forward * velocity;

        m_is_view_matrix_dirty = true;
    }

    void Camera::MoveRight(float delta) {
        if (Abs(delta) < EPS) return;

        float velocity = m_move_speed * delta;
        m_position += m_right * velocity;

        m_is_view_matrix_dirty = true;
    }

    void Camera::MoveUp(float delta) {
        if (Abs(delta) < EPS) return;

        float velocity = m_move_speed * delta;
        m_position += m_up * velocity;

        m_is_view_matrix_dirty = true;
    }

    void Camera::UpdateRotation(float delta_x, float delta_y) {
        if (Abs(delta_x) < EPS && Abs(delta_y) < EPS) return;

        m_yaw += delta_x * m_mouse_sensitivity;
        m_pitch += -1.0 * delta_y * m_mouse_sensitivity;
        m_pitch = Clamp(m_pitch, k_pitch_min, k_pitch_max);

        UpdateDerivedProperties();
    }

    void Camera::GetAABB(Vector3f& _out_min, Vector3f& _out_max) {
        Vector3f far_points[4];
        Vector3f near_points[4];
        Vector3f cam_pos = this->GetPosition();
        for (int i = 0; i < 4; i++) {
            far_points[i]  = cam_pos + Z * m_far_clip + X * (i % 2 == 0 ? 1.f : -1.f) * m_far_clip * m_aspect_ratio + Y * (i / 2 == 0 ? 1.f : -1.f) * m_far_clip;
            near_points[i] = cam_pos + Z * m_near_clip + X * (i % 2 == 0 ? 1.f : -1.f) * m_near_clip * m_aspect_ratio + Y * (i / 2 == 0 ? 1.f : -1.f) * m_near_clip;
        }
        _out_min = far_points[0];
        _out_max = far_points[0];
        for (int i = 0; i < 4; i++) {
            _out_min = Min(_out_min, far_points[i]);
            _out_min = Min(_out_min, near_points[i]);
            _out_max = Max(_out_max, far_points[i]);
            _out_max = Max(_out_max, near_points[i]);
        }
    }

    Vector4f Camera::GetFrustum() noexcept {
        UpdatePlanesAndFrustum();
        return m_frustum;
    }

    void Camera::GetPlanes(Vector4f _out_planes[6]) {
        UpdatePlanesAndFrustum();// TODO: m_is_xxx_dirty
        for (int i = 0; i < 6; i++) {
            _out_planes[i] = m_planes[i];
        }
    }

    //if changed cam_pos / cam_direction : m_to_world_dirty->true
    //if changed fov, clips, aspect_ratio：m_projection_dirty->true
    //currently not used
    bool Camera::IsDirty() const {
        return m_is_view_matrix_dirty || m_is_projection_matrix_dirty || m_is_view_projection_matrix_dirty || m_is_planes_and_frustum_dirty;
    }

    // position/yaw/pitch -> front/right/up/forward
    void Camera::UpdateDerivedProperties() {
        m_front   = Normalizef(Vector3f(cos(Angle::DegreeToRadian(m_yaw)) * cos(Angle::DegreeToRadian(m_pitch)),
                                      sin(Angle::DegreeToRadian(m_pitch)),
                                      sin(Angle::DegreeToRadian(m_yaw)) * cos(Angle::DegreeToRadian(m_pitch))));
        m_right   = Normalizef(Cross(m_front, UP_IN_WORLD));
        m_up      = Normalizef(Cross(m_right, m_front));
        m_forward = Normalizef(Cross(UP_IN_WORLD, m_right));

        m_is_view_matrix_dirty        = true;
        m_is_planes_and_frustum_dirty = true;
    }

    void Camera::UpdateViewMatrix() {
        if (!m_is_view_matrix_dirty) return;

        m_is_view_matrix_dirty            = false;
        m_is_view_projection_matrix_dirty = true;

        auto view_matrix_transform = Transform(m_position, m_position + m_front, m_up);
        m_view_matrix              = view_matrix_transform.matrix;
        // m_view_matrix     = MakeLookatViewMatrixRH(m_position, m_position + m_front, m_up);
        m_view_matrix_inv = Inverse(m_view_matrix);

        m_view_matrix_rotate_submatrix      = m_view_matrix;
        m_view_matrix_rotate_submatrix[0].w = 0.f;
        m_view_matrix_rotate_submatrix[1].w = 0.f;
        m_view_matrix_rotate_submatrix[2].w = 0.f;
        m_view_matrix_rotate_submatrix_inv  = Transpose(m_view_matrix_rotate_submatrix);
    }

    void Camera::UpdateProjectionMatrix() {
        if (!m_is_projection_matrix_dirty) return;

        m_is_projection_matrix_dirty      = false;
        m_is_view_projection_matrix_dirty = true;

        // The m_far_clip and m_near_clip is swapped on purpose.
        // Inverse Depth Projection: https://forums.developer.nvidia.com/t/inverse-depth-projection-tutorial-or-code-sample/219704
        m_projection_matrix     = MakePerspectiveMatrixRH(Angle::DegreeToRadian(m_fov_y), m_aspect_ratio, m_far_clip, m_near_clip);
        m_projection_matrix_inv = Inverse(m_projection_matrix);
    }

    void Camera::UpdateViewProjectionMatrix() {
        UpdateViewMatrix();
        UpdateProjectionMatrix();

        if (!m_is_view_projection_matrix_dirty) return;

        m_view_projection_matrix     = m_projection_matrix * m_view_matrix;
        m_view_projection_matrix_inv = Inverse(m_view_projection_matrix);
    }

    void Camera::UpdatePlanesAndFrustum() {
        if (!m_is_planes_and_frustum_dirty) return;

        auto vp     = GetViewProjectionMatrix();
        m_planes[0] = vp.r3 + vp.r0;//left
        m_planes[1] = vp.r3 - vp.r0;//right
        m_planes[2] = vp.r3 + vp.r1;//top
        m_planes[3] = vp.r3 - vp.r1;//bottom
        m_planes[4] = vp.r2;        //near
        m_planes[5] = vp.r3 - vp.r2;//far

        //normalize
        for (int i = 0; i < 6; i++) {
            auto length = Length(Vector3f(m_planes[i]));
            m_planes[i] = Vector4f(Normalizef(Vector3f(m_planes[i])), m_planes[i].w / length);
        }

        //frustum
        float x0 = m_planes[FRUSTUM_LEFT].z / m_planes[FRUSTUM_LEFT].x;
        float x1 = m_planes[FRUSTUM_RIGHT].z / m_planes[FRUSTUM_RIGHT].x;
        float y0 = m_planes[FRUSTUM_BOTTOM].z / m_planes[FRUSTUM_BOTTOM].y;
        float y1 = m_planes[FRUSTUM_TOP].z / m_planes[FRUSTUM_TOP].y;

        m_frustum.x = -x0;
        m_frustum.y = -y1;
        m_frustum.z = x0 - x1;
        m_frustum.w = y1 - y0;
    }

    /**
     * ## Camera Control Logic
     * 
     * - Drag right mouse button to rotate the camera
     * - Press F key to enter screen
     *   * Move mouse to rotate the camera
     *   * Press W/S/A/D to move the camera
     *   * Press Q/E to move the camera up and down
     *   * Use scroll wheel to adjust the camera fov
     */
    void Camera::Tick() {

        auto reset_cursor_delta = [&]() {
            wndInput.cursor_delta_x = 0.0f;
            wndInput.cursor_delta_y = 0.0f;
        };

        if (!wndInput.is_cursor_hiding) {
            if ((wndInput.cursor_delta_x || wndInput.cursor_delta_y) && wndInput.mouse_button_state[MouseButtons::Right]) {
                this->UpdateRotation(wndInput.cursor_delta_x, wndInput.cursor_delta_y);
                reset_cursor_delta();
            }

        } else {
            // Pressed F

            // fov & aspect_ratio
            this->SetFov(wndInput.fov);
            this->SetAspectRatio(wndInput.aspect_ratio);

            // camera speed
            if (wndInput.speed_up) {
                wndInput.camera_speed += wndInput.k_camera_speed_up_delta;
                if (wndInput.camera_speed > wndInput.k_max_camera_speed)
                    wndInput.camera_speed = wndInput.k_max_camera_speed;
            }
            if (wndInput.speed_down) {
                wndInput.camera_speed -= wndInput.k_camera_speed_down_delta;
                if (wndInput.camera_speed < wndInput.k_min_camera_speed)
                    wndInput.camera_speed = wndInput.k_min_camera_speed;
            }
            if (wndInput.reset_speed) {
                wndInput.camera_speed = wndInput.k_default_camera_speed;
            }

            // movement
            if (wndInput.camera_forward)
                this->MoveForward(wndInput.camera_speed * wndInput.delta_time);
            if (wndInput.camera_backward)
                this->MoveForward(-wndInput.camera_speed * wndInput.delta_time);
            if (wndInput.camera_left)
                this->MoveRight(-wndInput.camera_speed * wndInput.delta_time);
            if (wndInput.camera_right)
                this->MoveRight(wndInput.camera_speed * wndInput.delta_time);
            if (wndInput.camera_up)
                this->MoveUp(wndInput.camera_speed * wndInput.delta_time);
            if (wndInput.camera_down)
                this->MoveUp(-wndInput.camera_speed * wndInput.delta_time);

            // rotation
            if (wndInput.cursor_delta_x || wndInput.cursor_delta_y) {
                this->UpdateRotation(wndInput.cursor_delta_x, wndInput.cursor_delta_y);
                reset_cursor_delta();
            }
        }

        if (IsDirty()) {
            UpdateDerivedProperties();
        }
    }

    CameraRef Camera::CreateDefaultCamera() {// TODO: implement
        CameraRef default_camera = MoerNew(Camera)();
        default_camera->SetFov(36.f);
        Transform transform    = Transform(Vector3f(0.0f, 0.0f, 0.0f), Vector3f(0.0f, 0.0f, 1.0f), Vector3f(0.0f, 1.0f, 0.0f));
        auto      world_to_cam = Inverse(transform.GetMatrix4x4());
        transform.matrix       = world_to_cam;
        default_camera->SetWorldTransform(transform);
        default_camera->SetNearClip(0.1f);
        default_camera->SetFarClip(1000.0f);
        default_camera->SetAspectRatio(16.0f / 9.0f);
        return default_camera;
    }

    InputStream& Camera::operator>>(InputStream& _stream) {
        _stream >> m_position >> m_yaw >> m_pitch >> m_fov_y >> m_aspect_ratio >> m_near_clip >> m_far_clip;
        UpdateDerivedProperties();
        return _stream;
    }

    OutputStream& Camera::operator<<(OutputStream& _stream) const {
        _stream << m_position << m_yaw << m_pitch << m_fov_y << m_aspect_ratio << m_near_clip << m_far_clip;
        //calculate values
        return _stream;
    }

    std::string Camera::ToString() {
        std::string str = "Camera {\n";
        str += "  position: (" + std::to_string(m_position[0]) + ", " + std::to_string(m_position[1]) + ", " + std::to_string(m_position[2]) + ")\n";
        str += "  yaw: " + std::to_string(m_yaw) + "\n";
        str += "  pitch: " + std::to_string(m_pitch) + "\n";
        str += "  fov: " + std::to_string(m_fov_y) + "\n";
        str += "  aspect ratio: " + std::to_string(m_aspect_ratio) + "\n";
        str += "  near clip: " + std::to_string(m_near_clip) + "\n";
        str += "  far clip: " + std::to_string(m_far_clip) + "\n";
        auto view_matrix       = GetViewMatrix();
        auto projection_matrix = GetProjectionMatrix();
        str += "  view matrix:\n";
        for (int i = 0; i < 4; i++) {
            str += "    ";
            for (int j = 0; j < 4; j++) {
                str += std::to_string(view_matrix[i][j]);
                if (j < 3) {
                    str += ", ";
                }
            }
            str += "\n";
        }
        str += "  projection matrix:\n";
        for (int i = 0; i < 4; i++) {
            str += "    ";
            for (int j = 0; j < 4; j++) {
                str += std::to_string(projection_matrix[i][j]);
                if (j < 3) {
                    str += ", ";
                }
            }
            str += "\n";
        }
        str += "}\n";
        return str;
    }

}// namespace Moer
