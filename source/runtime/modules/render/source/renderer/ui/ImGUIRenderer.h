#ifndef MOER_ENGINE_IMGUI_RENDERER_H
#define MOER_ENGINE_IMGUI_RENDERER_H
#include "renderer/UIRenderer.h"

struct UIFrameData;

class ImGUIRenderer : public UIRenderer {

public:
    ImGUIRenderer()          = default;
    virtual ~ImGUIRenderer() = default;
    virtual void Init() override;
    virtual void ShutDown() override;

    virtual void BeginRenderFrame() override;
    virtual void EndRenderFrame() override;

private:
    UIFrameData* frame_data;
};

#endif