#pragma once

#include "Tag.h"

namespace Moer::Resource::MiXml {
    struct Property {
        std::string name;
        std::string value;

        Property(std::string_view name, std::string_view value) noexcept
            : name(name), value(value) {}
    };

    struct Object {
        const ETag            tag;
        std::string           obj_name;
        std::string           var_name;
        std::string           id;
        std::string           type;
        Moer::Array<Property> properties;
        Moer::Array<Object*>  sub_object;

        Object(std::string_view obj_name, std::string_view type, ETag obj_tag = ETag::UNKNOWN) noexcept
            : obj_name(obj_name), type(type), tag(obj_tag) {}

        std::string          GetProperty(std::string_view) const noexcept;
        Object*              GetUniqueSubObject(std::string_view) const noexcept;
        Moer::Array<Object*> GetSubObjects(std::string_view) const noexcept;

        std::pair<Object*, std::string> GetParameter(std::string_view) const noexcept;
    };
}// namespace Moer::Resource::MiXml