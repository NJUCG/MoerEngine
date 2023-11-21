#include "renderer/UIRenderer.h"

#include "rhi/RHI.h"
#include "rhi/RHICommandList.h"
#include "rhi/RHICommandQueue.h"

#include "ui/ImGUIRenderer.h"

UIRenderer* ui_renderer = nullptr;

UIRenderer* UIRenderer::GetRenderer() {
    if (ui_renderer == nullptr)
        ui_renderer = new ImGUIRenderer();
    return ui_renderer;
}