# Asset Loading System Design Document

This document describes a high-level asset loading system for Rigel that provides declarative markup for complex assets, automatic dependency resolution, and runtime asset management. It builds upon the existing `ResourceRegistry` for raw data access.

---

## Table of Contents

1. [Design Goals](#1-design-goals)
2. [Architecture Overview](#2-architecture-overview)
3. [Asset Manifest Format](#3-asset-manifest-format)
4. [Asset Types](#4-asset-types)
5. [Animation System](#5-animation-system)
6. [Dependency Resolution](#6-dependency-resolution)
7. [Runtime Asset Management](#7-runtime-asset-management)
8. [Integration Points](#8-integration-points)
9. [Error Handling](#9-error-handling)
10. [Extension Mechanisms](#10-extension-mechanisms)
11. [Appendix A: Complete Manifest Example](#appendix-a-complete-manifest-example)
12. [Appendix B: YAML Schema Reference](#appendix-b-yaml-schema-reference)
13. [Appendix C: Performance Considerations](#appendix-c-performance-considerations)
14. [Appendix D: rapidyaml Usage Patterns](#appendix-d-rapidyaml-usage-patterns)

---

## 1. Design Goals

| Priority | Goal |
|----------|------|
| **Primary** | Declarative markup for complex asset behavior |
| **Primary** | Automatic dependency resolution and load ordering |
| **Primary** | Type-safe asset handles with compile-time validation |
| **Secondary** | Hot-reloading support for development builds |
| **Secondary** | Async loading with progress tracking |
| **Tertiary** | Mod/pack support with asset overrides |

### Relationship to Existing Systems

```
┌─────────────────────────────────────────────────────────────────┐
│                      Application Code                           │
│                            │                                    │
│                            ▼                                    │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    AssetManager                          │   │
│  │  • Declarative loading    • Dependency resolution        │   │
│  │  • Type-safe handles      • Lifecycle management         │   │
│  └─────────────────────────────────────────────────────────┘   │
│                            │                                    │
│         ┌──────────────────┼──────────────────┐                │
│         ▼                  ▼                  ▼                 │
│  ┌─────────────┐   ┌─────────────┐   ┌───────────────────┐     │
│  │ Resource    │   │  rapidyaml  │   │   AssetLoaders    │     │
│  │ Registry    │   │  (parser)   │   │ (format-specific) │     │
│  └─────────────┘   └─────────────┘   └───────────────────┘     │
└─────────────────────────────────────────────────────────────────┘
```

**Dependencies:**
- `ResourceRegistry` - Provides raw embedded asset bytes (see `docs/assets.md`)
- `rapidyaml` - High-performance YAML parser for manifest files
- `AssetLoaders` - Format-specific decoders (PNG, glTF, OGG, etc.)

---

## 2. Architecture Overview

### 2.1 Core Components

```cpp
namespace Rigel::Asset {
    class AssetManager;      // Central registry and lifecycle manager
    class AssetManifest;     // Parsed manifest file
    class AssetHandle;       // Type-erased reference to loaded asset
    class AssetLoader;       // Base class for format-specific loaders

    template<typename T>
    class Handle;            // Type-safe asset reference
}
```

### 2.2 Asset Lifecycle

```
┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
│ Declared │───▶│ Pending  │───▶│ Loading  │───▶│  Ready   │
└──────────┘    └──────────┘    └──────────┘    └──────────┘
     │                               │                │
     │                               ▼                ▼
     │                          ┌──────────┐    ┌──────────┐
     └─────────────────────────▶│  Failed  │    │ Unloaded │
                                └──────────┘    └──────────┘
```

### 2.3 Namespace Organization

```cpp
namespace Rigel::Asset {
    // Core
    class AssetManager;
    class AssetManifest;

    // Handles
    template<typename T> class Handle;
    class WeakHandle;

    // Loaders
    class IAssetLoader;
    class TextureLoader;
    class ShaderLoader;
    class ModelLoader;
    class AnimationLoader;
    class AudioLoader;
}

namespace Rigel::Asset::Manifest {
    // Parsed manifest structures
    struct TextureAsset;
    struct ShaderAsset;
    struct AnimationAsset;
    struct ModelAsset;
}
```

### 2.4 Internal Asset Entry Structure

Assets are stored internally with their full manifest configuration preserved, enabling complex assets like shaders that require multiple source files or nested properties:

```cpp
struct AssetEntry {
    std::string id;           // Full asset ID (e.g., "shaders/voxel")
    std::string category;     // Category name (e.g., "shaders")
    ryml::Tree configTree;    // Preserved YAML subtree for this asset
    ryml::ConstNodeRef config; // Reference to asset's config node

    // Convenience accessors
    std::optional<std::string> getPath() const;
    std::optional<std::string> getString(const std::string& key) const;
    std::optional<ryml::ConstNodeRef> getNode(const std::string& key) const;

    // For inheritance support
    std::optional<std::string> inherit;  // Parent asset ID if inheriting
    bool resolved = false;               // True after inheritance is resolved
};
```

**Why preserve the full config node:**

| Asset Type | Simple `path` field | Full config needed |
|------------|--------------------|--------------------|
| Textures | `path: texture.png` | filter, wrap, mipmaps |
| Shaders | — | vertex, fragment, geometry, defines map |
| Models | `path: model.gltf` | scale, origin, textures map |
| Animations | — | frames list, timing, events |

For assets like shaders that don't have a single `path`, the loader receives the full config node and extracts what it needs:

```cpp
// Shader manifest entry - no single "path" field
shaders:
  voxel:
    vertex: shaders/voxel.vert      # Multiple source paths
    fragment: shaders/voxel.frag
    defines:                         # Nested map
      MAX_LIGHTS: 8
      ENABLE_AO: true
```

---

## 3. Asset Manifest Format

Assets are declared in YAML manifest files, parsed using rapidyaml for high-performance loading. Each manifest describes assets within a namespace and their relationships.

### 3.0 Manifest Parsing

Manifests are parsed into a `ryml::Tree` for efficient traversal:

```cpp
#include <ryml.hpp>
#include <ryml_std.hpp>

class AssetManifest {
public:
    static AssetManifest parse(std::span<const char> yamlData) {
        AssetManifest manifest;
        // Parse into arena (zero-copy where possible)
        manifest.m_tree = ryml::parse_in_arena(
            ryml::csubstr(yamlData.data(), yamlData.size())
        );
        manifest.m_root = manifest.m_tree.rootref();

        // Extract namespace
        if (manifest.m_root.has_child("namespace")) {
            manifest.m_root["namespace"] >> manifest.m_namespace;
        }

        return manifest;
    }

    const std::string& ns() const { return m_namespace; }
    ryml::ConstNodeRef root() const { return m_root; }
    ryml::ConstNodeRef assets() const { return m_root["assets"]; }

private:
    ryml::Tree m_tree;
    ryml::ConstNodeRef m_root;
    std::string m_namespace;
};
```

### 3.1 Manifest Structure

```yaml
# assets/manifest.yaml
namespace: rigel
#
# Note: AssetManager currently loads a single manifest. Imports are planned
# but not implemented yet.

# Import other manifests (planned, not yet implemented)
# imports:
#   - textures/blocks.yaml
#   - models/entities.yaml

# Inline asset declarations
assets:
  textures:
    stone:
      path: textures/blocks/stone.png
      filter: nearest

  shaders:
    voxel:
      vertex: shaders/voxel.vert
      fragment: shaders/voxel.frag
      defines:
        MAX_LIGHTS: 8
```

### 3.2 Asset Identifier Format

Assets are referenced by identifiers of the form:

```
<category>/<name>
```

**Examples:**
- `textures/stone`
- `shaders/voxel`
- `models/custom_block`

Namespace-qualified IDs are planned, but the current `AssetManager` treats IDs
as `category/name` only and does not resolve `rigel:textures/stone` yet.

### 3.3 Path Resolution

```yaml
# Paths map directly to ResourceRegistry keys (embedded asset paths)
assets:
  textures:
    logo:
      path: textures/logo.png
```

Variable expansion and manifest-relative path resolution are planned but not
implemented yet.

---

## 4. Asset Types

### 4.1 Textures

```yaml
textures:
  # Simple texture
  dirt:
    path: textures/dirt.png

  # Full specification (current loader supports only `filter`)
  grass_top:
    path: textures/grass_top.png
    filter: nearest              # nearest | linear
```

**C++ API:**

```cpp
struct TextureAsset {
    GLuint id;
    int width, height;
    int layers;           // >1 for array textures
    GLenum format;
};

Handle<TextureAsset> tex = assets.get<TextureAsset>("textures/stone");
glBindTexture(GL_TEXTURE_2D, tex->id);
```

### 4.2 Shaders

```yaml
shaders:
  voxel_opaque:
    vertex: shaders/voxel.vert
    fragment: shaders/voxel_opaque.frag
    defines:
      ENABLE_AO: true
      MAX_TEXTURE_LAYERS: 256
    uniforms:
      u_viewProjection: mat4
      u_chunkOffset: vec3
      u_worldTime: float
      u_textureAtlas: sampler2DArray

  voxel_transparent:
    inherit: voxel_opaque        # Inherit base configuration
    fragment: shaders/voxel_transparent.frag
    defines:
      ENABLE_ALPHA_BLEND: true

  # Compute shader
  lighting_propagate:
    compute: shaders/light_propagate.comp
    workgroup_size: [8, 8, 8]
```

**C++ API:**

```cpp
struct ShaderAsset {
    GLuint program;
    std::unordered_map<std::string, GLint> uniformLocations;

    GLint uniform(const std::string& name) const;
};

Handle<ShaderAsset> shader = assets.get<ShaderAsset>("rigel:shaders/voxel");
glUseProgram(shader->program);
glUniformMatrix4fv(shader->uniform("u_viewProjection"), 1, GL_FALSE, &vp[0][0]);
```

### 4.3 Models

```yaml
models:
  # Simple cube (built-in)
  cube:
    type: builtin
    builtin: cube

  # JSON model file (Minecraft-style format)
  stairs:
    path: models/blocks/stairs.json
    textures:
      top: textures/planks
      bottom: textures/planks
      side: textures/planks

  # OBJ/glTF model
  chest:
    path: models/entities/chest.gltf
    scale: 0.0625              # 1/16 scale for block-sized
    origin: [0.5, 0, 0.5]      # Pivot point
```

**C++ API:**

```cpp
struct ModelAsset {
    std::vector<ModelElement> elements;
    AABB boundingBox;

    bool isFullCube() const;
    bool occludesFace(Face face) const;
};
```

### 4.4 Audio

```yaml
audio:
  # Sound effect
  block_place:
    path: sounds/block/place.ogg
    volume: 0.8
    pitch_variance: 0.1        # Random pitch ±10%
    category: blocks

  # Music track
  ambient_cave:
    path: music/cave_ambient.ogg
    stream: true               # Stream from disk, don't load fully
    loop: true
    fade_in: 2.0
    fade_out: 1.5

  # Sound group (random selection)
  footstep_stone:
    type: group
    sounds:
      - sounds/step/stone1.ogg
      - sounds/step/stone2.ogg
      - sounds/step/stone3.ogg
      - sounds/step/stone4.ogg
    volume: 0.5
```

---

## 5. Animation System

The animation system supports multiple animation paradigms through declarative markup.

### 5.1 Texture Animations (Flipbook)

For animated block textures, water, lava, etc.

```yaml
animations:
  water_flow:
    type: flipbook
    texture: textures/water_flow.png
    layout:
      frames: 32               # Number of frames
      direction: vertical      # vertical | horizontal | grid
      columns: 1               # For grid layout
    timing:
      frame_duration: 0.05     # Seconds per frame
      mode: loop               # loop | ping_pong | once | reverse
    interpolation: none        # none | linear | smooth

  fire:
    type: flipbook
    frames:                    # Explicit frame list
      - textures/fire/frame_00.png
      - textures/fire/frame_01.png
      - textures/fire/frame_02.png
    timing:
      durations: [0.1, 0.08, 0.12]  # Per-frame timing
      mode: loop
    interpolation: linear      # Blend between frames
```

**C++ API:**

```cpp
struct FlipbookAnimation {
    Handle<TextureAsset> textureArray;  // Frames as array layers
    std::vector<float> frameTimes;       // Cumulative time per frame
    float totalDuration;
    AnimationMode mode;
    bool interpolate;

    // Returns layer index and blend factor
    std::pair<int, float> sample(float time) const;
};

Handle<FlipbookAnimation> anim = assets.get<FlipbookAnimation>("rigel:animations/water");
auto [frame, blend] = anim->sample(worldTime);
```

### 5.2 Skeletal Animations

For entity models with bone hierarchies.

```yaml
animations:
  player_walk:
    type: skeletal
    skeleton: models/player/skeleton.json
    clip: animations/player/walk.gltf
    duration: 0.8
    loop: true
    events:                    # Trigger points
      - time: 0.2
        event: footstep_left
      - time: 0.6
        event: footstep_right

  player_attack:
    type: skeletal
    skeleton: models/player/skeleton.json
    clip: animations/player/attack.gltf
    duration: 0.5
    loop: false
    blend_in: 0.1              # Transition time from previous
    blend_out: 0.15
    root_motion: true          # Extract movement from animation
```

**Skeleton Definition:**

```yaml
# models/player/skeleton.json (referenced above)
skeleton:
  bones:
    - name: root
      children: [spine]

    - name: spine
      parent: root
      position: [0, 0.5, 0]
      children: [chest, hip_l, hip_r]

    - name: chest
      parent: spine
      position: [0, 0.4, 0]
      children: [head, shoulder_l, shoulder_r]

    # ... etc
```

**C++ API:**

```cpp
struct SkeletalAnimation {
    Handle<SkeletonAsset> skeleton;
    std::vector<BoneKeyframes> channels;
    float duration;
    bool loops;

    // Sample bone transforms at time t
    void sample(float time, std::span<glm::mat4> boneMatrices) const;
};

struct AnimationController {
    void play(Handle<SkeletalAnimation> anim, float blendTime = 0.0f);
    void update(float deltaTime);
    std::span<const glm::mat4> getBoneMatrices() const;
};
```

### 5.3 Property Animations

For animating arbitrary numeric properties (UI, particles, etc.).

```yaml
animations:
  button_hover:
    type: property
    duration: 0.2
    properties:
      scale:
        from: 1.0
        to: 1.1
        easing: ease_out_quad
      color.a:
        from: 0.8
        to: 1.0
        easing: linear

  damage_flash:
    type: property
    duration: 0.3
    properties:
      tint:
        keyframes:
          - time: 0.0
            value: [1, 1, 1, 1]
          - time: 0.1
            value: [1, 0.3, 0.3, 1]
          - time: 0.3
            value: [1, 1, 1, 1]
        easing: linear
```

**Easing Functions:**

| Name | Description |
|------|-------------|
| `linear` | Constant speed |
| `ease_in_quad` | Accelerate (quadratic) |
| `ease_out_quad` | Decelerate (quadratic) |
| `ease_in_out_quad` | Accelerate then decelerate |
| `ease_in_cubic` | Accelerate (cubic) |
| `ease_out_cubic` | Decelerate (cubic) |
| `ease_in_out_cubic` | Smooth acceleration/deceleration |
| `ease_out_elastic` | Overshoot with bounce |
| `ease_out_bounce` | Bouncing settle |

**C++ API:**

```cpp
struct PropertyAnimation {
    float duration;
    std::unordered_map<std::string, PropertyTrack> tracks;

    float sample(const std::string& property, float time) const;
    glm::vec4 sampleVec4(const std::string& property, float time) const;
};
```

### 5.4 State Machines

For complex animation logic with transitions.

```yaml
animation_controllers:
  player:
    type: state_machine
    default_state: idle

    parameters:
      speed: float
      grounded: bool
      attacking: bool

    states:
      idle:
        animation: player_idle
        transitions:
          - to: walk
            condition: speed > 0.1 && grounded
          - to: fall
            condition: "!grounded"
          - to: attack
            condition: attacking

      walk:
        animation: player_walk
        speed_multiplier: $speed  # Parameterized playback
        transitions:
          - to: idle
            condition: speed < 0.1
          - to: run
            condition: speed > 5.0
          - to: fall
            condition: "!grounded"

      attack:
        animation: player_attack
        transitions:
          - to: idle
            condition: animation_finished

    # Blend tree for complex locomotion
    blend_trees:
      locomotion:
        type: blend_1d
        parameter: speed
        children:
          - animation: player_idle
            threshold: 0
          - animation: player_walk
            threshold: 2
          - animation: player_run
            threshold: 6
```

**C++ API:**

```cpp
class AnimationStateMachine {
public:
    void setParameter(const std::string& name, float value);
    void setParameter(const std::string& name, bool value);
    void update(float deltaTime);

    const std::string& currentState() const;
    std::span<const glm::mat4> getBoneMatrices() const;
};
```

---

## 6. Dependency Resolution

Dependency resolution is planned but not implemented in the current
`AssetManager`. Assets are loaded on demand without a dependency graph.

### 6.1 Dependency Graph

Assets declare dependencies explicitly or implicitly:

```yaml
assets:
  shaders:
    voxel:
      vertex: shaders/voxel.vert
      fragment: shaders/voxel.frag
      # Implicit dependency: both shader files

  materials:
    stone:
      shader: shaders/voxel           # Explicit dependency
      textures:
        diffuse: textures/stone       # Explicit dependency
        normal: textures/stone_normal
```

### 6.2 Load Ordering

```cpp
class DependencyResolver {
public:
    // Build load order from manifest
    std::vector<AssetID> resolveLoadOrder(const AssetManifest& manifest);

    // Detect circular dependencies
    std::optional<std::vector<AssetID>> findCycle() const;

private:
    void topologicalSort();
    std::unordered_map<AssetID, std::vector<AssetID>> m_dependencies;
};
```

**Load Order Example:**

```
1. textures/stone.png        (no deps)
2. textures/stone_normal.png (no deps)
3. shaders/voxel.vert        (no deps)
4. shaders/voxel.frag        (no deps)
5. shaders/voxel             (depends on 3, 4)
6. materials/stone           (depends on 2, 5)
```

### 6.3 Shader Inheritance

Shaders support inheritance via the `inherit` field, allowing shader variants to share common configuration:

```yaml
assets:
  shaders:
    voxel_base:
      vertex: shaders/voxel.vert
      fragment: shaders/voxel_opaque.frag
      defines:
        ENABLE_AO: true
        MAX_TEXTURE_LAYERS: 256

    voxel_transparent:
      inherit: shaders/voxel_base       # Parent shader
      fragment: shaders/voxel_alpha.frag  # Override fragment only
      defines:
        ENABLE_ALPHA_BLEND: true         # Additional define
```

**Inheritance Resolution:**

1. Parent must be loaded before child (automatic dependency)
2. Child inherits all fields from parent
3. Child's explicit fields override parent's
4. `defines` maps are merged (child values win on conflict)

```cpp
struct ResolvedShaderConfig {
    std::string vertex;
    std::string fragment;
    std::optional<std::string> geometry;
    std::optional<std::string> compute;
    std::unordered_map<std::string, std::string> defines;
};

ResolvedShaderConfig resolveShaderInheritance(
    ryml::ConstNodeRef config,
    const std::optional<ResolvedShaderConfig>& parent
) {
    ResolvedShaderConfig result;

    // Start with parent values if inheriting
    if (parent) {
        result = *parent;
    }

    // Override with child values
    if (config.has_child("vertex")) {
        config["vertex"] >> result.vertex;
    }
    if (config.has_child("fragment")) {
        config["fragment"] >> result.fragment;
    }

    // Merge defines (child wins)
    if (config.has_child("defines")) {
        for (auto child : config["defines"].children()) {
            ryml::csubstr key = child.key();
            std::string value;
            child >> value;
            result.defines[std::string(key.data(), key.size())] = value;
        }
    }

    return result;
}
```

**Circular Inheritance Detection:**

```cpp
bool hasCircularInheritance(const std::string& id,
                            std::unordered_set<std::string>& visited) {
    if (visited.contains(id)) {
        return true;  // Cycle detected
    }
    visited.insert(id);

    auto& entry = m_entries[id];
    if (entry.inherit) {
        return hasCircularInheritance(*entry.inherit, visited);
    }
    return false;
}
```

### 6.4 Lazy Loading

Assets can be marked for deferred loading:

```yaml
assets:
  textures:
    rare_item:
      path: textures/rare_item.png
      load: lazy                # Load on first access

  audio:
    boss_music:
      path: music/boss.ogg
      load: lazy
      priority: high            # Preload hint when entering boss area
```

---

## 7. Runtime Asset Management

### 7.1 AssetManager API

```cpp
class AssetManager {
public:
    // Initialization
    void loadManifest(const std::string& path);
    void loadManifest(std::span<const char> yamlData);

    // Loader registration (call before loadManifest)
    template<typename LoaderT>
    void registerLoader();

    void registerLoader(std::unique_ptr<IAssetLoader> loader);

    // Synchronous access (blocks if not loaded)
    template<typename T>
    Handle<T> get(const std::string& id);

    // Async access
    template<typename T>
    std::future<Handle<T>> getAsync(const std::string& id);

    // Batch loading
    void preload(std::span<const std::string> ids);
    void preloadCategory(const std::string& category);

    // Status
    AssetStatus getStatus(const std::string& id) const;
    float getLoadProgress() const;  // 0.0 - 1.0

    // Lifecycle
    void unload(const std::string& id);
    void unloadUnused();            // Unload assets with refcount 0
    void reloadAll();               // Development hot-reload

    // Events
    void onAssetLoaded(std::function<void(const std::string&)> callback);
    void onAssetFailed(std::function<void(const std::string&, const Error&)> callback);

private:
    // Loader selection by category or type
    IAssetLoader* findLoader(const std::string& category, ryml::ConstNodeRef config);

    std::unordered_map<std::string, AssetEntry> m_entries;
    std::unordered_map<std::string, std::shared_ptr<AssetBase>> m_cache;
    std::unordered_map<std::string, std::unique_ptr<IAssetLoader>> m_loaders;  // category -> loader
};

enum class AssetStatus {
    Unknown,      // Not declared in any manifest
    Declared,     // In manifest, not loaded
    Loading,      // Currently loading
    Ready,        // Loaded and available
    Failed,       // Load failed
    Unloaded      // Was loaded, now released
};
```

### 7.2 Handle System

Handles provide type-safe, reference-counted access:

```cpp
template<typename T>
class Handle {
public:
    Handle() = default;
    Handle(const Handle&);
    Handle(Handle&&) noexcept;
    ~Handle();

    // Access
    T* operator->() const;
    T& operator*() const;
    explicit operator bool() const;

    // Comparison
    bool operator==(const Handle&) const;

    // Metadata
    const std::string& id() const;
    AssetStatus status() const;

private:
    std::shared_ptr<AssetEntry<T>> m_entry;
};

// Weak reference (doesn't keep asset alive)
template<typename T>
class WeakHandle {
public:
    Handle<T> lock() const;
    bool expired() const;
};
```

### 7.3 Usage Patterns

```cpp
// Initialization
AssetManager assets;
assets.loadManifest("manifest.yaml");

// Preload critical assets
assets.preloadCategory("shaders");
assets.preloadCategory("textures/blocks");

// Get assets (blocks until ready)
auto stoneTexture = assets.get<TextureAsset>("rigel:textures/stone");
auto voxelShader = assets.get<ShaderAsset>("rigel:shaders/voxel");

// Async loading for non-critical assets
auto musicFuture = assets.getAsync<AudioAsset>("rigel:audio/ambient");

// Check status before access
if (assets.getStatus("rigel:textures/rare") == AssetStatus::Ready) {
    auto tex = assets.get<TextureAsset>("rigel:textures/rare");
}

// Periodic cleanup
assets.unloadUnused();
```

---

## 8. Integration Points

### 8.1 Voxel Engine Integration

The asset system integrates with `VoxelEngine.md` components:

```yaml
# Block definitions reference assets
blocks:
  stone:
    model: rigel:models/cube
    textures:
      all: rigel:textures/stone
    sounds:
      place: rigel:audio/block_place
      break: rigel:audio/stone_break

  water:
    model: rigel:models/fluid
    textures:
      still: rigel:animations/water_still    # Animated texture
      flow: rigel:animations/water_flow
    render_layer: transparent
```

**TextureAtlas Integration:**

```cpp
class TextureAtlasBuilder {
public:
    // Add textures from asset system
    void addFromManifest(AssetManager& assets, const std::string& category);

    // Resolve animated textures
    void addAnimation(Handle<FlipbookAnimation> anim);

    TextureAtlas build();
};

// Usage
TextureAtlasBuilder builder;
builder.addFromManifest(assets, "textures/blocks");
builder.addAnimation(assets.get<FlipbookAnimation>("rigel:animations/water"));
TextureAtlas atlas = builder.build();
```

### 8.2 ResourceRegistry Integration

The asset system reads raw data through `ResourceRegistry`:

```cpp
class EmbeddedAssetSource : public IAssetSource {
public:
    std::span<const char> read(const std::string& path) override {
        return ResourceRegistry::Get(path);
    }

    bool exists(const std::string& path) override {
        try {
            ResourceRegistry::Get(path);
            return true;
        } catch (...) {
            return false;
        }
    }
};
```

### 8.3 Hot-Reload Support (Development)

```cpp
#ifdef RIGEL_DEV_BUILD
class FileWatcher {
public:
    void watch(const std::string& directory);
    void poll();  // Check for changes

    void onFileChanged(std::function<void(const std::string&)> callback);
};

// In Application update loop
void Application::update() {
    m_fileWatcher.poll();
    // AssetManager receives change notifications and reloads affected assets
}
#endif
```

---

## 9. Error Handling

### 9.1 Error Types

```cpp
namespace Rigel::Asset {

class AssetError : public std::runtime_error {
public:
    AssetError(const std::string& assetId, const std::string& message);
    const std::string& assetId() const;
};

class AssetNotFoundError : public AssetError {
    // Asset ID not declared in any manifest
};

class AssetLoadError : public AssetError {
    // Failed to load asset data
};

class AssetParseError : public AssetError {
    // Failed to parse asset format
    int line() const;
    int column() const;
};

class DependencyError : public AssetError {
    // Dependency failed to load or circular dependency
    const std::vector<std::string>& dependencyChain() const;
};

}
```

### 9.2 Fallback Assets

```yaml
# manifest.yaml
fallbacks:
  textures: textures/missing.png      # Pink/black checkerboard
  shaders: shaders/error.glsl         # Magenta solid color
  models: models/error_cube.json      # Bright colored cube
  audio: null                          # Silent (no fallback)
```

```cpp
// AssetManager uses fallbacks on load failure
template<typename T>
Handle<T> AssetManager::get(const std::string& id) {
    auto status = getStatus(id);

    if (status == AssetStatus::Failed) {
        auto fallbackId = getFallback<T>();
        if (fallbackId) {
            spdlog::warn("Asset '{}' failed, using fallback '{}'", id, *fallbackId);
            return get<T>(*fallbackId);
        }
        throw AssetLoadError(id, m_errors[id]);
    }

    // ... normal loading
}
```

### 9.3 Validation

```cpp
class ManifestValidator {
public:
    struct ValidationResult {
        bool valid;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
    };

    ValidationResult validate(const AssetManifest& manifest);

private:
    void checkRequiredFields(const AssetDeclaration& decl);
    void checkFileExists(const std::string& path);
    void checkDependencies(const AssetDeclaration& decl);
    void checkCircularDependencies();
};
```

---

## 10. Extension Mechanisms

### 10.1 Custom Asset Loaders

```cpp
/// Context provided to asset loaders
struct LoadContext {
    const std::string& id;              // Asset ID being loaded
    ryml::ConstNodeRef config;          // Full manifest config for this asset

    // Load raw data from ResourceRegistry
    std::span<const char> loadResource(const std::string& path) const;

    // Load a dependency (another asset that must be loaded first)
    template<typename T>
    Handle<T> loadDependency(const std::string& assetId);
};

class IAssetLoader {
public:
    virtual ~IAssetLoader() = default;

    // Category this loader handles (e.g., "textures", "shaders")
    virtual std::string_view category() const = 0;

    // Load asset using the provided context
    // The loader reads config, loads resources, and returns the asset
    virtual std::shared_ptr<AssetBase> load(const LoadContext& ctx) = 0;

    // Optional: unload/cleanup (called when asset is evicted from cache)
    virtual void unload(AssetBase& asset) {}
};

// Registration - loaders are registered by category
assets.registerLoader("textures", std::make_unique<TextureLoader>());
assets.registerLoader("shaders", std::make_unique<ShaderLoader>());
```

**Example: Simple Texture Loader:**

```cpp
class TextureLoader : public IAssetLoader {
public:
    std::string_view category() const override { return "textures"; }

    std::shared_ptr<AssetBase> load(const LoadContext& ctx) override {
        // Get path from config
        std::string path;
        ctx.config["path"] >> path;

        // Load raw image data
        auto data = ctx.loadResource(path);

        // Decode and create texture
        auto texture = std::make_shared<TextureAsset>();
        // ... stb_image loading, OpenGL texture creation ...

        // Apply optional properties from config
        if (ctx.config.has_child("filter")) {
            std::string filter;
            ctx.config["filter"] >> filter;
            // ... apply filter setting ...
        }

        return texture;
    }
};
```

**Example: Multi-Source Shader Loader:**

```cpp
class ShaderLoader : public IAssetLoader {
public:
    std::string_view category() const override { return "shaders"; }

    std::shared_ptr<AssetBase> load(const LoadContext& ctx) override {
        auto shader = std::make_shared<ShaderAsset>();

        // Handle inheritance first
        if (ctx.config.has_child("inherit")) {
            std::string parentId;
            ctx.config["inherit"] >> parentId;
            // Load parent and copy its configuration
            auto parent = ctx.loadDependency<ShaderAsset>(parentId);
            // Merge parent settings with current config...
        }

        // Load multiple source files
        std::string vertexSource, fragmentSource;

        if (ctx.config.has_child("vertex")) {
            std::string vertPath;
            ctx.config["vertex"] >> vertPath;
            auto data = ctx.loadResource(vertPath);
            vertexSource = std::string(data.data(), data.size());
        }

        if (ctx.config.has_child("fragment")) {
            std::string fragPath;
            ctx.config["fragment"] >> fragPath;
            auto data = ctx.loadResource(fragPath);
            fragmentSource = std::string(data.data(), data.size());
        }

        // Extract defines map
        std::unordered_map<std::string, std::string> defines;
        if (ctx.config.has_child("defines")) {
            for (auto child : ctx.config["defines"].children()) {
                ryml::csubstr key = child.key();
                std::string value;
                child >> value;
                defines[std::string(key.data(), key.size())] = value;
            }
        }

        // Compile and link
        shader->program = ShaderCompiler::compile({
            .vertex = vertexSource,
            .fragment = fragmentSource,
            .defines = defines
        });

        return shader;
    }
};
```

### 10.2 Custom Asset Types

```yaml
# Declare custom asset type in manifest
asset_types:
  vox_model:
    loader: vox
    extensions: [.vox]
    category: models

assets:
  models:
    treasure_chest:
      type: vox_model
      path: models/chest.vox
      scale: 0.0625
```

### 10.3 Asset Processors

Transform assets at load time:

```cpp
class IAssetProcessor {
public:
    virtual ~IAssetProcessor() = default;
    virtual void process(AssetBase& asset, ryml::ConstNodeRef config) = 0;
};

// Example: Generate mipmaps for textures
class MipmapProcessor : public IAssetProcessor {
public:
    void process(AssetBase& asset, ryml::ConstNodeRef config) override {
        auto& texture = static_cast<TextureAsset&>(asset);

        bool generateMipmaps = false;
        if (config.has_child("generate_mipmaps")) {
            config["generate_mipmaps"] >> generateMipmaps;
        }

        if (generateMipmaps) {
            glBindTexture(GL_TEXTURE_2D, texture.id);
            glGenerateMipmap(GL_TEXTURE_2D);
        }
    }
};
```

### 10.4 Asset Packs

Support for mod/DLC asset overrides:

```yaml
# mods/mymod/manifest.yaml
namespace: mymod
priority: 100                    # Higher = loaded later (overrides)

# Override base game assets
overrides:
  rigel:textures/stone: textures/better_stone.png

# Add new assets
assets:
  textures:
    custom_ore:
      path: textures/custom_ore.png
```

```cpp
// Load order
assets.loadManifest("assets/manifest.yaml");      // Base game
assets.loadPack("mods/mymod/manifest.yaml");      // Mod overrides

// "rigel:textures/stone" now returns the mod's version
```

---

## Appendix A: Complete Manifest Example

```yaml
# assets/manifest.yaml
namespace: rigel
version: 1.0

# imports: [...]  # Planned, not yet implemented

fallbacks:
  textures: textures/missing.png
  shaders: shaders/error
  models: models/error_cube
  audio: null

asset_types:
  vox_model:
    loader: vox
    extensions: [.vox]
    category: models

assets:
  shaders:
    voxel_opaque:
      vertex: shaders/voxel.vert
      fragment: shaders/voxel_opaque.frag
      defines:
        MAX_TEXTURE_LAYERS: 256
        ENABLE_AO: true

    voxel_transparent:
      inherit: voxel_opaque
      fragment: shaders/voxel_transparent.frag

    ui:
      vertex: shaders/ui.vert
      fragment: shaders/ui.frag

  fonts:
    default:
      path: fonts/roboto.ttf
      sizes: [12, 16, 24, 32]
      charset: ascii_extended
```

---

## Appendix B: YAML Schema Reference

```yaml
# Top-level manifest fields
namespace: string              # Required: asset namespace
version: string                # Optional: manifest version
imports: [string]              # Planned: other manifests to include
fallbacks: map                 # Planned: fallback assets by category
asset_types: map               # Planned: custom asset type definitions

# Asset declaration common fields
<asset_name>:
  path: string                 # Path to source file
  type: string                 # Optional: explicit asset type
  load: eager | lazy           # Optional: load strategy (default: eager)
  priority: int                # Optional: load priority (higher = sooner)

# Texture-specific fields
filter: nearest | linear       # linear_mipmap planned
wrap: repeat | clamp | mirror  # Planned
format: rgba8 | rgb8 | srgb    # Planned
generate_mipmaps: bool         # Planned
max_anisotropy: int            # Planned

# Shader-specific fields
vertex: string
fragment: string
compute: string
geometry: string
defines: map
uniforms: map
inherit: string

# Animation-specific fields
type: flipbook | skeletal | property | state_machine
duration: float
loop: bool
interpolation: none | linear | smooth
easing: string
```

---

## Appendix C: Performance Considerations

| Optimization | Description |
|--------------|-------------|
| **Batch Loading** | Load multiple assets in single I/O pass |
| **Parallel Decoding** | Decode textures/audio on worker threads |
| **Streaming** | Large audio files streamed from embedded data |
| **Atlas Packing** | Combine small textures at build time |
| **Compression** | Support zstd-compressed embedded assets |
| **Caching** | Cache decoded assets to avoid re-parsing |

**Memory Budget Guidance:**

| Asset Type | Typical Size | Notes |
|------------|--------------|-------|
| Texture 256x256 RGBA | 256 KB | Per mip level |
| Texture Array 256x256x64 | 16 MB | Block atlas |
| Shader Program | 1-10 KB | Compiled binary |
| Model (1000 verts) | 50 KB | Vertices + indices |
| Animation Clip (60s) | 200 KB | Depends on bone count |
| Audio (1 min, 44kHz) | 10 MB | Uncompressed stereo |

---

## Appendix D: rapidyaml Usage Patterns

The asset system uses rapidyaml for YAML parsing. This appendix documents common patterns.

### D.1 Required Includes

```cpp
#include <ryml.hpp>
#include <ryml_std.hpp>  // For std::string support
```

### D.2 Parsing YAML

```cpp
// Parse from memory (zero-copy arena allocation)
ryml::Tree tree = ryml::parse_in_arena(ryml::to_csubstr(yamlData));
ryml::ConstNodeRef root = tree.rootref();

// Parse with filename for error messages
ryml::Tree tree = ryml::parse_in_arena(
    ryml::to_csubstr("manifest.yaml"),             // filename
    ryml::csubstr(yamlData.data(), yamlData.size()) // content from span
);
```

### D.3 Reading Values

```cpp
ryml::ConstNodeRef node = root["assets"]["textures"]["stone"];

// Read into variables using >> operator
std::string path;
node["path"] >> path;

int width;
node["width"] >> width;

float scale;
node["scale"] >> scale;

bool enabled;
node["enabled"] >> enabled;
```

### D.4 Optional Fields with Defaults

```cpp
// Check existence before reading
std::string filter = "nearest";  // default
if (node.has_child("filter")) {
    node["filter"] >> filter;
}

// Or use a helper function
template<typename T>
T readOr(ryml::ConstNodeRef node, const char* key, T defaultValue) {
    if (node.has_child(key)) {
        T value;
        node[key] >> value;
        return value;
    }
    return defaultValue;
}

// Usage
int maxAnisotropy = readOr(node, "max_anisotropy", 1);
bool generateMipmaps = readOr(node, "generate_mipmaps", false);
```

### D.5 Iterating Sequences

```cpp
// YAML:
// layers:
//   - textures/stone.png
//   - textures/dirt.png

ryml::ConstNodeRef layers = node["layers"];
for (ryml::ConstNodeRef layer : layers.children()) {
    std::string path;
    layer >> path;
    // process path...
}
```

### D.6 Iterating Maps

```cpp
// YAML:
// defines:
//   MAX_LIGHTS: 8
//   ENABLE_AO: true

ryml::ConstNodeRef defines = node["defines"];
for (ryml::ConstNodeRef child : defines.children()) {
    // key() and val() return csubstr directly - convert to std::string
    ryml::csubstr keyStr = child.key();
    std::string key(keyStr.data(), keyStr.size());  // "MAX_LIGHTS", "ENABLE_AO"

    std::string value;
    child >> value;  // "8", "true" (as strings) - use >> on the node itself
}
```

### D.7 Reading Arrays

```cpp
// YAML:
// origin: [0.5, 0, 0.5]

std::array<float, 3> origin;
ryml::ConstNodeRef arr = node["origin"];
for (size_t i = 0; i < 3 && i < arr.num_children(); ++i) {
    arr[i] >> origin[i];
}
```

### D.8 Error Handling

```cpp
try {
    ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(filename),
        ryml::to_csubstr(yamlData)
    );
} catch (const std::exception& e) {
    spdlog::error("Failed to parse {}: {}", filename, e.what());
    throw AssetParseError(filename, e.what());
}

// Check for required fields
if (!node.has_child("path")) {
    throw AssetParseError(assetId, "missing required field 'path'");
}
```

### D.9 Performance Notes

| Feature | Benefit |
|---------|---------|
| Arena allocation | Minimal heap allocations during parsing |
| Zero-copy strings | String views into source buffer where possible |
| In-situ parsing | Modifies source buffer to null-terminate tokens |
| No exceptions by default | Use `ryml::parse_in_arena` for exception-based API |

For maximum performance with large manifests:

```cpp
// Reuse tree across parses
ryml::Tree tree;
tree.reserve(estimatedNodeCount);

// Parse in-place (modifies yamlBuffer)
ryml::parse_in_place(
    ryml::to_csubstr(filename),
    ryml::to_substr(yamlBuffer),  // non-const!
    &tree
);
```
