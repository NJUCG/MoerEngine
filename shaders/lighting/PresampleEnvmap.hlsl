#include <framework/DI/Bindings.hlsli>
#include <framework/DI/PresampleFunctions.hlsli>

[num_threads(256, 1, 1)] void main(uint3 dtid : SV_DispatchThreadID) {

    RandomState rng = RandomState::Init(dtid.xy, 0);

    TextureHandle env_pdf_handle = TextureHandle(resample_params.bindless_handles.env_pdf);
    Texture2D<float> env_pdf_tex = env_pdf_handle.GetTexture2D<float>();
    Moer::SampleFunc::SampleEnvMap(
        rng,
        env_pdf_tex,
        resample_params.env_pdf_size,
        dtid.x,
        dtid.y,
        resample_params.env_light_ris_buffer_params
    );
}