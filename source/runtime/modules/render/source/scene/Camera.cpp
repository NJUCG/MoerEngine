#include "scene/Camera.h"

#include "log/LogSystem.h"
#include "math/Base.h"
#include "math/Constant.h"
#include "math/Function.h"

namespace Moer {

    //camera space axis
    const Vector3f Camera::X           = Vector3f(1.f, 0.f, 0.f);
    const Vector3f Camera::Y           = Vector3f(0.f, 1.f, 0.f);
    const Vector3f Camera::Z           = Vector3f(0.f, 0.f, 1.f);
    const Vector3f Camera::UP_IN_WORLD = Camera::Y;

    // mouse control parameters
    const float Camera::k_pitch_min      = -89.5f;
    const float Camera::k_pitch_max      = 89.5f;
    const float Camera::k_fov_default    = 60.0f;
    const float Camera::k_fov_min        = 0.012f;
    const float Camera::k_fov_max        = 180.0f;
    const float Camera::k_fov_multiplier = 2.f;

    const float Camera::k_mouse_sensitivity              = 0.2f;
    const float Camera::k_mouse_sensitivity_mouse_moving = 1.0f;

    // camera movement parameters
    // actual camera speed = camera_speed * k_camera_speed_multiplier * delta_time
    // e.g. camera_speed = 25.0 & k_camera_speed_multiplier = 0.1 => 2.5 units per second
    const float Camera::k_camera_speed_multiplier = 0.1f;
    const float Camera::k_camera_speed_default    = 25.0f;
    const float Camera::k_camera_speed_max        = 100.0f;
    const float Camera::k_camera_speed_min        = 0.1f;
    const float Camera::k_camera_speed_up_delta   = 50.0f;// use 2 sec to reach max speed
    const float Camera::k_camera_speed_down_delta = 25.0f;// use 4 sec to reach min speed

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

    Vector3f Camera::GetDirection() const noexcept { return m_front; }
    Vector3f Camera::GetUp() const noexcept { return m_up; }
    Vector3f Camera::GetRight() const noexcept { return m_right; }
    Vector3f Camera::GetFront() const noexcept { return m_front; }
    Vector3f Camera::GetForward() const noexcept { return m_forward; }

    Matrix4x4f Camera::GetViewMatrix() noexcept {
        return m_view_matrix;// world to camera
    }

    Matrix4x4f Camera::GetViewMatrixInv() noexcept {
        return m_view_matrix_inv;
    }

    Matrix4x4f Camera::GetToWorldMatrix() noexcept {
        return GetViewMatrixInv();
    }

    Matrix4x4f Camera::GetRotateMatrix() noexcept {
        return m_view_matrix_rotate_submatrix;
    }

    Matrix4x4f Camera::GetTranslateMatrix() noexcept {
        return MakeTranslation(m_position.x, m_position.y, m_position.z);
    }

    Matrix4x4f Camera::GetProjectionMatrix() noexcept {
        return m_projection_matrix;
    }

    Matrix4x4f Camera::GetProjectionMatrixInv() noexcept {
        return m_projection_matrix_inv;
    }

    Matrix4x4f Camera::GetViewProjectionMatrix() noexcept {
        return m_view_projection_matrix;
    }

    Matrix4x4f Camera::GetViewProjectionMatrixInv() noexcept {
        return m_view_projection_matrix_inv;
    }

    void Camera::SetProjectionFactor(float fov_y, float aspect_ratio, float near_clip, float far_clip) noexcept {
        if (fov_y < k_fov_min)
            fov_y = k_fov_min;
        else if (fov_y > k_fov_max)
            fov_y = k_fov_max;
        m_fov_y               = fov_y;
        m_aspect_ratio        = aspect_ratio;
        m_near_clip           = near_clip;
        m_far_clip            = far_clip;
        m_is_options_modified = true;
    }

    void Camera::SetFov(float fov) noexcept {
        if (fov < k_fov_min)
            fov = k_fov_min;
        else if (fov > k_fov_max)
            fov = k_fov_max;
        if (m_fov_y != fov) {
            m_fov_y               = fov;
            m_is_options_modified = true;
        }
    }

