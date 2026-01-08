#include <entt/entt.hpp>
#include <format>
#include <iostream>

struct Position {
    float x;
    float y;
};

struct Velocity {
    float dx;
    float dy;
};

struct Name {
    std::string name;

    // EnTT会自动扫描类型下特定函数签名，并自动绑定destroy
    static void on_destroy(entt::registry& reg, entt::entity entity) {
        std::cout << "Destorying " << reg.get<Name>(entity).name << std::endl;
    }
};

void update(entt::registry& registry) {
    auto view = registry.view<const Position, Velocity>();

    // use a callback
    view.each([](const auto& pos, auto& vel) {
        std::cout << "Pos & Vel: " << pos.x << ", " << pos.y << "; " << vel.dx << ", " << vel.dy << std::endl;
    });

    // use an extended callback
    view.each([](const auto entity, const auto& pos, auto& vel) {
        std::cout << "Pos & Vel [" << entt::to_integral(entity) << "]: " << pos.x << ", " << pos.y << "; "
                  << vel.dx << ", " << vel.dy << std::endl;
    });

    // use a range-for
    for (auto [entity, pos, vel] : view.each()) {
        // ...
    }

    // ======= view 2 =======
    auto view_pos = registry.view<Position, Name>();

    view_pos.each([&](const auto entity, auto& pos, auto& name1) {
        auto& pos1 = view_pos.get<Position>(entity);
        assert(pos.x == pos1.x && pos.y == pos1.y);

        auto& name = view_pos.get<Name>(entity).name;

        // std::cout << name << "; " << entt::to_integral(entity) << ", " << entt::to_version(entity) << ": "
        //           << pos.x << ", " << pos.y << std::endl;
    });

    // ===
    auto view_name = registry.view<Name>();

    view_name.each([&](const auto entity, auto& name) {
        std::cout << name.name << ": " << entt::to_integral(entity) << ", " << entt::to_version(entity)
                  << std::endl;
    });
}

void test_basic() {
    entt::registry registry;

    for (auto i = 0u; i < 10u; ++i) {
        if (i % 3 == 0) {
            for (int j = 0; j < 10; j++) {
                auto entity1 = registry.create();
                if (j % 2 == 0) {
                    registry.destroy(entity1);
                    entity1 = registry.create();
                }
                registry.emplace<Name>(entity1, std::format("Name_Empty_{}_{}", i, j));
            }
        }
        const auto entity = registry.create();
        registry.emplace<Position>(entity, i * 1.f, i * 1.f);
        if (i % 2 == 0) {
            registry.emplace<Velocity>(entity, i * .1f, i * .1f);
        }
        registry.emplace<Name>(entity, std::format("Name_{}", i));
    }

    update(registry);
}

void test_listener_output_info(entt::registry& registry, entt::entity entity) {
    const auto& name = registry.get<Name>(entity);

    std::cout << "Construct Name: " << name.name << ". id = " << entt::to_integral(entity) << std::endl;
}

/**
 * 如何给Component绑定construct listener
 */
void test_listener() {
    std::cout << "\n\n=== Test Listener ===" << std::endl;

    entt::registry registry;

    registry.on_construct<Name>().connect<&test_listener_output_info>();

    for (int i = 0; i < 10; i++) {
        const auto entity = registry.create();
        registry.emplace<Name>(entity, std::format("Name {}", i));
    }

    registry.clear<Name>(); // 手动清除特定类型，来触发auto-binding的on_destroy
}

int main() {

    // Ref: https://github.com/skypjack/entt/wiki/Entity-Component-System

    // 基础用法
    test_basic();
    // 如何给Component绑定construct listener
    test_listener();
}