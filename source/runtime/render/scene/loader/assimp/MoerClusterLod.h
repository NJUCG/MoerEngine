/**
 * MoerClusterLod.h — Moer Engine fork of meshoptimizer's clusterlod.h
 *
 * 基于 upstream clusterlod.h (meshoptimizer v1.1) 的定制版本。
 * 核心改动：集成 meshopt_simplifyWithUpdate，在简化时同步优化顶点位置和属性。
 *
 * 上游来源: 3rdparty/meshoptimizer/demo/clusterlod.h
 * 所有引擎特定改动以 [MOER] 注释标记，方便与上游 diff。
 *
 * Original copyright:
 * Copyright (C) 2016-2026, by Arseny Kapoulkine (arseny.kapoulkine@gmail.com)
 * This code is distributed under the MIT License. See notice at the end of this file.
 */
#pragma once

#include <stddef.h>

struct clodConfig
{
	size_t max_vertices;
	size_t min_triangles;
	size_t max_triangles;

	bool partition_spatial;
	bool partition_sort;
	size_t partition_size;

	bool cluster_spatial;
	float cluster_fill_weight;
	float cluster_split_factor;

	float simplify_ratio;
	float simplify_threshold;

	float simplify_error_merge_previous;
	float simplify_error_merge_additive;

	float simplify_error_factor_sloppy;

	float simplify_error_edge_limit;

	bool simplify_permissive;

	bool simplify_fallback_permissive;
	bool simplify_fallback_sloppy;

	bool simplify_regularize;

	// [MOER] 启用 meshopt_simplifyWithUpdate，在简化时同步优化顶点位置和属性。
	// 需要配合 clodMesh 中的 mutable_vertex_positions / mutable_vertex_attributes 使用。
	bool simplify_update;

	bool optimize_bounds;

	bool optimize_clusters;
	int optimize_clusters_level;
};

struct clodMesh
{
	const unsigned int* indices;
	size_t index_count;

	size_t vertex_count;

	const float* vertex_positions;
	size_t vertex_positions_stride;

	const float* vertex_attributes;
	size_t vertex_attributes_stride;

	const unsigned char* vertex_lock;

	const float* attribute_weights;
	size_t attribute_count;

	unsigned int attribute_protect_mask;

	// [MOER] 可变顶点数据，供 simplifyWithUpdate 就地修改。
	// 由调用方分配并初始化为原始数据的拷贝；clodBuild 内部会通过 simplifyWithUpdate 逐级修改。
	// 为 null 时退化为上游行为（仅使用 simplifyWithAttributes）。
	float* mutable_vertex_positions;
	float* mutable_vertex_attributes;
};

struct clodBounds
{
	float center[3];
	float radius;
	float error;
};

struct clodCluster
{
	int refined;
	clodBounds bounds;
	const unsigned int* indices;
	size_t index_count;
	size_t vertex_count;
};

struct clodGroup
{
	int depth;
	clodBounds simplified;
};

typedef int (*clodOutput)(void* output_context, clodGroup group, const clodCluster* clusters, size_t cluster_count);

#ifdef __cplusplus
extern "C"
{
#endif

clodConfig clodDefaultConfig(size_t max_triangles);
clodConfig clodDefaultConfigRT(size_t max_triangles);

size_t clodBuild(clodConfig config, clodMesh mesh, void* output_context, clodOutput output_callback);

size_t clodLocalIndices(unsigned int* vertices, unsigned char* triangles, const unsigned int* indices, size_t index_count);

#ifdef __cplusplus
} // extern "C"

template <typename Output>
size_t clodBuild(clodConfig config, clodMesh mesh, Output output)
{
	struct Call
	{
		static int output(void* output_context, clodGroup group, const clodCluster* clusters, size_t cluster_count)
		{
			return (*static_cast<Output*>(output_context))(group, clusters, cluster_count);
		}
	};

	return clodBuild(config, mesh, &output, &Call::output);
}
#endif

#ifdef MOER_CLUSTERLOD_IMPLEMENTATION
#include <float.h>
#include <math.h>
#include <string.h>

#include <algorithm>
#include <vector>

