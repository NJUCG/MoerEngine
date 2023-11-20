#ifndef MOER_ENGINE_GUI_CONTEXT_H
#define MOER_ENGINE_GUI_CONTEXT_H
#include <imgui.h>

class GUIContext {
public:
    GUIContext();
    ~GUIContext();

    void Initialize();
    void Shutdown();

    void BeginFrame();
    void EndFrame();

    // Add more member functions for handling viewports, windows, and resources

private:
    // Add member variables for ImGui context and other GUI-related resources
};

GUIContext::GUIContext() {
    // Constructor implementation
}

GUIContext::~GUIContext() {
    // Destructor implementation
}

void GUIContext::Initialize() {
    // Initialize ImGui and other GUI-related resources
    ImGui::CreateContext();
    // Add more initialization code as needed
}

void GUIContext::Shutdown() {
    // Shutdown ImGui and release other GUI-related resources
    ImGui::DestroyContext();
    // Add more shutdown code as needed
}

void GUIContext::BeginFrame() {
    // Start a new ImGui frame
    ImGui::NewFrame();
    // Add more frame-related code as needed
}

void GUIContext::EndFrame() {
    // End the current ImGui frame and render GUI
    ImGui::Render();
    // Add more frame-related code as needed
}
#endif// !MOER_ENGINE_GUI_CONTEXT_H