#ifndef MOERENGINE_WINDOW_INPUT_H
#define MOERENGINE_WINDOW_INPUT_H

namespace Moer {
    struct WindowInput{
        //cursor coods
        float lastX = 0, lastY = 0;

        bool firstMouse = true;
        // bool firstEnter = true;

        //timing(in order to perform equally on every device)
        float deltaTime = 0.0f;
        float lastFrame = 0.0f;

        //keyboard input
        bool camera_forward  = false;
        bool camera_backward = false;
        bool camera_left     = false;
        bool camera_right    = false;
        bool camera_up       = false;
        bool camera_down     = false;

        //mouse movement
        float deltaX = 0.0f;
        float deltaY = 0.0f;

        //camera speed(default)
        float cameraSpeed = 25.0f;    //to be optimized
        bool  speedUp     = false;
        bool  speedDown   = false;
        bool  resetSpeed  = false;

        //window size
        float width        = 1280.f;
        float height       = 720.f;
        float aspect_ratio = width / height;

        //fov
        float fov = 100.f;

        //mouse right button setting
        bool mouseEnterScreen = false;

        static WindowInput& GetInstance();
    };

    inline WindowInput& WindowInput::GetInstance() {
        static WindowInput wndInput;
        return wndInput;
    }
} // namespace Moer

#endif