    void Camera::SetAspectRatio(float aspect_ratio) noexcept {
        if (m_aspect_ratio != aspect_ratio) {
            m_aspect_ratio        = aspect_ratio;
            m_is_options_modified = true;
        }
    }

    void Camera::SetNearClip(float near_clip) noexcept {
        if (m_near_clip != near_clip) {
            m_near_clip           = near_clip;
            m_is_options_modified = true;
        }
    }

    void Camera::SetFarClip(float far_clip) noexcept {
        if (m_far_clip != far_clip) {
            m_far_clip            = far_clip;
            m_is_options_modified = true;
        }
    }

    void Camera::SetJitterMatrix(const Matrix4x4f& jitter_matrix) noexcept {
        m_jittered_matrix             = jitter_matrix;
        m_is_jittered_matrix_modified = true;
    }

    void Camera::SetJitterMatrix(const Vector2f& jitter) noexcept {
        m_jittered_matrix             = MakeTranslation(2.0f * jitter.x / WindowInput::Get().width, 2.0f * jitter.y / WindowInput::Get().height, 0.f);
        m_is_jittered_matrix_modified = true;
    }

    Matrix4x4f Camera::GetJitterMatrix() const noexcept {
        return m_jittered_matrix;
    }

    void Camera::ResetJitterMatrix() noexcept {
        m_jittered_matrix             = Matrix4x4f::Identity();
        m_is_jittered_matrix_modified = true;
    }

    void Camera::SetWorldTransform(const Transform& _to_world_transform) noexcept {
        // to_world == camera_to_world == view_matrix_inv
        auto to_world = _to_world_transform.GetMatrix4x4();// M^-1 = T^-1 * R^-1
        assert(Abs(to_world[3].x) < EPS && Abs(to_world[3].y) < EPS && Abs(to_world[3].z) < EPS && Abs(to_world[3].w - 1.f) < EPS);
        auto front_v0 = Normalizef(Vector3f(-to_world[0][2], -to_world[1][2], -to_world[2][2]));

        m_position.x = to_world[0].w;
        m_position.y = to_world[1].w;
        m_position.z = to_world[2].w;
        m_yaw        = Angle::RadianToDegree(atan2(front_v0.z, front_v0.x));
        m_pitch      = Angle::RadianToDegree(asin(front_v0.y));

        m_is_position_modified = true;
        m_is_rotation_modified = true;

        // auto to_world_rotate = to_world;// R^-1
        // to_world_rotate[0].w = 0.f;
        // to_world_rotate[1].w = 0.f;
        // to_world_rotate[2].w = 0.f;
        // auto view_matrix_inv = MakeTranslation(m_position.x, m_position.y, m_position.z) * to_world_rotate;
        // assert(view_matrix_inv == to_world) // This will pass
    }

    void Camera::MoveForward(float delta) {
        if (Abs(delta) < EPS) return;

        m_position += m_forward * delta;// use m_front for more comfortable control

        m_is_position_modified = true;
    }

    void Camera::MoveFront(float delta) {
        if (Abs(delta) < EPS) return;

        m_position += m_front * delta;

        m_is_position_modified = true;
    }

    void Camera::MoveRight(float delta) {
        if (Abs(delta) < EPS) return;

        m_position += m_right * delta;

        m_is_position_modified = true;
    }

    void Camera::MoveUp(float delta) {
        if (Abs(delta) < EPS) return;

        m_position += UP_IN_WORLD * delta;

        m_is_position_modified = true;
    }

    void Camera::ApplyRotation(float delta_x, float delta_y) {
        if (Abs(delta_x) < EPS && Abs(delta_y) < EPS) return;

        m_yaw += delta_x * k_mouse_sensitivity;
        m_pitch += -1.0 * delta_y * k_mouse_sensitivity;
        m_pitch = Clamp(m_pitch, k_pitch_min, k_pitch_max);

        m_is_rotation_modified = true;
    }

