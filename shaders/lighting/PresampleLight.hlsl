#include <framework/DI/Bindings.hlsli>
#include <framework/DI/PresampleFunctions.hlsli>

[num_threads(256, 1, 1)] void main(uint3 dtid : SV_DispatchThreadID) {

    RandomState rng = RandomState::Init(dtid.xy, 0);

    TextureHandle local_light_pdf_handle = TextureHandle(resample_params.bindless_handles.local_light_pdf);
    Texture2D<float> local_light_pdf_tex = local_light_pdf_handle.GetTexture2D<float>();
    Moer::SampleFunc::SampleLocalLights(
        rng,
        local_light_pdf_tex,
        dtid.x,
        dtid.y,
        resample_params.light_buffer_params.local_light_region,
        resample_params.local_light_ris_buffer_params
    );
}