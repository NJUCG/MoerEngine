#include <framework/DI/Bindings.hlsli>
#include <framework/DI/PresampleFunctions.hlsli>

[num_threads(256, 1, 1)] void main(uint dtid : SV_DispatchThreadID) {

    RandomState rng = RandomState::Init(uint2(gtid & 0xfff, gtid >> 12), 1);
    RandomState coherent_rng = RandomState::Init(uint2(gtid >> 8, 0), 1);

    Moer::SampleFunc::SampleLocalLightsForGrid(
        rng,
        coherent_rng,
        dtid,
        resample_params.light_buffer_params.local_light_region,
        resample_params.local_light_ris_buffer_params,
        resample_params.grid_params
    );
}