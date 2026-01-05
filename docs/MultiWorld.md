# Multi-World System

Status: WorldSet, WorldResources, and WorldView are implemented. The
application still boots a single default World/WorldView, but multiple Worlds
can now exist in memory.

This document describes the multi-world architecture that aligns with CR's
network model while preserving the existing naming scheme:

- A **World** is a single 3D voxel space.
- A **WorldSet** is a container that holds multiple Worlds.

This is a design+implementation document; networking remains a future layer.

---

## 1. Goals

- Support multiple independent voxel spaces in one session.
- Match CR's model where a session can host multiple world spaces.
- Keep server-authoritative ownership and client-side rendering boundaries.
- Allow multiple renderers to view the same World without duplicating meshes.

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

Renderer cache keys include mesh store identity to avoid collisions:

```
MeshGpuCache[(MeshStoreId, ChunkCoord, Revision)]
```

Multiple renderers can render the same WorldView without duplicating meshes.

---

## 4. Server vs Client WorldSets

### Server WorldSet

Holds authoritative Worlds, runs ticks, generates chunks, and produces deltas.

### Client WorldSet

Holds WorldViews, applies deltas, builds meshes off-thread, and renders.

Single-player can run both in-process with a local loopback.

---

## 5. Streaming and Meshing

- Streaming is per WorldView.
- Mesh builds are based on replicated data snapshots.
- GPU uploads are main-thread only.
- Revisions guard against applying stale meshes.

---

## 6. Configuration

Per-world config overlays should be supported by convention:

- `config/worlds/<worldId>/world_generation.yaml`
- `config/worlds/<worldId>/render.yaml`
- `config/worlds/<worldId>/persistence.yaml`

Global defaults apply when per-world overrides are absent.

---

## 7. Minimal Integration Path

1) Add WorldId and WorldRegistry in WorldSet.
2) Create one default World and WorldView.
3) Move CPU mesh ownership to WorldView.
4) Update renderer to accept WorldId + mesh store.
5) Add WorldSet entry points in the app layer.

---

## 8. Future Networking Hooks

Planned replication surface (no protocol binding yet):

```
WorldView::applyChunkDelta(WorldId, ChunkCoord, Payload)
World::serializeChunkDelta(WorldId, ChunkCoord)
```

These are placeholders for CR-style S2C/C2S pipelines.
