#ifndef MOER_DI_RIS_COMMON_HLSLI

#define MOER_DI_RIS_COMMON_HLSLI

namespace Moer{
    namespace DI{

        struct RISTileInfo{
            uint tile_offset;
            uint tile_size;//as const param
        };

        void RandomlySelectLightDataFromRISTile(
            inout RandomState _rng,
            RISTileInfo _tile_info,
            out uint2 _tile_data,
            out uint _ris_buf_idx
        ){
            float rng = _rng.NextFloat();
            uint ris_sample = min(uint(floor(rng * _tile_info.tile_size)), _tile_info.tile_size - 1);
            _ris_buf_idx = _tile_info.tile_offset + ris_sample;
            _tile_data = rw_ris_buffer[_ris_buf_idx];
        }

        RISTileInfo RandomlySelectRISTile(
            inout RandomState _rng,
            RISBufferSegmentParams _ris_params
        ){
            RISTileInfo result;
            float rng = _rng.NextFloat();
            uint tile_idx = uint(floor(rng * _ris_params.tile_cnt));
            result.tile_offset = tile_idx * _ris_params.tile_size + _ris_params.tile_offset;
            result.tile_size = _ris_params.tile_size;
            return result;
        }


    }
}
#endif