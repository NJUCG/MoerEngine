#ifndef MOER_ALGORITHM_ALIAS_TABLE_H
#define MOER_ALGORITHM_ALIAS_TABLE_H

#include "misc/STL.h"
#include <span>
namespace Moer::Render::Algo {
    struct AliasTable {
        Array<float> prob;
        Array<int>   alias;

        AliasTable(std::span<const float> _src_value);
    };
}// namespace Moer::Render::Algo

#endif