namespace moer_clod
{

struct Cluster
{
	size_t vertices;
	std::vector<unsigned int> indices;

	int group;
	int refined;

	clodBounds bounds;
};

static clodBounds boundsCompute(const clodMesh& mesh, const std::vector<unsigned int>& indices, float error)
{
	meshopt_Bounds bounds = meshopt_computeClusterBounds(&indices[0], indices.size(), mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride);

	clodBounds result;
	result.center[0] = bounds.center[0];
	result.center[1] = bounds.center[1];
	result.center[2] = bounds.center[2];
	result.radius = bounds.radius;
	result.error = error;
	return result;
}

static clodBounds boundsMerge(const std::vector<Cluster>& clusters, const std::vector<int>& group)
{
	std::vector<clodBounds> bounds(group.size());
	for (size_t j = 0; j < group.size(); ++j)
		bounds[j] = clusters[group[j]].bounds;

	meshopt_Bounds merged = meshopt_computeSphereBounds(&bounds[0].center[0], bounds.size(), sizeof(clodBounds), &bounds[0].radius, sizeof(clodBounds));

	clodBounds result = {};
	result.center[0] = merged.center[0];
	result.center[1] = merged.center[1];
	result.center[2] = merged.center[2];
	result.radius = merged.radius;

	result.error = 0.f;
	for (size_t j = 0; j < group.size(); ++j)
		result.error = std::max(result.error, clusters[group[j]].bounds.error);

	return result;
}

static std::vector<Cluster> clusterize(const clodConfig& config, const clodMesh& mesh, const unsigned int* indices, size_t index_count)
{
	size_t max_meshlets = meshopt_buildMeshletsBound(index_count, config.max_vertices, config.min_triangles);

	std::vector<meshopt_Meshlet> meshlets(max_meshlets);
	std::vector<unsigned int> meshlet_vertices(index_count);
	std::vector<unsigned char> meshlet_triangles(index_count);

	if (config.cluster_spatial)
		meshlets.resize(meshopt_buildMeshletsSpatial(meshlets.data(), meshlet_vertices.data(), meshlet_triangles.data(), indices, index_count,
		    mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride,
		    config.max_vertices, config.min_triangles, config.max_triangles, config.cluster_fill_weight));
	else
		meshlets.resize(meshopt_buildMeshletsFlex(meshlets.data(), meshlet_vertices.data(), meshlet_triangles.data(), indices, index_count,
		    mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride,
		    config.max_vertices, config.min_triangles, config.max_triangles, 0.f, config.cluster_split_factor));

	std::vector<Cluster> clusters(meshlets.size());

	for (size_t i = 0; i < meshlets.size(); ++i)
	{
		const meshopt_Meshlet& meshlet = meshlets[i];

		if (config.optimize_clusters)
			meshopt_optimizeMeshletLevel(&meshlet_vertices[meshlet.vertex_offset], meshlet.vertex_count, &meshlet_triangles[meshlet.triangle_offset], meshlet.triangle_count, config.optimize_clusters_level);

		clusters[i].vertices = meshlet.vertex_count;

		clusters[i].indices.resize(meshlet.triangle_count * 3);
		for (size_t j = 0; j < meshlet.triangle_count * 3; ++j)
			clusters[i].indices[j] = meshlet_vertices[meshlet.vertex_offset + meshlet_triangles[meshlet.triangle_offset + j]];

		clusters[i].group = -1;
		clusters[i].refined = -1;
	}

	return clusters;
}

static std::vector<std::vector<int> > partition(const clodConfig& config, const clodMesh& mesh, const std::vector<Cluster>& clusters, const std::vector<int>& pending, const std::vector<unsigned int>& remap)
{
	if (pending.size() <= config.partition_size)
		return {pending};

	std::vector<unsigned int> cluster_indices;
	std::vector<unsigned int> cluster_counts(pending.size());

	size_t total_index_count = 0;
	for (size_t i = 0; i < pending.size(); ++i)
		total_index_count += clusters[pending[i]].indices.size();

	cluster_indices.reserve(total_index_count);

	for (size_t i = 0; i < pending.size(); ++i)
	{
		const Cluster& cluster = clusters[pending[i]];

		cluster_counts[i] = unsigned(cluster.indices.size());

		for (size_t j = 0; j < cluster.indices.size(); ++j)
			cluster_indices.push_back(remap[cluster.indices[j]]);
	}

	std::vector<unsigned int> cluster_part(pending.size());
	size_t partition_count = meshopt_partitionClusters(&cluster_part[0], &cluster_indices[0], cluster_indices.size(), &cluster_counts[0], cluster_counts.size(),
	    config.partition_spatial ? mesh.vertex_positions : NULL, remap.size(), mesh.vertex_positions_stride, config.partition_size);

	std::vector<std::vector<int> > partitions(partition_count);
	for (size_t i = 0; i < partition_count; ++i)
		partitions[i].reserve(config.partition_size + config.partition_size / 3);

	std::vector<unsigned int> partition_remap;

	if (config.partition_sort)
	{
		std::vector<float> partition_point(partition_count * 3);
		for (size_t i = 0; i < pending.size(); ++i)
			memcpy(&partition_point[cluster_part[i] * 3], clusters[pending[i]].bounds.center, sizeof(float) * 3);

		partition_remap.resize(partition_count);
		meshopt_spatialSortRemap(partition_remap.data(), partition_point.data(), partition_count, sizeof(float) * 3);
	}

	for (size_t i = 0; i < pending.size(); ++i)
		partitions[partition_remap.empty() ? cluster_part[i] : partition_remap[cluster_part[i]]].push_back(pending[i]);

	return partitions;
}

static void lockBoundary(std::vector<unsigned char>& locks, const std::vector<std::vector<int> >& groups, const std::vector<Cluster>& clusters, const std::vector<unsigned int>& remap, const unsigned char* vertex_lock)
{
	for (size_t i = 0; i < locks.size(); ++i)
		locks[i] &= ~((1 << 0) | (1 << 7));

	for (size_t i = 0; i < groups.size(); ++i)
	{
		for (size_t j = 0; j < groups[i].size(); ++j)
		{
			const Cluster& cluster = clusters[groups[i][j]];
			for (size_t k = 0; k < cluster.indices.size(); ++k)
			{
				unsigned int v = cluster.indices[k];
				unsigned int r = remap[v];
				locks[r] |= locks[r] >> 7;
			}
		}

		for (size_t j = 0; j < groups[i].size(); ++j)
		{
			const Cluster& cluster = clusters[groups[i][j]];
			for (size_t k = 0; k < cluster.indices.size(); ++k)
			{
				unsigned int v = cluster.indices[k];
				unsigned int r = remap[v];
				locks[r] |= 1 << 7;
			}
		}
	}

	for (size_t i = 0; i < locks.size(); ++i)
	{
		unsigned int r = remap[i];
		locks[i] = (locks[r] & 1) | (locks[i] & meshopt_SimplifyVertex_Protect);
		if (vertex_lock)
			locks[i] |= vertex_lock[i];
	}
}

struct SloppyVertex
{
	float x, y, z;
	unsigned int id;
};

static void simplifyFallback(std::vector<unsigned int>& lod, const clodMesh& mesh, const std::vector<unsigned int>& indices, const std::vector<unsigned char>& locks, size_t target_count, float* error)
{
	std::vector<SloppyVertex> subset(indices.size());
	std::vector<unsigned char> subset_locks(indices.size());

	lod.resize(indices.size());

	size_t positions_stride = mesh.vertex_positions_stride / sizeof(float);

	for (size_t i = 0; i < indices.size(); ++i)
	{
		unsigned int v = indices[i];
		assert(v < mesh.vertex_count);

		subset[i].x = mesh.vertex_positions[v * positions_stride + 0];
		subset[i].y = mesh.vertex_positions[v * positions_stride + 1];
		subset[i].z = mesh.vertex_positions[v * positions_stride + 2];
		subset[i].id = v;

		subset_locks[i] = locks[v];
		lod[i] = unsigned(i);
	}

	lod.resize(meshopt_simplifySloppy(&lod[0], &lod[0], lod.size(), &subset[0].x, subset.size(), sizeof(SloppyVertex), subset_locks.data(), target_count, FLT_MAX, error));

	*error *= meshopt_simplifyScale(&subset[0].x, subset.size(), sizeof(SloppyVertex));

	for (size_t i = 0; i < lod.size(); ++i)
		lod[i] = subset[lod[i]].id;
}

// [MOER] 增加 mutable_positions / mutable_attributes 参数，支持 simplifyWithUpdate
static std::vector<unsigned int> simplify(const clodConfig& config, const clodMesh& mesh,
    float* mutable_positions, float* mutable_attributes,
    const std::vector<unsigned int>& indices, const std::vector<unsigned char>& locks, size_t target_count, float* error)
{
	if (target_count > indices.size())
		return indices;

	unsigned int options = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute | (config.simplify_permissive ? meshopt_SimplifyPermissive : 0) | (config.simplify_regularize ? meshopt_SimplifyRegularize : 0);

	std::vector<unsigned int> lod;

	// [MOER] 使用 simplifyWithUpdate 时：就地修改 indices + 更新顶点位置/属性
	if (config.simplify_update && mutable_positions)
	{
		lod.assign(indices.begin(), indices.end());
		lod.resize(meshopt_simplifyWithUpdate(&lod[0], lod.size(),
		    mutable_positions, mesh.vertex_count, mesh.vertex_positions_stride,
		    mutable_attributes, mesh.vertex_attributes_stride, mesh.attribute_weights, mesh.attribute_count,
		    &locks[0], target_count, FLT_MAX, options, error));

		if (lod.size() > target_count && config.simplify_fallback_permissive && !config.simplify_permissive)
		{
			lod.assign(indices.begin(), indices.end());
			lod.resize(meshopt_simplifyWithUpdate(&lod[0], lod.size(),
			    mutable_positions, mesh.vertex_count, mesh.vertex_positions_stride,
			    mutable_attributes, mesh.vertex_attributes_stride, mesh.attribute_weights, mesh.attribute_count,
			    &locks[0], target_count, FLT_MAX, options | meshopt_SimplifyPermissive, error));
		}
	}
	else
	{
		lod.resize(indices.size());
		lod.resize(meshopt_simplifyWithAttributes(&lod[0], &indices[0], indices.size(),
		    mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride,
		    mesh.vertex_attributes, mesh.vertex_attributes_stride, mesh.attribute_weights, mesh.attribute_count,
		    &locks[0], target_count, FLT_MAX, options, error));

		if (lod.size() > target_count && config.simplify_fallback_permissive && !config.simplify_permissive)
			lod.resize(meshopt_simplifyWithAttributes(&lod[0], &indices[0], indices.size(),
			    mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride,
			    mesh.vertex_attributes, mesh.vertex_attributes_stride, mesh.attribute_weights, mesh.attribute_count,
			    &locks[0], target_count, FLT_MAX, options | meshopt_SimplifyPermissive, error));
	}

	if (lod.size() > target_count && config.simplify_fallback_sloppy)
	{
		simplifyFallback(lod, mesh, indices, locks, target_count, error);
		*error *= config.simplify_error_factor_sloppy;
	}

	if (config.simplify_error_edge_limit > 0)
	{
		float max_edge_sq = 0;

		for (size_t i = 0; i < indices.size(); i += 3)
		{
			unsigned int a = indices[i + 0], b = indices[i + 1], c = indices[i + 2];
			assert(a < mesh.vertex_count && b < mesh.vertex_count && c < mesh.vertex_count);

			const float* va = &mesh.vertex_positions[a * (mesh.vertex_positions_stride / sizeof(float))];
			const float* vb = &mesh.vertex_positions[b * (mesh.vertex_positions_stride / sizeof(float))];
			const float* vc = &mesh.vertex_positions[c * (mesh.vertex_positions_stride / sizeof(float))];

			float eab = (va[0] - vb[0]) * (va[0] - vb[0]) + (va[1] - vb[1]) * (va[1] - vb[1]) + (va[2] - vb[2]) * (va[2] - vb[2]);
			float eac = (va[0] - vc[0]) * (va[0] - vc[0]) + (va[1] - vc[1]) * (va[1] - vc[1]) + (va[2] - vc[2]) * (va[2] - vc[2]);
			float ebc = (vb[0] - vc[0]) * (vb[0] - vc[0]) + (vb[1] - vc[1]) * (vb[1] - vc[1]) + (vb[2] - vc[2]) * (vb[2] - vc[2]);

			float emax = std::max(std::max(eab, eac), ebc);
			float emin = std::min(std::min(eab, eac), ebc);

			max_edge_sq = std::max(max_edge_sq, std::max(emin, emax / 4));
		}

		*error = std::min(*error, sqrtf(max_edge_sq) * config.simplify_error_edge_limit);
	}

	return lod;
}

static int outputGroup(const clodConfig& config, const clodMesh& mesh, const std::vector<Cluster>& clusters, const std::vector<int>& group, const clodBounds& simplified, int depth, void* output_context, clodOutput output_callback)
{
	std::vector<clodCluster> group_clusters(group.size());

	for (size_t i = 0; i < group.size(); ++i)
	{
		const Cluster& cluster = clusters[group[i]];
		clodCluster& result = group_clusters[i];

		result.refined = cluster.refined;
		result.bounds = (config.optimize_bounds && cluster.refined != -1) ? boundsCompute(mesh, cluster.indices, cluster.bounds.error) : cluster.bounds;
		result.indices = cluster.indices.data();
		result.index_count = cluster.indices.size();
		result.vertex_count = cluster.vertices;
	}

	return output_callback ? output_callback(output_context, {depth, simplified}, group_clusters.data(), group_clusters.size()) : -1;
}

} // namespace moer_clod

