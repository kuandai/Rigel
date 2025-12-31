#pragma once

/**
 * @file Voxel.h
 * @brief Convenience header that includes all voxel system components.
 *
 * Include this single header to access all voxel functionality:
 *
 * @code
 * #include <Rigel/Voxel/Voxel.h>
 *
 * Rigel::Voxel::WorldResources resources;
 * resources.initialize(assets);
 * Rigel::Voxel::World world(resources);
 * @endcode
 */

#include "Block.h"
#include "BlockType.h"
#include "BlockRegistry.h"
#include "BlockLoader.h"
#include "Chunk.h"
#include "ChunkCoord.h"
#include "ChunkManager.h"
#include "ChunkMesh.h"
#include "VoxelVertex.h"
#include "MeshBuilder.h"
#include "TextureAtlas.h"
#include "ChunkRenderer.h"
#include "ChunkBenchmark.h"
#include "ChunkCache.h"
#include "ChunkStreamer.h"
#include "WorldMeshStore.h"
#include "WorldRenderContext.h"
#include "WorldGenConfig.h"
#include "WorldConfigProvider.h"
#include "WorldGenerator.h"
#include "World.h"
#include "WorldId.h"
#include "WorldResources.h"
#include "WorldView.h"
#include "WorldSet.h"
