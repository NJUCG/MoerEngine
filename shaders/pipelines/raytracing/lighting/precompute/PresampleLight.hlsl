#include <lighting/lib/restir/Bindings.hlsli>
#include <lighting/lib/restir/PresampleFunctions.hlsli>

[numthreads(DI_PRESAMPLE_GRID_SIZE, 1, 1)] void main(uint3 dtid
                                  : SV_DispatchThreadID) {
  Moer::RandomState rng = Moer::RandomState::Create(dtid.xy, resample_params.frame_idx);

  TextureHandle local_light_pdf_handle =
      TextureHandle(resample_params.bindless_handles.local_light_pdf);
  Texture2D<float> local_light_pdf_tex =
      local_light_pdf_handle.GetTexture2D<float>();

  uint tile_idx = dtid.y;
  uint in_tile_idx = dtid.x;

  Moer::SampleFunc::SampleLocalLights(
      rng, local_light_pdf_tex, resample_params.local_light_pdf_size, tile_idx,
      in_tile_idx, resample_params.light_buffer_params.local_light_region,
      resample_params.local_light_ris_buffer_params);
}