clodConfig clodDefaultConfig(size_t max_triangles)
{
	assert(max_triangles >= 4 && max_triangles <= 256);

	clodConfig config = {};
	config.max_vertices = max_triangles;
	config.min_triangles = max_triangles / 3;
	config.max_triangles = max_triangles;

	config.partition_spatial = true;
	config.partition_size = 16;

	config.cluster_spatial = false;
	config.cluster_split_factor = 2.0f;

	config.optimize_clusters = true;
	config.optimize_clusters_level = 1;

	config.simplify_ratio = 0.5f;
	config.simplify_threshold = 0.85f;
	config.simplify_error_merge_previous = 1.0f;
	config.simplify_error_factor_sloppy = 2.0f;
	config.simplify_permissive = false;
	config.simplify_fallback_permissive = true;
	config.simplify_fallback_sloppy = true;

	// [MOER] 默认启用顶点更新
	config.simplify_update = true;

	return config;
}

clodConfig clodDefaultConfigRT(size_t max_triangles)
{
	clodConfig config = clodDefaultConfig(max_triangles);

	config.min_triangles = max_triangles / 4;
	config.max_vertices = std::min(size_t(256), max_triangles * 2);

	config.cluster_spatial = true;
	config.cluster_fill_weight = 0.5f;

	return config;
}

