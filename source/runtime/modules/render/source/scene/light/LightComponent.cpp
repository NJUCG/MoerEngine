#include "scene/light/LightComponent.h"

#include "scene/light/DirectionalLightComponent.h"
#include "scene/light/PointLightComponent.h"
#include "misc/STL.h"

namespace Moer {

    Array<LightComponentRef> LightComponent::CreateDefaultLightComponents() {
        Array<LightComponentRef> lights;

        // Directional light
        {
            LightComponentRef directional_light = MoerNew(DirectionalLightComponent)(
                Vector3f(1.0f, 1.0f, 1.0f),   // color
                1.0f,                         // intensity
                Vector3f(-1.0f, -2.0f, -1.0f),// direction
                0.f);
            lights.push_back(directional_light);
        }

        // Point light
        // {
        //     // The following code is copied from DeferredRenderer::Impl::InitSceneResources() and is modified a bit.
        //     // The following lights should be placed in the Sponza scene.
        //
        //     auto light_pos   = Moer::Vector3f(0.0f, 128.0f, -225.0f);
        //     auto light_color = Moer::Vector3f(1.0, 1.0, 1.0);
        //     // Magic numbers used to offset lights in the Sponza scene
        //     for (int i = -4; i < 4; ++i) {
        //         for (int j = 0; j < 2; ++j) {
        //             Moer::Vector3f pos = light_pos;
        //             pos.x += i * 400;
        //             pos.z += j * (225 + 140);
        //             pos.y = 8;
        //
        //             for (int k = 0; k < 3; ++k) {
        //                 pos.y = pos.y + (k * 100);
        //
        //                 light_color.x = static_cast<float>(rand()) / (RAND_MAX);
        //                 light_color.y = static_cast<float>(rand()) / (RAND_MAX);
        //                 light_color.z = static_cast<float>(rand()) / (RAND_MAX);
        //
        //                 LightComponentRef point_light = MoerNew(PointLightComponent)(light_color, 1.0f, pos);
        //
        //                 lights.push_back(point_light);
        //             }
        //         }
        //     }
        // }

        return lights;
    }

}// namespace Moer