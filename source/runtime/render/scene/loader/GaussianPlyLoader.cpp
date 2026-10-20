#include "GaussianPlyLoader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace Moer {
namespace {
struct Property {
    std::string name;
    std::string type;
    size_t      byte_count;
};

size_t ScalarSize(const std::string& type) {
    if (type == "char" || type == "uchar" || type == "int8" || type == "uint8")
        return 1;
    if (type == "short" || type == "ushort" || type == "int16" || type == "uint16")
        return 2;
    if (type == "int" || type == "uint" || type == "int32" || type == "uint32" || type == "float" ||
        type == "float32")
        return 4;
    if (type == "double" || type == "float64")
        return 8;
    throw std::runtime_error("Unsupported PLY scalar type: " + type);
}

template<typename T>
double ReadScalar(std::istream& input) {
    T value{};
    if (!input.read(reinterpret_cast<char*>(&value), sizeof(value))) {
        throw std::runtime_error("Truncated binary PLY vertex data");
    }
    return static_cast<double>(value);
}

double ReadBinary(std::istream& input, const std::string& type) {
    if (type == "char" || type == "int8")
        return ReadScalar<int8_t>(input);
    if (type == "uchar" || type == "uint8")
        return ReadScalar<uint8_t>(input);
    if (type == "short" || type == "int16")
        return ReadScalar<int16_t>(input);
    if (type == "ushort" || type == "uint16")
        return ReadScalar<uint16_t>(input);
    if (type == "int" || type == "int32")
        return ReadScalar<int32_t>(input);
    if (type == "uint" || type == "uint32")
        return ReadScalar<uint32_t>(input);
    if (type == "float" || type == "float32")
        return ReadScalar<float>(input);
    return ReadScalar<double>(input);
}

float Sigmoid(float value) {
    if (value >= 0.f)
        return 1.f / (1.f + std::exp(-value));
    const float exp_value = std::exp(value);
    return exp_value / (1.f + exp_value);
}
} // namespace

