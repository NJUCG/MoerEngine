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
    size_t      bytes;
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
    const float e = std::exp(value);
    return e / (1.f + e);
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
        bool                  ascii = false, format_seen = false, vertices_seen = false, in_vertices = false;
        bool                  header_ended = false;
        uint64_t              vertex_count = 0;
        size_t                header_bytes = 4;
        std::vector<Property> properties;
        while (!header_ended) {
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
                if (format_seen || version != "1.0" ||
                    (format != "ascii" && format != "binary_little_endian")) {
                    throw std::runtime_error("Expected PLY 1.0 ASCII or binary_little_endian");
                }
                format_seen = true;
                ascii       = format == "ascii";
            } else if (keyword == "element") {
                std::string name, count_text;
                fields >> name >> count_text;
                size_t consumed = 0;
                if (count_text.empty() || count_text.front() == '-')
                    throw std::runtime_error("Invalid element count");
                const uint64_t count = std::stoull(count_text, &consumed);
                if (consumed != count_text.size())
                    throw std::runtime_error("Invalid element count");
                if (name == "vertex") {
                    if (vertices_seen)
                        throw std::runtime_error("Duplicate vertex element");
                    vertices_seen = in_vertices = true;
                    vertex_count                = count;
                } else {
                    // Strict Gaussian assets contain only vertices (zero-sized extra elements are harmless).
                    if (count != 0)
                        throw std::runtime_error(
                            "Non-vertex PLY elements are not supported for Gaussian assets"
                        );
                    in_vertices = false;
                }
            } else if (keyword == "property" && in_vertices) {
                Property property;
                fields >> property.type >> property.name;
                if (property.type == "list" || property.name.empty())
                    throw std::runtime_error("Expected scalar vertex property");
                property.bytes = ScalarSize(property.type);
                properties.push_back(std::move(property));
                if (properties.size() > 256)
                    throw std::runtime_error("Too many PLY vertex properties");
            } else if (keyword == "end_header") {
                header_ended = true;
            } else if (
                keyword != "comment" && keyword != "obj_info" && !keyword.empty() && keyword != "property"
            ) {
                throw std::runtime_error("Unsupported PLY header directive: " + keyword);
            }
        }
        if (!format_seen || !vertices_seen)
            throw std::runtime_error("Missing PLY format or vertex element");
        if (!ascii && std::endian::native != std::endian::little)
            throw std::runtime_error("Unsupported host byte order");
        // One typed GPU buffer has a uint32 byte size in the current RHI.
        if (vertex_count > std::numeric_limits<uint32_t>::max() / sizeof(Render::GGaussian)) {
            throw std::runtime_error("Gaussian asset exceeds current RHI buffer size limit");
        }
        std::unordered_map<std::string, size_t> indices;
        size_t                                  stride = 0;
        for (size_t i = 0; i < properties.size(); ++i) {
            if (!indices.emplace(properties[i].name, i).second)
                throw std::runtime_error("Duplicate PLY property: " + properties[i].name);
            stride += properties[i].bytes;
        }
        const auto require = [&](const std::string& name) {
            const auto it = indices.find(name);
            if (it == indices.end())
                throw std::runtime_error("Missing standard 3DGS property: " + name);
            return it->second;
        };
        std::array<size_t, 3> xyz, scales, dc;
        std::array<size_t, 4> rotation;
        for (int i = 0; i < 3; ++i) {
            xyz[i]    = require(std::string(1, "xyz"[i]));
            scales[i] = require("scale_" + std::to_string(i));
            dc[i]     = require("f_dc_" + std::to_string(i));
        }
        for (int i = 0; i < 4; ++i)
            rotation[i] = require("rot_" + std::to_string(i));
        const size_t opacity    = require("opacity");
        size_t       rest_count = 0;
        for (const auto& property : properties)
            if (property.name.starts_with("f_rest_"))
                ++rest_count;
        uint32 degree = 0;
        if (rest_count == 9)
            degree = 1;
        else if (rest_count == 24)
            degree = 2;
        else if (rest_count == 45)
            degree = 3;
        else if (rest_count != 0)
            throw std::runtime_error("SH coefficients must describe degree 0, 1, 2 or 3");
        std::vector<size_t> rest;
        for (size_t i = 0; i < rest_count; ++i)
            rest.push_back(require("f_rest_" + std::to_string(i)));
        const auto data_start = input.tellg();
        input.seekg(0, std::ios::end);
        const auto data_bytes = input.tellg() - data_start;
        if (data_bytes < 0 || (!ascii && static_cast<uint64_t>(data_bytes) < vertex_count * stride) ||
            (ascii && static_cast<uint64_t>(data_bytes) < vertex_count * properties.size())) {
            throw std::runtime_error("Truncated PLY vertex payload");
        }
        input.seekg(data_start);
        auto asset         = std::make_shared<GaussianAsset>();
        asset->source_path = std::filesystem::absolute(path).lexically_normal();
        asset->sh_degree   = degree;
        asset->gaussians.reserve(static_cast<size_t>(vertex_count));
        std::vector<float> values(properties.size());
        for (uint64_t vertex = 0; vertex < vertex_count; ++vertex) {
            for (size_t i = 0; i < properties.size(); ++i) {
                double number;
                if (ascii) {
                    if (!(input >> number))
                        throw std::runtime_error("Invalid or truncated ASCII PLY vertex");
                } else
                    number = ReadBinary(input, properties[i].type);
                if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) {
                    throw std::runtime_error("PLY vertex contains non-finite or out-of-range values");
                }
                values[i] = static_cast<float>(number);
            }
            Render::GGaussian g{};
            g.position = float4(values[xyz[0]], values[xyz[1]], values[xyz[2]], 1.f);
            const float3 scale(
                std::exp(values[scales[0]]), std::exp(values[scales[1]]), std::exp(values[scales[2]])
            );
            if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z) ||
                scale.x <= 0.f || scale.y <= 0.f || scale.z <= 0.f)
                throw std::runtime_error("Invalid Gaussian scale after exp");
            g.scale_opacity = float4(scale.x, scale.y, scale.z, Sigmoid(values[opacity]));
            const double w = values[rotation[0]], x = values[rotation[1]], y = values[rotation[2]],
                         z      = values[rotation[3]];
            const double length = std::sqrt(w * w + x * x + y * y + z * z);
            if (!(length > 1e-12))
                throw std::runtime_error("Zero-length Gaussian quaternion");
            g.rotation = float4(
                static_cast<float>(x / length),
                static_cast<float>(y / length),
                static_cast<float>(z / length),
                static_cast<float>(w / length)
            );
            g.sh[0]                  = float4(values[dc[0]], values[dc[1]], values[dc[2]], 0.f);
            const size_t per_channel = rest_count / 3;
            for (size_t i = 0; i < per_channel; ++i) {
                g.sh[i + 1] = float4(
                    values[rest[i]], values[rest[per_channel + i]], values[rest[2 * per_channel + i]], 0.f
                );
            }
            // Conservative rotation-independent 3-sigma support; exact filtering is a rendering decision.
            const float  radius = 3.f * std::max({scale.x, scale.y, scale.z});
            const float3 center(g.position.x, g.position.y, g.position.z);
            const float3 lo = center - float3(radius), hi = center + float3(radius);
            if (!std::isfinite(radius) || !std::isfinite(lo.x) || !std::isfinite(lo.y) ||
                !std::isfinite(lo.z) || !std::isfinite(hi.x) || !std::isfinite(hi.y) || !std::isfinite(hi.z))
                throw std::runtime_error("Gaussian bounds overflow");
            asset->bounds.Expand(lo);
            asset->bounds.Expand(hi);
            asset->gaussians.push_back(g);
        }
        return {std::move(asset), {}};
    } catch (const std::exception& error) {
        return {nullptr, error.what()};
    }
}
} // namespace Moer
