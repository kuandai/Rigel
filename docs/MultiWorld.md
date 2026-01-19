# Multi-World System

Status: WorldSet, WorldResources, and WorldView are implemented. The
application still boots a single default World/WorldView, but multiple Worlds
can now exist in memory. GPU cache sharing across multiple renderers is not
implemented; each renderer maintains its own GPU cache.

This document describes the multi-world architecture that aligns with CR's
network model while preserving the existing naming scheme:

- A **World** is a single 3D voxel space.
- A **WorldSet** is a container that holds multiple Worlds.

This document describes what exists today, and calls out planned extensions.

---

## 1. Goals

- Support multiple independent voxel spaces in one session.
- Match CR's model where a session can host multiple world spaces.
- Keep server-authoritative ownership and client-side rendering boundaries.
- Allow multiple renderers to view the same World without duplicating meshes
  (CPU meshes can be shared; GPU caches are still per renderer).

---

## 2. Core Concepts

### 2.1 WorldSet (Container)

WorldSet is the explicit container of Worlds. It owns shared resources and
provides lookup by WorldId.

```
struct WorldSet {
  WorldRegistry worlds;      // WorldId -> World
  WorldResources resources;  // shared registry + atlas
};
```

### 2.2 World (Voxel Space)

A World is the authoritative voxel space. It owns chunk data and a generator.
It does not own meshes.

```
struct World {
  WorldId id;
  WorldConfig config;
  ChunkStore chunks;
  WorldGenerator generator;
  TickScheduler tick;
};
```

### 2.3 WorldView (Client View)

WorldView is the client-side representation of a World. It owns CPU meshes,
streaming state, and renderer-facing config. It can host replicated chunk
data in the future.

```
struct WorldView {
  WorldId id;
  WorldRenderConfig renderConfig;
  WorldMeshStore meshes;
  StreamingController streaming;
};
```

---

## 3. Ownership and Rendering

- **World** owns authoritative chunk data.
- **WorldView** owns CPU meshes and derived state.
- **Renderer** owns GPU caches only and never owns chunk data.

CPU meshes live in `WorldMeshStore` and can be referenced by multiple views or
systems. GPU meshes are cached per `ChunkRenderer`, so separate renderers will
duplicate GPU buffers today.

---

## 4. Current Implementation

### 4.1 WorldSet API

`WorldSet` manages world entries and shared resources:

- `createWorld(WorldId id)` creates or returns a `World`.
- `createView(WorldId id, AssetManager& assets)` creates a `WorldView` for a
  world and initializes render resources.
- `world(id)` and `view(id)` return existing instances.
- `removeWorld(id)` destroys the World and its view (if present).
- `clear()` releases all worlds and resources.

World entries are stored as a `WorldEntry` containing:

```
struct WorldEntry {
  World world;
  std::unique_ptr<WorldView> view;
};
```

Only one `WorldView` is stored per world today.

### 4.2 Shared Resources

`WorldResources` are shared across all worlds:

- `BlockRegistry`
- `TextureAtlas`

Block definitions and textures are global in the current architecture.

### 4.3 World and Entities

Each `World` owns:

- `ChunkManager` (block data)
- `WorldGenerator`
- `WorldEntities`

Entity state is per-world and not shared across worlds.

### 4.4 WorldView and Streaming

Each `WorldView` owns:

- `ChunkStreamer` (async generation + meshing)
- `WorldMeshStore` (CPU meshes)
- `ChunkRenderer` (GPU cache)
- Render config and shader handles

`WorldView::setGenerator` binds the streaming pipeline to the world generator.

---

## 5. Persistence Integration

Persistence is scoped per world ID:

- Root path is `saves/world_<id>`.
- Per-world overrides are loaded from `config/worlds/<id>/...`.
- `WorldSet::persistenceContext(id)` supplies providers and storage for the
  active world.

`World` exposes a provider registry to formats (e.g., block registry provider).

---

## 6. Server vs Client WorldSets

### Server WorldSet

Holds authoritative Worlds, runs ticks, generates chunks, and produces deltas.

### Client WorldSet

Holds WorldViews, applies deltas, builds meshes off-thread, and renders.

Single-player can run both in-process with a local loopback.

---

## 7. Streaming and Meshing

- Streaming is per WorldView.
- Mesh builds are based on replicated data snapshots.
- GPU uploads are main-thread only.
- Revisions guard against applying stale meshes.

---

## 8. Configuration

Per-world config overlays should be supported by convention:

- `config/worlds/<worldId>/world_generation.yaml`
- `config/worlds/<worldId>/render.yaml`
- `config/worlds/<worldId>/persistence.yaml`

Global defaults apply when per-world overrides are absent.

---

## 9. Minimal Integration Path

1) Add WorldId and WorldRegistry in WorldSet.
2) Create one default World and WorldView.
3) Move CPU mesh ownership to WorldView.
4) Update renderer to accept WorldId + mesh store.
5) Add WorldSet entry points in the app layer.

---

## 10. Future Networking Hooks

Planned replication surface (no protocol binding yet):

```
WorldView::applyChunkDelta(WorldId, ChunkCoord, Payload)
World::serializeChunkDelta(WorldId, ChunkCoord)
```

These are placeholders for CR-style S2C/C2S pipelines.

---

## 11. Current Limitations

- Only a single `WorldView` is tracked per world.
- GPU mesh caches are renderer-local (no sharing across views).
- Application boot path creates only the default world and view.

---

## Related Docs

- `docs/VoxelEngine.md`
- `docs/WorldGeneration.md`
- `docs/PersistenceAPI.md`
- `docs/ConfigurationSystem.md`