GaussianLoadResult LoadGaussianPly(const std::filesystem::path& path) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("Cannot open Gaussian PLY: " + path.string());
        std::string line;
        const auto  next_line = [&]() {
            if (!std::getline(input, line))
                throw std::runtime_error("Truncated PLY header");
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
        };
        next_line();
        if (line != "ply")
            throw std::runtime_error("Missing PLY magic");
        bool     is_ascii = false, has_format = false, has_vertices = false, is_in_vertices = false;
        bool     has_header_ended = false;
        uint64_t vertex_count     = 0;
        size_t   header_bytes     = 4;
        std::vector<Property> properties;
        while (!has_header_ended) {
            next_line();
            header_bytes += line.size() + 1;
            if (header_bytes > 1024 * 1024)
                throw std::runtime_error("PLY header exceeds 1 MiB");
            std::istringstream fields(line);
            std::string        keyword;
            fields >> keyword;
            if (keyword == "format") {
                std::string format, version;
                fields >> format >> version;
                if (has_format || version != "1.0" ||
                    (format != "ascii" && format != "binary_little_endian")) {
                    throw std::runtime_error("Expected PLY 1.0 ASCII or binary_little_endian");
                }
                has_format = true;
                is_ascii   = format == "ascii";
            } else if (keyword == "element") {
                std::string name, count_text;
                fields >> name >> count_text;
                size_t consumed_count = 0;
                if (count_text.empty() || count_text.front() == '-')
                    throw std::runtime_error("Invalid element count");
                const uint64_t count = std::stoull(count_text, &consumed_count);
                if (consumed_count != count_text.size())
                    throw std::runtime_error("Invalid element count");
                if (name == "vertex") {
                    if (has_vertices)
                        throw std::runtime_error("Duplicate vertex element");
                    has_vertices = is_in_vertices = true;
                    vertex_count                  = count;
                } else {
                    // Strict Gaussian assets contain only vertices (zero-sized extra elements are harmless).
                    if (count != 0)
                        throw std::runtime_error(
                            "Non-vertex PLY elements are not supported for Gaussian assets"
                        );
                    is_in_vertices = false;
                }
            } else if (keyword == "property" && is_in_vertices) {
                Property property;
                fields >> property.type >> property.name;
                if (property.type == "list" || property.name.empty())
                    throw std::runtime_error("Expected scalar vertex property");
                property.byte_count = ScalarSize(property.type);
                properties.push_back(std::move(property));
                if (properties.size() > 256)
                    throw std::runtime_error("Too many PLY vertex properties");
            } else if (keyword == "end_header") {
                has_header_ended = true;
            } else if (
                keyword != "comment" && keyword != "obj_info" && !keyword.empty() && keyword != "property"
            ) {
                throw std::runtime_error("Unsupported PLY header directive: " + keyword);
            }
        }
        if (!has_format || !has_vertices)
            throw std::runtime_error("Missing PLY format or vertex element");
        if (!is_ascii && std::endian::native != std::endian::little)
            throw std::runtime_error("Unsupported host byte order");
        // One typed GPU buffer has a uint32 byte size in the current RHI.
        if (vertex_count > std::numeric_limits<uint32_t>::max() / sizeof(Render::GGaussian)) {
            throw std::runtime_error("Gaussian asset exceeds current RHI buffer size limit");
        }
        std::unordered_map<std::string, size_t> indices;
        size_t                                  vertex_byte_stride = 0;
        for (size_t property_id = 0; property_id < properties.size(); ++property_id) {
            if (!indices.emplace(properties[property_id].name, property_id).second)
                throw std::runtime_error("Duplicate PLY property: " + properties[property_id].name);
            vertex_byte_stride += properties[property_id].byte_count;
        }
        const auto require = [&](const std::string& name) {
            const auto it = indices.find(name);
            if (it == indices.end())
                throw std::runtime_error("Missing standard 3DGS property: " + name);
            return it->second;
        };
        std::array<size_t, 3> position_property_ids, scale_property_ids, dc_property_ids;
        std::array<size_t, 4> rotation_property_ids;
        for (int property_id = 0; property_id < 3; ++property_id) {
            position_property_ids[property_id] = require(std::string(1, "xyz"[property_id]));
            scale_property_ids[property_id]    = require("scale_" + std::to_string(property_id));
            dc_property_ids[property_id]       = require("f_dc_" + std::to_string(property_id));
        }
        for (int property_id = 0; property_id < 4; ++property_id)
            rotation_property_ids[property_id] = require("rot_" + std::to_string(property_id));
        const size_t opacity_property_id = require("opacity");
        size_t       rest_count          = 0;
        for (const auto& property : properties)
            if (property.name.starts_with("f_rest_"))
                ++rest_count;
        uint32 sh_degree = 0;
        if (rest_count == 9)
            sh_degree = 1;
        else if (rest_count == 24)
            sh_degree = 2;
        else if (rest_count == 45)
            sh_degree = 3;
        else if (rest_count != 0)
            throw std::runtime_error("SH coefficients must describe degree 0, 1, 2 or 3");
        std::vector<size_t> rest_property_ids;
        for (size_t property_id = 0; property_id < rest_count; ++property_id)
            rest_property_ids.push_back(require("f_rest_" + std::to_string(property_id)));
        const auto data_start = input.tellg();
        input.seekg(0, std::ios::end);
        const auto data_bytes = input.tellg() - data_start;
        if (data_bytes < 0 ||
            (!is_ascii && static_cast<uint64_t>(data_bytes) < vertex_count * vertex_byte_stride) ||
            (is_ascii && static_cast<uint64_t>(data_bytes) < vertex_count * properties.size())) {
            throw std::runtime_error("Truncated PLY vertex payload");
        }
        input.seekg(data_start);
        auto asset         = std::make_shared<GaussianAsset>();
        asset->source_path = std::filesystem::absolute(path).lexically_normal();
        asset->sh_degree   = sh_degree;
        asset->gaussians.reserve(static_cast<size_t>(vertex_count));
        std::vector<float> values(properties.size());
        for (uint64_t vertex_id = 0; vertex_id < vertex_count; ++vertex_id) {
            for (size_t property_id = 0; property_id < properties.size(); ++property_id) {
                double number;
                if (is_ascii) {
                    if (!(input >> number))
                        throw std::runtime_error("Invalid or truncated ASCII PLY vertex");
                } else
                    number = ReadBinary(input, properties[property_id].type);
                if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) {
                    throw std::runtime_error("PLY vertex contains non-finite or out-of-range values");
                }
                values[property_id] = static_cast<float>(number);
            }
            Render::GGaussian gaussian{};
            gaussian.position = float4(
                values[position_property_ids[0]],
                values[position_property_ids[1]],
                values[position_property_ids[2]],
                1.f
            );
            const float3 scale(
                std::exp(values[scale_property_ids[0]]),
                std::exp(values[scale_property_ids[1]]),
                std::exp(values[scale_property_ids[2]])
            );
            if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z) ||
                scale.x <= 0.f || scale.y <= 0.f || scale.z <= 0.f)
                throw std::runtime_error("Invalid Gaussian scale after exp");
            gaussian.scale_opacity = float4(scale.x, scale.y, scale.z, Sigmoid(values[opacity_property_id]));
            const double w = values[rotation_property_ids[0]], x = values[rotation_property_ids[1]],
                         y = values[rotation_property_ids[2]], z = values[rotation_property_ids[3]];
            const double length = std::sqrt(w * w + x * x + y * y + z * z);
            if (!(length > 1e-12))
                throw std::runtime_error("Zero-length Gaussian quaternion");
            gaussian.rotation = float4(
                static_cast<float>(x / length),
                static_cast<float>(y / length),
                static_cast<float>(z / length),
                static_cast<float>(w / length)
            );
            gaussian.sh[0] = float4(
                values[dc_property_ids[0]], values[dc_property_ids[1]], values[dc_property_ids[2]], 0.f
            );
            const size_t coefficients_per_channel = rest_count / 3;
            for (size_t property_id = 0; property_id < coefficients_per_channel; ++property_id) {
                gaussian.sh[property_id + 1] = float4(
                    values[rest_property_ids[property_id]],
                    values[rest_property_ids[coefficients_per_channel + property_id]],
                    values[rest_property_ids[2 * coefficients_per_channel + property_id]],
                    0.f
                );
            }
            // Conservative rotation-independent 3-sigma support; exact filtering is a rendering decision.
            const float  radius = 3.f * std::max({scale.x, scale.y, scale.z});
            const float3 center(gaussian.position.x, gaussian.position.y, gaussian.position.z);
            const float3 min_bounds = center - float3(radius), max_bounds = center + float3(radius);
            if (!std::isfinite(radius) || !std::isfinite(min_bounds.x) || !std::isfinite(min_bounds.y) ||
                !std::isfinite(min_bounds.z) || !std::isfinite(max_bounds.x) ||
                !std::isfinite(max_bounds.y) || !std::isfinite(max_bounds.z))
                throw std::runtime_error("Gaussian bounds overflow");
            asset->bounds.Expand(min_bounds);
            asset->bounds.Expand(max_bounds);
            asset->gaussians.push_back(gaussian);
        }
        return {std::move(asset), {}};
    } catch (const std::exception& error) {
        return {nullptr, error.what()};
    }
}
} // namespace Moer
