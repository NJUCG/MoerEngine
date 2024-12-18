#include <framework/Bindless.hlsl>
#include <framework/Common.hlsl>
#include <shared/lighting/ShaderParameters.h>

BINDLESS_BINDINGS(3, 2, 4, 5)

[[vk::push_constant]] ConstantBuffer<Moer::PrepareLightsParams> param;

[[vk::binding(0, 0)]] RWStructuredBuffer<Moer::PolymorphicLightInfo> light_data
    : register(t0);
[[vk::binding(1, 0)]] RWBuffer<uint> light_index_mapping;
    : register(u0);

    [[vk::binding(2,
                  0)]] StructuredBuffer<Moer::PolymorphicLightInfo> prim_lights;
    [[vk::binding(3, 0)]] StructuredBuffer<Moer::PrepareLightsTask> tasks;

    bool FindTask(uint dtid, out Moer::PrepareLightsTask task) {
      // binary search in task buffer
      int left = 0;
      int right = int(param.num_tasks) - 1;

      while (left <= right) {
        int mid = (left + right) / 2;
        task = tasks[mid];
        int tri = int(dtid) - int(task.light_buffer_offset);

        if (tri < 0) {
          right = mid - 1;
        } else if (tri >= int(task.light_buffer_size)) {
          left = mid + 1;
        } else {
          return true;
        }
      }
    }

    [numthreads(256, 1, 1)] void main(uint dtid
                                      : SV_DISPATCHTHREADID, uint gtid
                                      : SV_GROUPTHREADID) {
      Moer::PrepareLightsTask task = (Moer::PrepareLightsTask)0;
      if (!FindTask(dtid, task)) {
        return;
      }

      uint tri_idx = dtid - task.light_buffer_offset;
      bool is_prim_light = (task.instance_geo_idx & g_task_prim_light_bit) != 0;

      Moer::PolymorphicLightInfo light_info = (Moer::PolymorphicLightInfo)0;

      if (is_prim_light) {
        uint prim_light_idx = task.instance_geo_idx & ~g_task_prim_light_bit;
        light_info = prim_lights[prim_light_idx];
      } else {
      }
    }