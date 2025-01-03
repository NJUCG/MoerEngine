#ifndef MOER_NRD_SHARED_H
#define MOER_NRD_SHARED_H

// NRD variant
#define NORMAL                0
#define SH                    1// NORMAL + SH (SG) resolve
#define OCCLUSION             2
#define DIRECTIONAL_OCCLUSION 3// OCCLUSION + SH (SG) resolve

// Denoiser
#define DENOISER_REBLUR    0
#define DENOISER_RELAX     1
#define DENOISER_REFERENCE 2

// Normal encoding variants ( match NormalEncoding )
#define NRD_NORMAL_ENCODING_RGBA8_UNORM       0
#define NRD_NORMAL_ENCODING_RGBA8_SNORM       1
#define NRD_NORMAL_ENCODING_R10G10B10A2_UNORM 2// supports material ID bits
#define NRD_NORMAL_ENCODING_RGBA16_UNORM      3
#define NRD_NORMAL_ENCODING_RGBA16_SNORM      4// also can be used with FP formats

// Roughness encoding variants ( match RoughnessEncoding )
#define NRD_ROUGHNESS_ENCODING_SQ_LINEAR   0// linearRoughness * linearRoughness
#define NRD_ROUGHNESS_ENCODING_LINEAR      1// linearRoughness
#define NRD_ROUGHNESS_ENCODING_SQRT_LINEAR 2// sqrt( linearRoughness )

#define NRD_FP16_MAX                 65504.0
#define NRD_PI                       3.14159265358979323846
#define NRD_EPS                      1e-6
#define NRD_REJITTER_VIEWZ_THRESHOLD 0.01// normalized %
#define NRD_ROUGHNESS_EPS \
    sqrt(sqrt(NRD_EPS))// "m2" fitting in FP32 to "linear roughness"
#define NRD_INF 1e6

#endif