    void Camera::GetAABB(Vector3f& _out_min, Vector3f& _out_max) {
        LOG_WARNING("Camera.GetAABB(..) is not tested yet");

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
        return m_frustum;
    }

    void Camera::GetPlanes(Vector4f _out_planes[6]) {
        for (int i = 0; i < 6; i++) {
            _out_planes[i] = m_planes[i];
        }
    }

    //if changed cam_pos / cam_direction : m_to_world_dirty->true
    //if changed fov, clips, aspect_ratio：m_projection_dirty->true
    //currently not used
    bool Camera::IsDirty() const {
        return m_is_position_modified || m_is_rotation_modified || m_is_options_modified;
    }

    void Camera::Initialize(const Transform& to_world_transform, float fov_y, float aspect_ratio, float near_clip, float far_clip) {
        fov_y = k_fov_default;

        SetWorldTransform(to_world_transform);
        SetProjectionFactor(fov_y, aspect_ratio, near_clip, far_clip);
        ResetJitterMatrix();
        UpdateAllDerivedProperties();
    }

    void Camera::Initialize(Vector3f position, float yaw, float pitch, float fov_y, float aspect_ratio, float near_clip, float far_clip) {
        fov_y = k_fov_default;

        m_position             = position;
        m_yaw                  = yaw;
        m_pitch                = pitch;
        m_is_position_modified = true;
        m_is_rotation_modified = true;
        SetProjectionFactor(fov_y, aspect_ratio, near_clip, far_clip);
        ResetJitterMatrix();
        UpdateAllDerivedProperties();
    }

    void Camera::UpdateAllDerivedProperties() {
        if (m_is_rotation_modified) {
            UpdateVectors();
        }
        if (m_is_position_modified || m_is_rotation_modified) {
            UpdateViewMatrix();
        }
        if (m_is_options_modified || m_is_jittered_matrix_modified) {
            UpdateProjectionMatrix();
        }
        if (m_is_position_modified || m_is_rotation_modified || m_is_jittered_matrix_modified || m_is_options_modified) {
            UpdateViewProjectionMatrix();
            UpdatePlanesAndFrustum();
        }

        m_is_position_modified        = false;
        m_is_rotation_modified        = false;
        m_is_options_modified         = false;
        m_is_jittered_matrix_modified = false;
    }

    // position/yaw/pitch -> front/right/up/forward
    void Camera::UpdateVectors() {
        // m_front has no need to be normalized
        m_front   = Vector3f(cos(Angle::DegreeToRadian(m_yaw)) * cos(Angle::DegreeToRadian(m_pitch)),
                           sin(Angle::DegreeToRadian(m_pitch)),
                           sin(Angle::DegreeToRadian(m_yaw)) * cos(Angle::DegreeToRadian(m_pitch)));
        m_right   = Normalizef(Cross(m_front, UP_IN_WORLD));
        m_up      = Normalizef(Cross(m_right, m_front));
        m_forward = Normalizef(Cross(UP_IN_WORLD, m_right));
    }

    void Camera::UpdateViewMatrix() {
        auto view_matrix_transform = Transform(m_position, m_position + m_front, m_up);
        m_view_matrix              = view_matrix_transform.matrix;
        // m_view_matrix     = MakeLookatViewMatrixRH(m_position, m_position + m_front, m_up);

        // m_view_matrix = m_view_matrix * m_jittered_matrix;

        m_view_matrix_inv = Inverse(m_view_matrix);

        m_view_matrix_rotate_submatrix      = m_view_matrix;
        m_view_matrix_rotate_submatrix[0].w = 0.f;
        m_view_matrix_rotate_submatrix[1].w = 0.f;
        m_view_matrix_rotate_submatrix[2].w = 0.f;
        m_view_matrix_rotate_submatrix_inv  = Transpose(m_view_matrix_rotate_submatrix);
    }

    void Camera::UpdateProjectionMatrix() {
        // The m_far_clip and m_near_clip is swapped on purpose.
        // Inverse Depth Projection: https://forums.developer.nvidia.com/t/inverse-depth-projection-tutorial-or-code-sample/219704
        m_projection_matrix     = m_jittered_matrix * MakePerspectiveMatrixRH(Angle::DegreeToRadian(m_fov_y), m_aspect_ratio, m_far_clip, m_near_clip);
        m_projection_matrix_inv = Inverse(m_projection_matrix);
    }

