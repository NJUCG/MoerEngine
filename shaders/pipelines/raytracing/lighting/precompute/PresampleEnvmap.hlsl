#include <pipelines/raytracing/lighting/lib/restir/Bindings.hlsli>
#include <pipelines/raytracing/lighting/lib/restir/PresampleFunctions.hlsli>

[numthreads(256, 1, 1)] void main(uint3 dtid
                                  : SV_DispatchThreadID) {
  Moer::RandomState rng = Moer::RandomState::Create(dtid.xy, resample_params.frame_idx);

  uint tile_idx = dtid.y;
  uint in_tile_idx = dtid.x;
  TextureHandle env_pdf_handle =
      TextureHandle(resample_params.bindless_handles.env_pdf);
  Texture2D<float> env_pdf_tex = env_pdf_handle.GetTexture2D<float>();
  Moer::SampleFunc::SampleEnvMap(rng, env_pdf_tex, resample_params.env_pdf_size,
                                 tile_idx, in_tile_idx,
                                 resample_params.env_light_ris_buffer_params);
}