#include "loader/mixml/Parser.h"
#include "loader/mixml/XmlObject.h"
#include "pugixml.hpp"

namespace Moer::MiXml {

struct GlobalCursor {
    Object*                                      current_obj = nullptr;
    Moer::Array<std::unique_ptr<Object>>         objects_pool;
    Moer::UnorderedMap<std::string, std::string> global_params;
    Moer::UnorderedMap<std::string, Object*>     ref_objects_map;

    void AddGlobalParam(std::string, std::string) noexcept;

    void ReplaceDefaultValue(pugi::xml_node*) noexcept;
};

struct Parser::Impl {
    GlobalCursor global_cursor;

    void DfsParse(pugi::xml_node node) noexcept;
    bool Visit(ETag, pugi::xml_node node) noexcept;
};

Parser::Parser() noexcept {
    m_impl = new Impl();
}

Parser::~Parser() noexcept {
    if (m_impl)
        delete m_impl;
}

Object* Parser::LoadFromFile(std::filesystem::path file_path) noexcept {
    pugi::xml_document     doc;
    pugi::xml_parse_result result = doc.load_file(file_path.c_str());
    m_impl->DfsParse(doc.document_element());

    return m_impl->global_cursor.objects_pool[0].get();
}

void Parser::Impl::DfsParse(pugi::xml_node node) noexcept {
    ETag tag = ETag::UNKNOWN;
    if (auto it = S_TAG_MAP.find(std::string{node.name()}); it != S_TAG_MAP.end())
        tag = it->second;

    auto obj_parent = global_cursor.current_obj;

    bool flag = Visit(tag, node);
    if (!flag)
        return;

    for (pugi::xml_node& child : node.children())
        DfsParse(child);

    global_cursor.current_obj = obj_parent;
}

void GlobalCursor::AddGlobalParam(std::string name, std::string value) noexcept {
    global_params[name] = value;
}

void GlobalCursor::ReplaceDefaultValue(pugi::xml_node* node) noexcept {
    for (auto attr : node->attributes()) {
        std::string a_value = attr.value();
        if (a_value.find('$') == std::string::npos)
            continue;
        for (auto& [p_name, p_value] : global_params) {
            size_t pos       = 0;
            auto   temp_name = "$" + p_name;
            while ((pos = a_value.find(temp_name, pos)) != std::string::npos) {
                a_value.replace(pos, temp_name.length(), p_value);
                pos += p_value.length();
            }
        }
        attr.set_value(a_value.c_str());
    }
}

namespace {
inline bool PropertyVisitor(ETag tag, GlobalCursor& global_cursor, pugi::xml_node& node) {
    global_cursor.ReplaceDefaultValue(&node);
    std::string name = node.attribute("name").value();
    if (name.empty())
        name = node.name();
    std::string value = node.attribute("value").value();
    if (global_cursor.current_obj) {
        global_cursor.current_obj->properties.emplace_back(name, value);
    }
    return true;
}

inline bool ObjectVisitor(ETag tag, GlobalCursor& global_cursor, pugi::xml_node& node) {
    global_cursor.ReplaceDefaultValue(&node);
    auto obj     = std::make_unique<Object>(node.name(), node.attribute("type").value(), tag);
    auto id_attr = node.attribute("id");
    if (!id_attr.empty()) {
        obj->id                                = id_attr.value();
        global_cursor.ref_objects_map[obj->id] = obj.get();
    }
    auto name_attr = node.attribute("name");
    if (!name_attr.empty()) {
        obj->var_name = name_attr.value();
    }
    if (global_cursor.current_obj) {
        global_cursor.current_obj->sub_object.emplace_back(obj.get());
    }

    global_cursor.current_obj = obj.get();
    global_cursor.objects_pool.emplace_back(std::move(obj));
    return true;
}

inline bool XYZValuePropertyVisitor(
    ETag             tag,
    GlobalCursor&    global_cursor,
    pugi::xml_node&  node,
    std::string_view default_x,
    std::string_view default_y,
    std::string_view default_z
) {
    global_cursor.ReplaceDefaultValue(&node);
    std::string name = node.attribute("name").value();
    if (name.empty())
        name = node.name();
    std::string value = node.attribute("value").value();
    if (value.empty()) {
        std::string x = node.attribute("x").value();
        std::string y = node.attribute("y").value();
        std::string z = node.attribute("z").value();

        if (x.empty())
            x = default_x;
        if (y.empty())
            y = default_y;
        if (z.empty())
            z = default_z;
        value = x + "," + y + "," + z;
    }
    if (global_cursor.current_obj) {
        global_cursor.current_obj->properties.emplace_back(name, value);
    }
    return true;
}
} // namespace

bool Parser::Impl::Visit(ETag tag, pugi::xml_node node) noexcept {
    switch (tag) {
        case ETag::SCENE: {
            auto scene_root =
                std::make_unique<Object>(node.name(), node.attribute("version").value(), ETag::SCENE);
            global_cursor.current_obj = scene_root.get();
            global_cursor.objects_pool.emplace_back(std::move(scene_root));
            return true;
        }
        case ETag::DEFAULT: {
            global_cursor.ReplaceDefaultValue(&node);
            std::string name  = node.attribute("name").value();
            std::string value = node.attribute("value").value();
            global_cursor.AddGlobalParam(name, value);
            return true;
        }
        case ETag::REF: {
            global_cursor.ReplaceDefaultValue(&node);
            auto id_attr = node.attribute("id");
            if (!id_attr.empty()) {
                if (global_cursor.ref_objects_map.find(id_attr.value()) !=
                    global_cursor.ref_objects_map.end())
                    global_cursor.current_obj->sub_object.emplace_back(
                        global_cursor.ref_objects_map[id_attr.value()]
                    );
            }
            return true;
        }
        case ETag::LOOKAT: {
            /*<lookat origin="1, 1, 1" target="1, 2, 1" up="0, 0, 1"/>*/
            global_cursor.ReplaceDefaultValue(&node);
            auto lookat_obj = std::make_unique<Object>(node.name(), "", ETag::LOOKAT);
            auto origin     = node.attribute("origin");
            lookat_obj->properties.emplace_back(origin.name(), origin.value());
            auto target = node.attribute("target");
            lookat_obj->properties.emplace_back(target.name(), target.value());
            auto up = node.attribute("up");
            lookat_obj->properties.emplace_back(up.name(), up.value());

            if (global_cursor.current_obj) {
                global_cursor.current_obj->sub_object.emplace_back(lookat_obj.get());
            }
            global_cursor.objects_pool.emplace_back(std::move(lookat_obj));
            return true;
        }
        case ETag::ROTATE: {
            // <rotate value="0.701, 0.701, 0.701" angle="180"/>
            // <rotate y="1" angle="45"/>
            global_cursor.ReplaceDefaultValue(&node);
            auto        rotate_obj = std::make_unique<Object>(node.name(), "", ETag::ROTATE);
            std::string axis{};
            if (!node.attribute("value").empty()) {
                axis = node.attribute("value").value();
            } else if (!node.attribute("x").empty()) {
                axis = "1, 0, 0";
            } else if (!node.attribute("y").empty()) {
                axis = "0, 1, 0";
            } else if (!node.attribute("z").empty()) {
                axis = "0, 0, 1";
            }
            rotate_obj->properties.emplace_back("axis", axis);

            auto angle = node.attribute("angle");
            rotate_obj->properties.emplace_back(angle.name(), angle.value());

            if (global_cursor.current_obj) {
                global_cursor.current_obj->sub_object.emplace_back(rotate_obj.get());
            }
            global_cursor.objects_pool.emplace_back(std::move(rotate_obj));
            return true;
        }

        case ETag::SCALE:
            // <scale value="5"/>
            // <scale value="2, 1, -1"/>
            // <scale x="4" y="2"/>
            return XYZValuePropertyVisitor(ETag::SCALE, global_cursor, node, "1", "1", "1");
        case ETag::POINT:

            // <point name="center" value="1,1,1"/>
            // <point name="center" x="1" y="0" z="0"/>
            return XYZValuePropertyVisitor(ETag::POINT, global_cursor, node, "0", "0", "0");

        case ETag::TRANSLATE:
            // <translate x="1" y="0" z="0"/>
            return XYZValuePropertyVisitor(ETag::TRANSLATE, global_cursor, node, "0", "0", "0");

        case ETag::INTEGER:
        case ETag::STRING:
        case ETag::FLOAT:
        case ETag::RGB:
        case ETag::VECTOR:
        case ETag::BOOLEAN:
        case ETag::MATRIX:
            return PropertyVisitor(tag, global_cursor, node);

        case ETag::BSDF:
        case ETag::EMITTER:
        case ETag::FILM:
        case ETag::INTEGRATOR:
        case ETag::SENSOR:
        case ETag::SHAPE:
        case ETag::TEXTURE:
        case ETag::TRANSFORM:
            return ObjectVisitor(tag, global_cursor, node);
    }
    return false;
}

} // namespace Moer::MiXml