size_t clodBuild(clodConfig config, clodMesh mesh, void* output_context, clodOutput output_callback)
{
	using namespace moer_clod;

	assert(mesh.vertex_attributes_stride % sizeof(float) == 0);
	assert(mesh.attribute_count * sizeof(float) <= mesh.vertex_attributes_stride);
	assert(mesh.attribute_protect_mask < (1u << (mesh.vertex_attributes_stride / sizeof(float))));

	// [MOER] 当启用 simplify_update 时，内部使用可变数据副本进行简化和后续操作。
	// 所有读取操作（bounds、partition 等）也使用可变数据，以保持位置一致性。
	float* mutable_positions = mesh.mutable_vertex_positions;
	float* mutable_attributes = mesh.mutable_vertex_attributes;

	clodMesh working_mesh = mesh;
	if (config.simplify_update && mutable_positions)
	{
		working_mesh.vertex_positions = mutable_positions;
		if (mutable_attributes)
			working_mesh.vertex_attributes = mutable_attributes;
	}

	std::vector<unsigned char> locks(mesh.vertex_count);

	std::vector<unsigned int> remap(mesh.vertex_count);
	meshopt_generatePositionRemap(&remap[0], working_mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride);

	if (mesh.attribute_protect_mask)
	{
		size_t max_attributes = mesh.vertex_attributes_stride / sizeof(float);

		for (size_t i = 0; i < mesh.vertex_count; ++i)
		{
			unsigned int r = remap[i];

			for (size_t j = 0; j < max_attributes; ++j)
				if (r != i && (mesh.attribute_protect_mask & (1u << j)) && working_mesh.vertex_attributes[i * max_attributes + j] != working_mesh.vertex_attributes[r * max_attributes + j])
					locks[i] |= meshopt_SimplifyVertex_Protect;
		}
	}

	std::vector<Cluster> clusters = clusterize(config, working_mesh, mesh.indices, mesh.index_count);

	for (Cluster& cluster : clusters)
		cluster.bounds = boundsCompute(working_mesh, cluster.indices, 0.f);

	std::vector<int> pending(clusters.size());
	for (size_t i = 0; i < clusters.size(); ++i)
		pending[i] = int(i);

	int depth = 0;

	while (pending.size() > 1)
	{
		std::vector<std::vector<int> > groups = partition(config, working_mesh, clusters, pending, remap);

		pending.clear();

		lockBoundary(locks, groups, clusters, remap, mesh.vertex_lock);

		for (size_t i = 0; i < groups.size(); ++i)
		{
			std::vector<unsigned int> merged;
			merged.reserve(groups[i].size() * config.max_triangles * 3);
			for (size_t j = 0; j < groups[i].size(); ++j)
				merged.insert(merged.end(), clusters[groups[i][j]].indices.begin(), clusters[groups[i][j]].indices.end());

			size_t target_size = size_t((merged.size() / 3) * config.simplify_ratio) * 3;

			clodBounds bounds = boundsMerge(clusters, groups[i]);

			float error = 0.f;
			// [MOER] 传入可变数据指针
			std::vector<unsigned int> simplified = simplify(config, working_mesh, mutable_positions, mutable_attributes, merged, locks, target_size, &error);
			if (simplified.size() > merged.size() * config.simplify_threshold)
			{
				bounds.error = FLT_MAX;
				outputGroup(config, working_mesh, clusters, groups[i], bounds, depth, output_context, output_callback);
				continue;
			}

			bounds.error = std::max(bounds.error * config.simplify_error_merge_previous, error) + error * config.simplify_error_merge_additive;

			int refined = outputGroup(config, working_mesh, clusters, groups[i], bounds, depth, output_context, output_callback);

			for (size_t j = 0; j < groups[i].size(); ++j)
				clusters[groups[i][j]].indices = std::vector<unsigned int>();

			std::vector<Cluster> split = clusterize(config, working_mesh, simplified.data(), simplified.size());

			for (Cluster& cluster : split)
			{
				cluster.refined = refined;
				cluster.bounds = bounds;

				clusters.push_back(std::move(cluster));
				pending.push_back(int(clusters.size()) - 1);
			}
		}

		// [MOER] simplifyWithUpdate 修改了可变数据后，需要刷新 position remap
		// （因为顶点位置可能已变化，影响后续 partition 和 lockBoundary）
		if (config.simplify_update && mutable_positions)
			meshopt_generatePositionRemap(&remap[0], mutable_positions, mesh.vertex_count, mesh.vertex_positions_stride);

		depth++;
	}

	if (pending.size())
	{
		assert(pending.size() == 1);
		const Cluster& cluster = clusters[pending[0]];

		clodBounds bounds = cluster.bounds;
		bounds.error = FLT_MAX;

		outputGroup(config, working_mesh, clusters, pending, bounds, depth, output_context, output_callback);
	}

	return clusters.size();
}

size_t clodLocalIndices(unsigned int* vertices, unsigned char* triangles, const unsigned int* indices, size_t index_count)
{
	return meshopt_extractMeshletIndices(vertices, triangles, indices, index_count);
}
#endif

/**
 * Copyright (c) 2016-2026 Arseny Kapoulkine
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
