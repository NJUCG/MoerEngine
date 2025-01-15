#include <framework/DI/Bindings.hlsli>
#include <framework/DI/PresampleFunctions.hlsli>

[numthreads(256, 1, 1)] void main(uint3 dtid
                                  : SV_DispatchThreadID) {
  Moer::RandomState rng = Moer::RandomState::Create(dtid.xy, 0);
//   float4x4 view2world = resample_params.main_view.view2world;
//       printf("view2world: %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f\n",
//          view2world[0][0], view2world[0][1], view2world[0][2],
//          view2world[0][3], view2world[1][0], view2world[1][1],
//          view2world[1][2], view2world[1][3], view2world[2][0],
//          view2world[2][1], view2world[2][2], view2world[2][3],
//          view2world[3][0], view2world[3][1], view2world[3][2],
//          view2world[3][3]);
  TextureHandle env_pdf_handle =
      TextureHandle(resample_params.bindless_handles.env_pdf);
  Texture2D<float> env_pdf_tex = env_pdf_handle.GetTexture2D<float>();
  Moer::SampleFunc::SampleEnvMap(rng, env_pdf_tex, resample_params.env_pdf_size,
                                 dtid.x, dtid.y,
                                 resample_params.env_light_ris_buffer_params);
}