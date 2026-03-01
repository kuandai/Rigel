# Entity System

This document describes the current entity system in Rigel, including runtime
behavior, rendering, and persistence.

---

## 1. Overview

Entities are runtime objects with physics, rendering, and persistence hooks:

- `Entity` stores state (position, velocity, bounds, tags).
- `WorldEntities` owns all entities in a `Voxel::World`.
- `EntityRenderer` renders entities and handles shadow casting.
- `EntityModelAsset` describes meshes, textures, animations, and hitboxes.
- `Asset::EntityTypeRegistry` stores deterministic entity definition metadata
  compiled from asset IR.

---

## 2. Core Runtime Types

### 2.1 Entity

`Entity` is the base class and stores:

- Transform: `position`, `velocity`, `viewDirection`
- Physics: gravity modifier, floor friction, collision flags
- Bounds: `localBounds` and `worldBounds`
- Tags: `EntityTags` (e.g. `NoClip`, `NoSaveInChunks`)
- Render state: model handle + tint
- Components: update and render component lists

Collision is axis-aligned (AABB) and resolved per-axis against voxel solids.
Entities tagged `EntityTags::NoClip` bypass collision resolution.

### 2.2 Components

Two component interfaces exist:

- `IUpdateEntityComponent` for update logic
- `IRenderEntityComponent` for custom rendering hooks

Components are attached to an entity and invoked during update/render.

---

## 3. World Ownership and Spatial Partitioning

### 3.1 WorldEntities

`WorldEntities` is owned by `Voxel::World` and provides:

- `spawn`, `despawn`, and lookup by `EntityId`
- `tick(dt)` to update entities
- iteration over all entities

### 3.2 Regions and Chunks

Entities are indexed into spatial buckets for persistence:

- `EntityRegion` groups chunks into a 16x16x16 chunk region.
- `EntityChunk` holds pointers to entities within a chunk.
- `WorldEntities::updateEntityChunk` keeps an entity in the correct bucket.

`EntityRegion` has active and inactive chunk maps, but current runtime flow
only uses active chunks. `deactivateChunk(...)` exists but is not currently
invoked by `WorldEntities` update/persistence paths.

---

## 4. Rendering

### 4.1 EntityRenderer

`EntityRenderer`:

- Uses `shaders/entity` for main pass.
- Uses `shaders/entity_shadow_depth` for shadow pass.
- Applies frustum culling using the view-projection matrix.

Per-entity ambient occlusion is computed by sampling a 3x3x3 cube of nearby
voxel blocks around the entity bounds center.

### 4.2 Model Instances

Rendering is delegated to `IEntityModelInstance`:

- Each entity lazily creates a model instance the first time it renders.
- The instance stores CPU vertices and uploads to a dynamic VBO.
- Bone animations are evaluated each frame and trigger mesh rebuilds.

Shadows use a separate render path that writes into the voxel shadow cascades.

---

## 5. Entity Models and Animations

### 5.1 EntityModelAsset

`EntityModelAsset` describes:

- `texture_width`, `texture_height`
- `model_scale`
- `render_offset`
- `lighting` (`lit` or `unlit`)
- `hitbox` (optional AABB, used to set entity bounds)
- `textures` map (e.g. `diffuse`, `emission`)
- `bones` with cube geometry
- `animation_set` and `default_animation`

Entity definition metadata from the IR pipeline is loaded separately from model
assets and includes identifier/model/animation/render fields plus preserved
source-specific extension metadata. Runtime code can query this metadata via
`WorldResources::entityTypes()`.

### 5.2 Model Format (YAML/JSON)

Models are loaded via the `entity_models` asset category. Each model file can
be JSON or YAML (rapidyaml parser).

Key fields:

```yaml
texture_width: 64
texture_height: 64
model_scale: 1.0
render_offset: [0.0, 0.0, 0.0]
lighting: lit
textures:
  diffuse: textures/entity/my_entity.png
  emission: textures/entity/my_entity_emissive.png
hitbox:
  min: [-0.4, 0.0, -0.4]
  max: [0.4, 1.8, 0.4]
bones:
  - name: body
    pivot: [0, 0, 0]
    rotation: [0, 0, 0]
    cubes:
      - origin: [-0.5, 0, -0.5]
        size: [1, 1, 1]
        uv: [0, 0]
```

### 5.3 Animation Sets

Animation sets are loaded via `entity_anims`:

```yaml
animations:
  idle:
    duration: 1.0
    loop: true
    bones:
      body:
        rotation:
          - time: 0.0
            value: [0, 0, 0]
          - time: 0.5
            value: [0, 20, 0]
          - time: 1.0
            value: [0, 0, 0]
```

Tracks support either sequences of `{ time, value }` or a map keyed by time.

---

## 6. Persistence

Entities are serialized via `Persistence::saveWorldToDisk`:

- Entities are grouped into `EntityRegionSnapshot` entries.
- Regions are keyed by `EntityRegionKey` (zone + region coords).
- Entities tagged `EntityTags::NoSaveInChunks` are skipped.

When loading:

- `EntityFactory` is used to instantiate known types.
- Unknown types fall back to a generic `Entity` with the type ID.
- Position, velocity, view direction, and model ID are restored.

Persistence stores string `typeId` + `modelId` directly in entity payloads.
These values remain compatible with IR-registered entity definitions because
definition registries are deterministic across runs.

---

## 7. Entity Factory

`EntityFactory` is a global registry mapping `typeId` to a constructor:

```cpp
EntityFactory::instance().registerType(
    "rigel:interceptor",
    []() { return std::make_unique<InterceptorEntity>(); }
);
```

---

## Related Docs

- `docs/RenderingPipeline.md`
- `docs/AssetSystem.md`
- `docs/DebugTooling.md`
- `docs/PersistenceAPI.md`

Persistence and runtime spawners use this registry when possible.
