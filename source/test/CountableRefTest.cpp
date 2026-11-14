#include <cstdint>
#include "misc/CountableRef.h"

#include <iostream>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
struct TrackedResource {
    int references = 0;
    int acquisitions = 0;
    int releases = 0;
    int AddRef() { ++acquisitions; return ++references; }
    int DeRef() { ++releases; return --references; }
    int GetRefCount() const { return references; }
};
struct DerivedResource : TrackedResource {};

static_assert(std::is_nothrow_move_constructible_v<CountableRef<TrackedResource>>);
static_assert(std::is_nothrow_constructible_v<CountableRef<TrackedResource>, CountableRef<DerivedResource>&&>);
}

int main() {
    TrackedResource resource;
    {
        std::vector<CountableRef<TrackedResource>> owners;
        owners.reserve(1);
        owners.emplace_back(&resource);
        owners.reserve(32);
        if (resource.references != 1 || resource.acquisitions != 1 || resource.releases != 0 ||
            owners.front().Get() != &resource) {
            std::cerr << "Vector growth must transfer an existing reference without acquiring or releasing it.\n";
            return 1;
        }
    }
    if (resource.references != 0 || resource.releases != 1) { return 1; }

    DerivedResource derived;
    {
        CountableRef<DerivedResource> source(&derived);
        CountableRef<TrackedResource> destination(std::move(source));
        if (source.IsValid() || destination.Get() != &derived ||
            derived.references != 1 || derived.acquisitions != 1 || derived.releases != 0) {
            std::cerr << "Converting move must transfer exactly one reference and clear its source.\n";
            return 1;
        }
    }
    if (derived.references != 0 || derived.releases != 1) { return 1; }
    std::cout << "CountableRef ownership transfer checks passed.\n";
}
