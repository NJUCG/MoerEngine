#include "metis/include/metis.h"
#include <vector>

void Test(){
decltype(METIS_PartGraphRecursive)* metis_patition_func = METIS_PartGraphRecursive;

    decltype(METIS_MeshToDual)* metis_mesh_to_dual_func = METIS_MeshToDual;

    

    static int32_t header_line[]{7,1};
    static std::vector<std::vector<int32_t>> mesh_data{
        {},
        {},
        {},
        {},
        {},
        {},
        {}
    };
    int num_element = 813;
    int num_nodes = 813 * 3;

    std::vector<int32_t> eptr(num_element+1);
    std::vector<int32_t> eind(num_nodes);//num indices

    constexpr int32_t num_node_per_element = 3; //triangle mesh
    int32_t current_offset = 0;
    for(int i = 0; i < num_element + 1; i++){
        eptr[i] = current_offset;
        current_offset += num_node_per_element;
    }

    //two triangle is connected when they have two common nodes
    int num_common_node = 2;
    int num_flag = 0;

    idx_t* xadj, *adjncy;
    metis_mesh_to_dual_func(
        &num_element,
        &num_nodes,
        eptr.data(),
        eind.data(),
        &num_common_node,
        &num_flag,
        &xadj,
        &adjncy
    );
}