    void Camera::UpdateViewProjectionMatrix() {
        m_view_projection_matrix     = m_projection_matrix * m_view_matrix;
        m_view_projection_matrix_inv = Inverse(m_view_projection_matrix);
    }

    void Camera::UpdatePlanesAndFrustum() {
        auto vp     = GetProjectionMatrix();
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
     * - Almost the same as Unreal Engine
     * - When dragging the mouse, press W/A/S/D/Q/E to move the camera
     * - Drag `right mouse button` to rotate the camera
     * - Drag `left mouse button` to rotate the camera and move forward/backward
     * - Drag `both mouse buttons` or `middle button` to move the camera
     */
    void Camera::Tick(float aspect_ratio) {

        if (aspect_ratio <= EPS) {
            this->SetAspectRatio(WindowInput::Get().aspect_ratio);
        } else {
            this->SetAspectRatio(aspect_ratio);
        }

        if (!WindowInput::Get().is_cursor_hiding) {

            // fov & aspect_ratio
            if (!IsZero(WindowInput::Get().scroll_offset)) {
                float coef;
                if (Compare(m_fov_y, k_fov_default) <= 0) {
                    coef = (Min(k_fov_default, m_fov_y) - k_fov_min) / (k_fov_default - k_fov_min);
                } else {
                    coef = (k_fov_max - Max(k_fov_default, m_fov_y)) / (k_fov_max - k_fov_default);
                }

                this->SetFov(m_fov_y - WindowInput::Get().scroll_offset * coef * k_fov_multiplier);
                WindowInput::Get().scroll_offset = 0.0f;
            }

        } else {

            // camera speed

            if (WindowInput::Get().speed_up) {
                camera_speed += k_camera_speed_up_delta * WindowInput::Get().delta_time;
                if (camera_speed > k_camera_speed_max)
                    camera_speed = k_camera_speed_max;
            }
            if (WindowInput::Get().speed_down) {
                camera_speed -= k_camera_speed_down_delta * WindowInput::Get().delta_time;
                if (camera_speed < k_camera_speed_min)
                    camera_speed = k_camera_speed_min;
            }
            if (WindowInput::Get().reset_speed) {
                camera_speed = k_camera_speed_default;
            }

            float speed = 1.0 * camera_speed * k_camera_speed_multiplier * WindowInput::Get().delta_time;

            // movement
            if (WindowInput::Get().camera_forward)
                this->MoveFront(speed);
            if (WindowInput::Get().camera_backward)
                this->MoveFront(-speed);
            if (WindowInput::Get().camera_left)
                this->MoveRight(-speed);
            if (WindowInput::Get().camera_right)
                this->MoveRight(speed);
            if (WindowInput::Get().camera_up)
                this->MoveUp(speed);
            if (WindowInput::Get().camera_down)
                this->MoveUp(-speed);

            // rotation

            auto pure_rotate = [&]() {
                this->ApplyRotation(WindowInput::Get().cursor_delta_x, WindowInput::Get().cursor_delta_y);
            };

            auto rotate_with_moving = [&]() {
                this->ApplyRotation(WindowInput::Get().cursor_delta_x, 0.f);

                float speed_y = WindowInput::Get().cursor_delta_y * k_mouse_sensitivity_mouse_moving * WindowInput::Get().delta_time;
                this->MoveForward(-speed_y);
            };

            auto pure_move = [&]() {
                float speed_x = WindowInput::Get().cursor_delta_x * k_mouse_sensitivity_mouse_moving * WindowInput::Get().delta_time;
                float speed_y = WindowInput::Get().cursor_delta_y * k_mouse_sensitivity_mouse_moving * WindowInput::Get().delta_time;

                this->MoveRight(speed_x);
                this->MoveUp(-speed_y);
            };

            if (WindowInput::Get().cursor_delta_x || WindowInput::Get().cursor_delta_y) {

                // unreal style camera control
                if (WindowInput::Get().key_button_switch_state[KeyButtons::F]) {
                    pure_rotate();
                } else if (WindowInput::Get().mouse_button_state[MouseButtons::Left] && WindowInput::Get().mouse_button_state[MouseButtons::Right]) {
                    pure_move();
                } else if (WindowInput::Get().mouse_button_state[MouseButtons::Middle]) {
                    pure_move();
                } else if (WindowInput::Get().mouse_button_state[MouseButtons::Right]) {
                    pure_rotate();
                } else if (WindowInput::Get().mouse_button_state[MouseButtons::Left]) {
                    rotate_with_moving();
                }

                WindowInput::Get().cursor_delta_x = 0.0f;
                WindowInput::Get().cursor_delta_y = 0.0f;
            }
        }

        UpdateAllDerivedProperties();
        elapsed_time = WindowInput::Get().delta_time;
    }

    CameraRef Camera::CreateDefaultCamera() {
        CameraRef default_camera = MoerNew(Camera)();
        Transform transform      = Transform(Vector3f(0.0f, 0.0f, 0.0f), Vector3f(0.0f, 0.0f, 1.0f), Vector3f(0.0f, 1.0f, 0.0f));
        auto      world_to_cam   = Inverse(transform.GetMatrix4x4());
        transform.matrix         = world_to_cam;
        default_camera->Initialize(transform, 36.f, 16.0f / 9.0f, 0.1f, 1000.0f);
        return default_camera;
    }

    InputStream& Camera::operator>>(InputStream& _stream) {
        _stream >> m_position >> m_yaw >> m_pitch >> m_fov_y >> m_aspect_ratio >> m_near_clip >> m_far_clip;
        Initialize(m_position, m_yaw, m_pitch, m_fov_y, m_aspect_ratio, m_near_clip, m_far_clip);
        return _stream;
    }

    OutputStream& Camera::operator<<(OutputStream& _stream) const {
        _stream << m_position << m_yaw << m_pitch << m_fov_y << m_aspect_ratio << m_near_clip << m_far_clip;
        return _stream;
    }

    float Camera::GetDeletaTime() const noexcept {
        return WindowInput::Get().delta_time;
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
        auto inv_view_matrix   = GetViewMatrixInv();
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
        str += "  inv view matrix:\n";
        for (int i = 0; i < 4; i++) {
            str += "    ";
            for (int j = 0; j < 4; j++) {
                str += std::to_string(inv_view_matrix[i][j]);
                if (j < 3) {
                    str += ", ";
                }
            }
            str += "\n";
        }
        str += "  front: (" + std::to_string(m_front[0]) + ", " + std::to_string(m_front[1]) + ", " + std::to_string(m_front[2]) + ")\n";
        str += "  right: (" + std::to_string(m_right[0]) + ", " + std::to_string(m_right[1]) + ", " + std::to_string(m_right[2]) + ")\n";
        str += "  up: (" + std::to_string(m_up[0]) + ", " + std::to_string(m_up[1]) + ", " + std::to_string(m_up[2]) + ")\n";
        str += "  forward: (" + std::to_string(m_forward[0]) + ", " + std::to_string(m_forward[1]) + ", " + std::to_string(m_forward[2]) + ")\n";
        Vector4f planes[6];
        GetPlanes(planes);
        str += "  planes:\n";
        for (int i = 0; i < 6; i++) {
            str += "    ";
            for (int j = 0; j < 4; j++) {
                str += std::to_string(planes[i][j]);
                if (j < 3) {
                    str += ", ";
                }
            }
            str += "\n";
        }
        auto frustum = GetFrustum();
        str += "  frustum: (" + std::to_string(frustum[0]) + ", " + std::to_string(frustum[1]) + ", ";
        str += std::to_string(frustum[2]) + ", " + std::to_string(frustum[3]) + ")\n";
        str += "}\n";

        return str;
    }

    float2 Camera::GetJitter() const noexcept {
        return jitter;
    }

}// namespace Moer
