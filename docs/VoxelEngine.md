# Voxel Engine Overview

This document outlines the current voxel engine architecture within Rigel. Sections marked "Planned" describe intended future work and are not implemented yet.

---

## Table of Contents

1. [Design Goals](#1-design-goals)
2. [Core Architecture](#2-core-architecture)
3. [Block Type System](#3-block-type-system)
4. [Chunk Management](#4-chunk-management)
5. [Mesh Generation](#5-mesh-generation)
6. [Texture System](#6-texture-system)
7. [Custom Models & Non-Axis Aligned Faces](#7-custom-models--non-axis-aligned-faces)
8. [Rendering Pipeline](#8-rendering-pipeline)
9. [Performance Optimizations](#9-performance-optimizations)
10. [Extension Points](#10-extension-points)

---

## 1. Design Goals

| Priority | Goal |
|----------|------|
| **Primary** | Extensibility for custom block behaviors and geometries |
| **Primary** | Support for animated textures with minimal GPU overhead |
| **Primary** | Non-axis-aligned faces (slopes, stairs, rotated blocks) |
| **Secondary** | Competitive performance with modern voxel engines |
| **Secondary** | Memory efficiency for large worlds |
| **Tertiary** | Mod/plugin-friendly architecture |

### Non-Goals

- Physics simulation (delegated to external systems)
- Multiplayer synchronization (networking layer separate)

---

## 2. Core Architecture

### 2.1 System Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                           WorldSet                              │
│  ┌────────────────┐   ┌──────────────────────────────────────┐  │
│  │ WorldResources │   │ WorldEntry (WorldId -> World/View)    │  │
│  │ BlockRegistry  │   │  ┌──────────────┐  ┌───────────────┐  │  │
│  │ TextureAtlas   │   │  │    World     │  │   WorldView   │  │  │
│  └────────────────┘   │  │ ChunkManager│  │ WorldMeshStore│  │  │
│                       │  │ Generator   │  │ ChunkStreamer │  │  │
│                       │  └──────────────┘  │ ChunkRenderer │  │  │
│                       └────────────────────┴───────────────┘  │  │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 World Ownership (Current)

- `WorldSet` owns shared `WorldResources` and houses multiple `World` entries.
- `WorldResources` contains the shared `BlockRegistry` and `TextureAtlas`.
- `World` owns authoritative chunk data, entity data, and the active `WorldGenerator`.
- `WorldView` owns the streaming, meshing, and render state for a `World`.
- `SvoLodManager` in `WorldView` currently builds revisioned CPU LOD cell
  hierarchies, uploads node payload buffers, and feeds a preliminary far opaque
  LOD proxy pass.
- `ChunkRenderer` applies near/far transition bands with hysteresis:
  near chunk meshes are retained in a near band, while far SVO proxies are
  activated beyond the configured LOD start radius.

World data and render state are deliberately split: `World` stores CPU-side data,
while `WorldView` owns GPU resources like meshes and shaders.

### 2.3 Namespace Organization

```cpp
namespace Rigel::Voxel {
    class World;
    class WorldView;
    class WorldSet;
    class WorldResources;
    class Chunk;
    class ChunkManager;
    class ChunkStreamer;
    class MeshBuilder;
    class ChunkRenderer;
    class WorldMeshStore;
    using WorldId = uint32_t;
    class TextureAtlas;
    class BlockRegistry;
    struct BlockType;
}
```

---

## 3. Block Type System

### 3.1 Block Representation

Blocks are stored as 16-bit identifiers with optional metadata. This balances memory efficiency against flexibility.

```cpp
struct BlockID {
    uint16_t type;      // Block type index (0 = air, 65535 types max)
};

struct BlockState {
    BlockID id;
    uint8_t metadata;   // Rotation, variant, connection state, etc.
    uint8_t lightLevel; // Packed: 4 bits sky light, 4 bits block light
};
```

**Memory Layout Options:**

| Storage Mode | Per-Block Size | Use Case |
|--------------|----------------|----------|
| `BlockID` only | 2 bytes | Simple worlds, maximum density |
| `BlockState` | 4 bytes | Full-featured with lighting |
| Palette + indices | Variable | Sparse chunks with few unique types |

### 3.2 BlockType Definition

Each block type defines its visual and behavioral properties through a data-driven structure:

```cpp
struct BlockType {
    std::string identifier;        // "rigel:stone", "mymod:glowing_ore"

    // Geometry
    std::string model;             // "cube" (only cube is rendered today)
    bool isOpaque;                 // Occludes faces of neighbors
    bool isSolid;                  // Has collision
    bool cullSameType;             // Cull faces when adjacent to same type

    // Rendering
    FaceTextures textures;         // Per-face texture assignments
    RenderLayer layer;             // Opaque, Transparent, Translucent

    // Lighting
    uint8_t emittedLight;          // 0-15, for light-emitting blocks
    uint8_t lightAttenuation;      // How much light is blocked (0-15)

    // Extensibility
    std::any customData;           // User-defined payload
};

enum class RenderLayer : uint8_t {
    Opaque,       // Rendered first, writes depth
    Cutout,       // Alpha-tested (leaves, tall grass)
    Transparent,  // Alpha-blended, sorted back-to-front
    Emissive      // Additive blending for glow effects
};
```

Note: the current mesh builder only renders blocks with `model == "cube"`.
Other model strings are parsed but skipped during meshing.

### 3.3 Block Registry

```cpp
class BlockRegistry {
public:
    // Registration
    BlockID registerBlock(const std::string& identifier, BlockType type);

    // Lookup
    const BlockType& getType(BlockID id) const;
    std::optional<BlockID> findByIdentifier(const std::string& id) const;

    // Iteration (for tools, debug)
    auto begin() const;
    auto end() const;

private:
    std::vector<BlockType> m_types;
    std::unordered_map<std::string, BlockID> m_identifierMap;
};
```

**Registration Example:**

```cpp
BlockRegistry registry;

// Simple opaque block
registry.registerBlock("rigel:stone", {
    .model = "cube",
    .isOpaque = true,
    .textures = FaceTextures::uniform("textures/stone.png"),
    .layer = RenderLayer::Opaque
});

// Transparent block with per-face textures
registry.registerBlock("rigel:grass", {
    .model = "cube",
    .isOpaque = true,
    .textures = FaceTextures::topBottomSides(
        "textures/grass_top.png",
        "textures/dirt.png",
        "textures/grass_side.png"
    ),
    .layer = RenderLayer::Opaque
});
```

---

## 4. Chunk Management

### 4.1 Chunk Structure

Chunks are fixed-size cubic regions. A 32x32x32 size balances mesh complexity against update granularity.

```cpp
class Chunk {
public:
    static constexpr int SIZE = 32;
    static constexpr int VOLUME = SIZE * SIZE * SIZE;  // 32,768 blocks
    static constexpr int SUBCHUNK_SIZE = SIZE / 2;
    static constexpr int SUBCHUNK_VOLUME = SUBCHUNK_SIZE * SUBCHUNK_SIZE * SUBCHUNK_SIZE;
    static constexpr int SUBCHUNK_COUNT = 8;           // 2x2x2

    // Block access
    BlockState getBlock(int x, int y, int z) const;
    void setBlock(int x, int y, int z, BlockState state);

    // Bulk operations
    void fill(BlockState state);
    void copyFrom(std::span<const BlockState> data);

    // State tracking
    bool isDirty() const;           // Needs mesh rebuild
    bool isEmpty() const;           // All air, skip rendering
    bool isFullyOpaque() const;     // Optimization for occlusion
    bool isPersistDirty() const;    // Needs persistence write
    uint32_t meshRevision() const;  // Mesh revision for stale work
    uint32_t worldGenVersion() const; // World generator version tag

    // Serialization
    std::vector<uint8_t> serialize() const;
    static Chunk deserialize(std::span<const uint8_t> data);

private:
    struct Subchunk {
        std::unique_ptr<std::array<BlockState, SUBCHUNK_VOLUME>> blocks;
        uint32_t nonAirCount = 0;
        uint32_t opaqueCount = 0;
    };

    ChunkCoord m_position;
    std::array<Subchunk, SUBCHUNK_COUNT> m_subchunks;

    // Cached state
    bool m_dirty = true;
    bool m_persistDirty = false;
    uint32_t m_nonAirCount = 0;
    uint32_t m_opaqueCount = 0;
    uint32_t m_meshRevision = 0;
    uint32_t m_worldGenVersion = 0;
};
```

Chunks allocate 2x2x2 subchunks on demand, so empty space does not reserve a full
128 KB block array. Each chunk tracks mesh dirtiness, persistence dirtiness, and
the world-generation version used to create it.

### 4.2 Chunk Coordinate System

```cpp
// Direction enum for face indexing
enum class Direction : uint8_t {
    PosX = 0,  // East  (+X)
    NegX = 1,  // West  (-X)
    PosY = 2,  // Up    (+Y)
    NegY = 3,  // Down  (-Y)
    PosZ = 4,  // South (+Z)
    NegZ = 5   // North (-Z)
};

struct ChunkCoord {
    int32_t x, y, z;

    bool operator==(const ChunkCoord&) const = default;
    auto operator<=>(const ChunkCoord&) const = default;

    // Convert to world-space center point
    glm::vec3 toWorldCenter() const {
        constexpr float halfSize = Chunk::SIZE / 2.0f;
        return glm::vec3(
            x * Chunk::SIZE + halfSize,
            y * Chunk::SIZE + halfSize,
            z * Chunk::SIZE + halfSize
        );
    }
};

struct ChunkCoordHash {
    std::size_t operator()(const ChunkCoord& c) const {
        // Interleaved hashing for spatial coherence
        return std::hash<int64_t>{}(
            (static_cast<int64_t>(c.x) * 73856093) ^
            (static_cast<int64_t>(c.y) * 19349663) ^
            (static_cast<int64_t>(c.z) * 83492791)
        );
    }
};
```

### 4.3 ChunkManager

```cpp
class ChunkManager {
public:
    // Chunk access
    Chunk* getChunk(ChunkCoord coord);
    const Chunk* getChunk(ChunkCoord coord) const;
    Chunk& getOrCreateChunk(ChunkCoord coord);

    // Block access (world coordinates)
    BlockState getBlock(int wx, int wy, int wz) const;
    void setBlock(int wx, int wy, int wz, BlockState state);

    // Lifecycle
    void loadChunk(ChunkCoord coord, std::span<const uint8_t> data);
    void unloadChunk(ChunkCoord coord);

    // Dirty tracking
    std::vector<ChunkCoord> getDirtyChunks() const;
    void clearDirtyFlags();

    // Opacity-aware block updates
    void setRegistry(const BlockRegistry* registry);

    // Iteration
    void forEachChunk(std::function<void(ChunkCoord, Chunk&)> fn);

private:
    std::unordered_map<ChunkCoord, std::unique_ptr<Chunk>, ChunkCoordHash> m_chunks;
    const BlockRegistry* m_registry = nullptr;
};
```

Dirty tracking currently scans loaded chunks and collects those with the dirty flag set.

### 4.4 Streaming Overview (Current)

`ChunkStreamer` lives in `WorldView` and drives async generation and meshing.
It builds a spherical desired set around the camera chunk, schedules background
generation/meshing work, and unloads chunks outside the configured distance.

See `docs/WorldGeneration.md` for the full streaming state machine and queue behavior.

---

## 5. Mesh Generation

### 5.1 Vertex Format

```cpp
struct VoxelVertex {
    // Position (12 bytes)
    float x, y, z;

    // Texture coordinates (8 bytes)
    float u, v;

    // Packed data (4 bytes)
    uint8_t normalIndex;      // 0-5 for axis-aligned directions
    uint8_t aoLevel;          // Ambient occlusion: 0-3
    uint8_t textureLayer;     // Array texture layer
    uint8_t flags;            // Reserved for future use

    // Total: 24 bytes per vertex
};
```

### 5.2 Face Culling Strategy

Faces are culled when adjacent to opaque blocks. The mesh builder queries neighbors during generation:

```cpp
class MeshBuilder {
public:
    struct BuildContext {
        const Chunk& chunk;
        const BlockRegistry& registry;
        const TextureAtlas& atlas;

        // Neighbor access for face culling
        std::array<const Chunk*, 6> neighbors;  // +X, -X, +Y, -Y, +Z, -Z
    };

    ChunkMesh build(const BuildContext& ctx);

private:
    bool shouldRenderFace(const BuildContext& ctx,
                          int x, int y, int z,
                          Direction face) const;

    void appendCubeFaces(const BuildContext& ctx,
                         int x, int y, int z,
                         const BlockType& type,
                         std::vector<VoxelVertex>& vertices,
                         std::vector<uint32_t>& indices);
};
```

### 5.3 Greedy Meshing (Planned)

Greedy meshing is not implemented yet. The current mesh builder emits per-face
geometry. The following notes describe a planned optimization:

```cpp
struct GreedyMeshConfig {
    bool enabled = true;
    bool mergeAcrossMetadata = false;  // Merge blocks with different rotation?
    int maxMergeSize = 16;             // Limit merged quad size
};

// Greedy meshing reduces a flat wall of 32x32 blocks from:
//   - Naive: 32*32*4 = 4096 vertices per face
//   - Greedy: 4 vertices (single quad)
```

### 5.4 Mesh Output

```cpp
struct ChunkMesh {
    // Geometry (CPU-only)
    std::vector<VoxelVertex> vertices;
    std::vector<uint32_t> indices;

    // Render layer separation
    struct LayerRange {
        uint32_t indexStart;
        uint32_t indexCount;
    };
    std::array<LayerRange, 4> layers;  // Opaque, Cutout, Transparent, Emissive
};
```

GPU resources are managed by `ChunkRenderer` and keyed by mesh revision in the
`WorldMeshStore` owned by `WorldView`. `ChunkMesh` is treated as immutable once
published.

---

## 6. Texture System

### 6.1 Texture Atlas

All block textures are packed into array textures to minimize bind calls:

```cpp
class TextureAtlas {
public:
    struct Config {
        int tileSize = 16;           // Pixels per texture tile
        int maxLayers = 256;         // Array texture depth
        bool generateMipmaps = true;
    };

    // Registration
    TextureHandle addTexture(const std::string& path);

    // Lookup
    TextureCoords getUVs(TextureHandle handle) const;
    int getLayer(TextureHandle handle) const;

    // GPU
    void upload();
    void bind(GLuint textureUnit = 0);

private:
    GLuint m_textureArray = 0;
    std::vector<TextureEntry> m_entries;
    RectanglePacker m_packer;
};

struct TextureCoords {
    float u0, v0;  // Top-left
    float u1, v1;  // Bottom-right
    int layer;     // Array texture layer
};
```

### 6.2 Animated Textures (planned)

Animated textures use texture array layers for each frame. The shader interpolates based on time:

```cpp
struct AnimationConfig {
    int frameCount;
    float frameDuration;         // Seconds per frame
    bool interpolate = false;    // Smooth frame blending
    AnimationMode mode = AnimationMode::Loop;
};

enum class AnimationMode {
    Loop,        // 0, 1, 2, 0, 1, 2, ...
    PingPong,    // 0, 1, 2, 1, 0, 1, 2, ...
    Once         // 0, 1, 2, 2, 2, ... (hold last frame)
};

// Animated textures are not implemented yet. The design below is aspirational
// and will be introduced when a time-based atlas animation system is added.
```

**Shader Integration (planned):**

```glsl
// Vertex shader output
out float v_animationTime;
out flat int v_baseLayer;
out flat int v_frameCount;

// Fragment shader
uniform sampler2DArray u_textureAtlas;
uniform float u_worldTime;

void main() {
    // Calculate current frame
    float frame = mod(u_worldTime / u_frameDuration, float(v_frameCount));
    int frame0 = int(floor(frame));
    int frame1 = (frame0 + 1) % v_frameCount;
    float blend = fract(frame);

    // Sample both frames
    vec4 color0 = texture(u_textureAtlas, vec3(v_uv, v_baseLayer + frame0));
    vec4 color1 = texture(u_textureAtlas, vec3(v_uv, v_baseLayer + frame1));

    // Interpolate
    FragColor = mix(color0, color1, blend);
}
```

### 6.3 Texture Bleeding Prevention

Array textures with proper UV clamping prevent bleeding. Additional measures:

1. **Half-pixel UV inset** at tile boundaries
2. **Padding pixels** between atlas entries (duplicate edge pixels)
3. **Separate array layers** for animated textures (no atlas packing)

```cpp
TextureCoords TextureAtlas::getUVs(TextureHandle handle) const {
    const auto& entry = m_entries[handle.index];

    float halfPixel = 0.5f / m_config.atlasSize;
    return {
        .u0 = entry.u0 + halfPixel,
        .v0 = entry.v0 + halfPixel,
        .u1 = entry.u1 - halfPixel,
        .v1 = entry.v1 - halfPixel,
        .layer = entry.layer
    };
}
```

---

## 7. Custom Models & Non-Axis Aligned Faces (Planned)

This section documents a future design. The current implementation only renders
`model == "cube"`; other model definitions are not used by the mesh builder yet.

Status: Planned. No custom model parsing or non-cubic meshing is implemented.

### 7.1 BlockModel Structure

Custom models define arbitrary geometry per block type:

```cpp
class BlockModel {
public:
    struct ElementFace {
        TextureRef texture;
        glm::vec4 uv;              // Custom UV mapping
        CullFace cullFace;         // Which neighbor occludes this face
        int tintIndex = -1;        // For biome coloring
        float rotation = 0.0f;     // UV rotation in degrees

        // Computed during model loading
        std::array<glm::vec3, 4> vertices;  // Quad corners
        glm::vec3 normal;                    // Face normal for lighting
    };

    struct Element {
        // Bounding box (0-16 coordinate space, like Minecraft)
        glm::vec3 from;
        glm::vec3 to;

        // Per-face definitions (indexed by Direction enum)
        std::array<std::optional<ElementFace>, 6> faces;

        // Rotation
        std::optional<Rotation> rotation;
    };

    struct Rotation {
        glm::vec3 origin;          // Rotation pivot (0-16 space)
        Axis axis;                 // X, Y, or Z
        float angle;               // Degrees: -45, -22.5, 0, 22.5, 45
        bool rescale = false;      // Scale to maintain 1:1:1 proportions
    };

    // Factory methods
    static BlockModel Cube();
    static BlockModel Cross();     // X-shaped (flowers, saplings)
    static BlockModel Slab(bool upper);
    static BlockModel Stairs(Facing facing, bool upsideDown);
    static BlockModel Slope(Facing facing);

    // Custom model loading (uses rapidyaml - JSON is valid YAML 1.2)
    static BlockModel fromJSON(std::span<const char> jsonData);
    static BlockModel fromYAML(std::span<const char> yamlData);

    // Accessors
    std::span<const Element> getElements() const;
    bool isFullCube() const;
    bool occludesFace(Direction dir) const;  // Does this model fully cover the given face?

private:
    std::vector<Element> m_elements;
    std::array<bool, 6> m_fullFaceOcclusion;
};
```

### 7.2 Model Variants

Block states map to model variants through a state-to-model resolver:

```cpp
struct BlockStateModel {
    // State conditions
    std::map<std::string, std::string> conditions;  // e.g., {"facing": "north"}

    // Model reference
    BlockModelRef model;

    // Transform
    int rotationX = 0;   // 0, 90, 180, 270
    int rotationY = 0;
    bool uvlock = false; // Keep UVs fixed during rotation
    int weight = 1;      // For random variant selection
};

class BlockStateResolver {
public:
    void addVariant(BlockID id, BlockStateModel variant);

    const BlockModel& resolve(BlockID id, uint8_t metadata) const;
    glm::mat4 getTransform(BlockID id, uint8_t metadata) const;

private:
    std::unordered_map<BlockID, std::vector<BlockStateModel>> m_variants;
};
```

### 7.3 Non-Axis Aligned Geometry

For slopes, wedges, and rotated elements:

```cpp
// Slopes define triangular faces
struct SlopeElement {
    std::array<glm::vec3, 3> triangle;  // Three vertices
    glm::vec3 normal;                    // For lighting
    TextureRef texture;
    std::array<glm::vec2, 3> uvs;       // Per-vertex UVs
};

// The mesh builder handles rotation transforms
void MeshBuilder::appendRotatedElement(
    const BlockModel::Element& element,
    const glm::mat4& transform,
    std::vector<VoxelVertex>& vertices,
    std::vector<uint32_t>& indices
) {
    for (const auto& face : element.faces) {
        if (!face) continue;

        // Transform vertices
        glm::vec3 v0 = transform * glm::vec4(face.vertices[0], 1.0f);
        glm::vec3 v1 = transform * glm::vec4(face.vertices[1], 1.0f);
        // ... etc

        // Transform normal for lighting
        glm::vec3 normal = glm::normalize(
            glm::mat3(glm::transpose(glm::inverse(transform))) * face.normal
        );

        // Emit vertices with transformed positions
        // ...
    }
}
```

### 7.4 Culling with Custom Models

Face culling for non-cubic blocks requires more sophisticated occlusion testing:

```cpp
enum class CullFace {
    None,    // Never cull (internal faces)
    PosX,    // Cull if neighbor at +X occludes its -X face
    NegX,
    PosY,
    NegY,
    PosZ,
    NegZ,
    Full     // Only cull if neighbor is a full opaque cube
};

bool MeshBuilder::shouldCullFace(
    const BlockModel::ElementFace& face,
    Direction dir,
    const Chunk& chunk,
    int x, int y, int z
) {
    if (face.cullFace == CullFace::None) return false;

    BlockState neighbor = getNeighborBlock(chunk, x, y, z, dir);
    if (neighbor.id.type == 0) return false;  // Air

    const BlockType& neighborType = m_registry.getType(neighbor.id);
    const BlockModel& neighborModel = m_registry.getModel(neighborType.model);

    // Full cube check
    if (face.cullFace == CullFace::Full) {
        return neighborType.isOpaque && neighborModel.isFullCube();
    }

    // Per-face occlusion
    Direction oppositeDir = getOppositeDirection(dir);
    return neighborModel.occludesFace(oppositeDir);
}
```

---

## 8. Rendering Pipeline

Status: Partial. Layered rendering and transparent sorting exist; the shader
snippets and frustum culling notes below are illustrative and not a verbatim
representation of the current implementation.

### 8.1 Render Order

```cpp
class ChunkRenderer {
public:
    void render(const Camera& camera, float worldTime);

private:
    void setupShader(const Camera& camera, float worldTime);

    // Ordered rendering passes
    void renderOpaquePass(const Camera& camera);
    void renderCutoutPass(const Camera& camera);
    void renderTransparentPass(const Camera& camera);  // Sorted back-to-front
    void renderEmissivePass(const Camera& camera);

    // Per-pass state
    void bindOpaqueState();    // Depth write ON, blend OFF
    void bindCutoutState();    // Depth write ON, alpha test ON
    void bindTransparentState(); // Depth write OFF, blend ON
    void bindEmissiveState();  // Depth write OFF, additive blend
};
```

### 8.2 Shader Design

```glsl
// voxel.vert
#version 330 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_packedData;  // normal, ao, layer, anim

uniform mat4 u_viewProjection;
uniform vec3 u_chunkOffset;

out vec2 v_uv;
out float v_ao;
out vec3 v_normal;
flat out int v_textureLayer;
flat out int v_animFrameCount;

const vec3 NORMALS[6] = vec3[](
    vec3( 1,  0,  0),  // +X
    vec3(-1,  0,  0),  // -X
    vec3( 0,  1,  0),  // +Y
    vec3( 0, -1,  0),  // -Y
    vec3( 0,  0,  1),  // +Z
    vec3( 0,  0, -1)   // -Z
);

void main() {
    vec3 worldPos = a_position + u_chunkOffset;
    gl_Position = u_viewProjection * vec4(worldPos, 1.0);

    v_uv = a_uv;
    v_ao = a_packedData.y / 3.0;  // 0-3 -> 0-1
    v_normal = NORMALS[int(a_packedData.x)];
    v_textureLayer = int(a_packedData.z);
    v_animFrameCount = int(a_packedData.w);
}
```

```glsl
// voxel.frag
#version 330 core

in vec2 v_uv;
in float v_ao;
in vec3 v_normal;
flat in int v_textureLayer;
flat in int v_animFrameCount;

uniform sampler2DArray u_textureAtlas;
uniform vec3 u_sunDirection;
uniform float u_worldTime;
uniform float u_frameDuration;

out vec4 FragColor;

void main() {
    // Animated texture sampling
    int layer = v_textureLayer;
    if (v_animFrameCount > 1) {
        float frame = mod(u_worldTime / u_frameDuration, float(v_animFrameCount));
        layer += int(frame);
    }

    vec4 texColor = texture(u_textureAtlas, vec3(v_uv, layer));

    // Simple directional lighting
    float diffuse = max(dot(v_normal, u_sunDirection), 0.0);
    float lighting = 0.3 + 0.7 * diffuse;  // Ambient + diffuse

    // Ambient occlusion
    float ao = 0.5 + 0.5 * v_ao;

    FragColor = vec4(texColor.rgb * lighting * ao, texColor.a);
}
```

### 8.3 Frustum Culling

Status: Planned for voxel chunks. The current renderer only performs distance
culling for voxel meshes. Entity rendering does its own frustum checks.

Chunks outside the view frustum are skipped entirely:

```cpp
class Frustum {
public:
    void update(const glm::mat4& viewProjection);
    bool containsAABB(const AABB& box) const;

private:
    std::array<glm::vec4, 6> m_planes;  // Near, Far, Left, Right, Top, Bottom
};

void ChunkRenderer::collectVisibleChunks(const Camera& camera) {
    m_visibleChunks.clear();

    Frustum frustum;
    frustum.update(camera.getViewProjection());

m_chunkManager.forEachChunk([&](ChunkCoord coord, Chunk& chunk) {
        if (chunk.isEmpty()) return;

        AABB bounds = chunk.getBoundingBox();
        if (frustum.containsAABB(bounds)) {
            m_visibleChunks.push_back(&chunk);
        }
    });
}
```

---

## 9. Performance Optimizations

Status: Partial. Chunk generation/meshing is multithreaded via `ChunkStreamer`;
other optimizations in this section are planned.

### 9.1 Chunk LOD (Level of Detail)

Distant chunks use simplified meshes:

```cpp
enum class LODLevel {
    Full,       // All faces, normal textures
    Reduced,    // Greedy-meshed only, no small details
    Billboard,  // Single textured quad (very far)
    Skip        // Beyond render distance
};

LODLevel ChunkRenderer::calculateLOD(const ChunkCoord& coord,
                                      const glm::vec3& cameraPos) {
    float distance = glm::distance(coord.toWorldCenter(), cameraPos);

    if (distance > m_config.farDistance) return LODLevel::Skip;
    if (distance > m_config.billboardDistance) return LODLevel::Billboard;
    if (distance > m_config.reducedDistance) return LODLevel::Reduced;
    return LODLevel::Full;
}
```

### 9.2 Occlusion Culling

Chunks fully occluded by terrain are skipped:

```cpp
class OcclusionCuller {
public:
    // CPU-side conservative occlusion using chunk opacity
    void buildOcclusionField(const ChunkManager& chunks);
    bool isChunkVisible(ChunkCoord coord, const glm::vec3& cameraPos) const;

    // GPU occlusion queries (optional, higher accuracy)
    void beginOcclusionQuery(ChunkCoord coord);
    bool wasChunkVisible(ChunkCoord coord) const;

private:
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_visibleSet;
    std::unordered_map<ChunkCoord, GLuint, ChunkCoordHash> m_queries;
};
```

### 9.3 Multithreaded Mesh Generation

Mesh building is CPU-intensive and parallelizable:

```cpp
class MeshBuildQueue {
public:
    // Submit chunk for background meshing
    void enqueue(ChunkCoord coord, MeshPriority priority);

    // Retrieve completed meshes (call from main thread)
    std::vector<std::pair<ChunkCoord, ChunkMesh>> fetchCompleted();

    // Prioritize visible chunks
    void reprioritize(const std::vector<ChunkCoord>& visible);

private:
    void workerThread();

    std::priority_queue<MeshJob> m_pending;
    std::vector<std::pair<ChunkCoord, ChunkMesh>> m_completed;
    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_cv;
};
```

### 9.4 Instanced Rendering

For repeated decorations (torches, flowers), use instancing:

```cpp
struct InstanceData {
    glm::vec3 position;
    uint8_t rotation;
    uint8_t variant;
    uint16_t padding;
};

class InstancedModelRenderer {
public:
    void addInstance(BlockModelRef model, const InstanceData& data);
    void flush();  // Draw all instances

private:
    std::unordered_map<BlockModelRef, std::vector<InstanceData>> m_instances;
    GLuint m_instanceVBO;
};
```

### 9.5 Memory Optimizations

**Palette Compression:**

Chunks with few unique block types use index compression:

```cpp
class PalettedChunk {
public:
    BlockState getBlock(int x, int y, int z) const {
        uint16_t index = m_indices[flatIndex(x, y, z)];
        return m_palette[index];
    }

    void setBlock(int x, int y, int z, BlockState state) {
        auto it = std::find(m_palette.begin(), m_palette.end(), state);
        if (it == m_palette.end()) {
            m_palette.push_back(state);
            it = m_palette.end() - 1;
        }
        m_indices[flatIndex(x, y, z)] = std::distance(m_palette.begin(), it);
    }

private:
    std::vector<BlockState> m_palette;  // Typically 1-64 entries
    std::vector<uint16_t> m_indices;    // Or uint8_t if palette < 256
};
```

**Chunk Unloading:**

```cpp
class ChunkCache {
public:
    // LRU cache with configurable size
    void setMaxCachedChunks(size_t count);

    // Serialize before eviction
    void setEvictionCallback(std::function<void(ChunkCoord, Chunk&)> cb);

    // Access (marks as recently used)
    Chunk* get(ChunkCoord coord);

private:
    std::list<std::pair<ChunkCoord, std::unique_ptr<Chunk>>> m_cache;
    std::unordered_map<ChunkCoord, decltype(m_cache)::iterator, ChunkCoordHash> m_lookup;
};
```

### 9.6 Draw Call Batching

Minimize state changes by sorting draw calls:

```cpp
void ChunkRenderer::sortDrawCalls() {
    std::sort(m_drawCalls.begin(), m_drawCalls.end(), [](const auto& a, const auto& b) {
        // Primary: render layer (opaque before transparent)
        if (a.layer != b.layer) return a.layer < b.layer;

        // Secondary: texture array (minimize binds)
        if (a.textureArray != b.textureArray) return a.textureArray < b.textureArray;

        // Tertiary: front-to-back for opaque (early-z optimization)
        return a.distanceToCamera < b.distanceToCamera;
    });
}
```

---

## 10. Extension Points

Status: Planned. Interfaces below are design sketches and not implemented.

### 10.1 Custom Block Behaviors

```cpp
class IBlockBehavior {
public:
    virtual ~IBlockBehavior() = default;

    // Called when block is placed/broken
    virtual void onPlace(World& world, BlockPos pos) {}
    virtual void onBreak(World& world, BlockPos pos) {}

    // Called each tick for blocks with scheduled updates
    virtual void onTick(World& world, BlockPos pos) {}

    // Custom rendering hooks
    virtual void onPreRender(ChunkMesh& mesh, BlockPos pos) {}
    virtual void onPostRender() {}
};

// Registration
registry.registerBlock("mymod:custom_block", {
    .model = "cube",
    .behavior = std::make_shared<MyCustomBehavior>()
});
```

### 10.2 World Generators

```cpp
class IWorldGenerator {
public:
    virtual ~IWorldGenerator() = default;
    virtual void generate(Chunk& chunk, ChunkCoord coord) = 0;
};

class CompositeGenerator : public IWorldGenerator {
public:
    void addPass(std::unique_ptr<IWorldGenerator> pass);
    void generate(Chunk& chunk, ChunkCoord coord) override;

private:
    std::vector<std::unique_ptr<IWorldGenerator>> m_passes;
};
```

### 10.3 Custom Shaders

```cpp
class ShaderRegistry {
public:
    void registerBlockShader(const std::string& id,
                             std::span<const char> vertSource,
                             std::span<const char> fragSource);

    GLuint getProgram(const std::string& id) const;
};

// Blocks can specify custom shaders
registry.registerBlock("mymod:hologram", {
    .model = "cube",
    .shader = "mymod:hologram_shader",  // Uses custom rendering
    .layer = RenderLayer::Transparent
});
```

### 10.4 Event System

```cpp
namespace Rigel::Voxel::Events {
    struct BlockPlaced {
        BlockPos position;
        BlockState newState;
        BlockState oldState;
    };

    struct ChunkLoaded {
        ChunkCoord coord;
        Chunk& chunk;
    };

    struct ChunkMeshed {
        ChunkCoord coord;
        float meshTimeMs;
        size_t vertexCount;
    };
}

class EventBus {
public:
    template<typename Event>
    void subscribe(std::function<void(const Event&)> handler);

    template<typename Event>
    void emit(const Event& event);
};
```

---

## Appendix A: Recommended Implementation Order

Status: Planning guide, not a current roadmap.

1. **Phase 1: Foundation**
   - Block storage and chunk data structures
   - Basic cube mesh generation
   - Single-chunk rendering

2. **Phase 2: World**
   - Multi-chunk management
   - Face culling between chunks
   - Chunk loading/unloading

3. **Phase 3: Textures**
   - Texture atlas generation
   - Per-face textures
   - Animated texture support

4. **Phase 4: Custom Geometry**
   - BlockModel parsing
   - Non-cubic mesh generation
   - Model variants and rotation

5. **Phase 5: Optimization**
   - Greedy meshing
   - Multithreaded mesh building
   - Frustum and occlusion culling

6. **Phase 6: Polish**
   - Lighting system
   - LOD system
   - Memory optimization

---

## Appendix B: Memory Budget Estimates

| Component | Memory (per chunk) | Notes |
|-----------|-------------------|-------|
| Block data | 128 KB | 32K blocks * 4 bytes |
| Mesh vertices | ~50-200 KB | Highly variable by content |
| Mesh indices | ~25-100 KB | ~1.5 indices per vertex avg |
| GPU buffers | Same as CPU | Mirrored for rendering |

**World scale example (256 loaded chunks):**
- Block data: 32 MB
- Meshes: ~50 MB CPU + 50 MB GPU
- Total: ~130 MB for visible world

---

## Appendix C: File Format Recommendations

Status: Proposed formats and parsing examples; not implemented.

All JSON and YAML files are parsed using **rapidyaml**. Since JSON is a valid subset of YAML 1.2, the same parser handles both formats seamlessly.

---

## Related Docs

- `docs/WorldGeneration.md`
- `docs/RenderingPipeline.md`
- `docs/EntitySystem.md`

### Block Registry (JSON)

```json
{
  "rigel:stone": {
    "model": "rigel:block/cube",
    "textures": {
      "all": "rigel:textures/stone.png"
    },
    "opaque": true,
    "solid": true
  }
}
```

### Block Model (JSON)

```json
{
  "elements": [
    {
      "from": [0, 0, 0],
      "to": [16, 8, 16],
      "faces": {
        "up":    {"texture": "#top", "uv": [0, 0, 16, 16]},
        "down":  {"texture": "#bottom", "cullface": "down"},
        "north": {"texture": "#side", "cullface": "north"},
        "south": {"texture": "#side", "cullface": "south"},
        "east":  {"texture": "#side", "cullface": "east"},
        "west":  {"texture": "#side", "cullface": "west"}
      }
    }
  ]
}
```

### Parsing Block Models with rapidyaml

```cpp
#include <ryml.hpp>
#include <ryml_std.hpp>

BlockModel BlockModel::fromJSON(std::span<const char> jsonData) {
    ryml::Tree tree = ryml::parse_in_arena(
        ryml::csubstr(jsonData.data(), jsonData.size())
    );
    ryml::ConstNodeRef root = tree.rootref();

    BlockModel model;

    if (root.has_child("elements")) {
        for (ryml::ConstNodeRef elemNode : root["elements"].children()) {
            Element elem;

            // Parse "from" and "to" arrays
            if (elemNode.has_child("from")) {
                ryml::ConstNodeRef from = elemNode["from"];
                from[0] >> elem.from.x;
                from[1] >> elem.from.y;
                from[2] >> elem.from.z;
            }

            if (elemNode.has_child("to")) {
                ryml::ConstNodeRef to = elemNode["to"];
                to[0] >> elem.to.x;
                to[1] >> elem.to.y;
                to[2] >> elem.to.z;
            }

            // Parse faces
            if (elemNode.has_child("faces")) {
                parseFaces(elemNode["faces"], elem.faces);
            }

            model.m_elements.push_back(std::move(elem));
        }
    }

    model.computeOcclusion();
    return model;
}
```

### Chunk Data (Binary)

```
Header (16 bytes):
  - Magic: "RCHK" (4 bytes)
  - Version: uint16
  - Compression: uint8 (0=none, 1=zstd, 2=lz4)
  - Flags: uint8
  - Block count: uint32
  - Palette size: uint16
  - Reserved: uint16

Palette (variable):
  - Array of BlockState (4 bytes each)

Indices (variable):
  - If palette <= 16: 4-bit indices (16KB)
  - If palette <= 256: 8-bit indices (32KB)
  - Otherwise: 16-bit indices (64KB)
```
