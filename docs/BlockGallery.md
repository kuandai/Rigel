# Developer block gallery

The block gallery is a developer-only runtime mode for inspecting every
renderable block state that Rigel actually loaded. It is regenerated from the
frozen runtime `BlockRegistry`; it is not a second asset inventory, a save, or a
special renderer.

## Prepare and build the runtime assets

The interactive gallery needs the same generated Cosmic Reach assets as the
normal interactive world. From the repository root, stage a legitimately
obtained JAR, synchronize the ignored generated tree, and verify it:

```bash
python3 scripts/rigel_assets.py stage /absolute/path/to/Cosmic-Reach.jar
python3 scripts/rigel_assets.py sync
python3 scripts/rigel_assets.py status
python3 scripts/rigel_assets.py validate
```

Synchronization, status, and validation report the generated PNG alpha census,
effective single-layer and mixed registration counts, and any noncanonical but
permitted alpha/layer cross-classifications. This keeps the ignored import's
material-layer evidence tied to the exact generated tree and importer revision.

Then configure and build Rigel as described in the top-level `README.md`.
Reconfigure an existing build after staging or changing a JAR so CMake embeds a
fresh content-addressed generated-resource snapshot. The staged JAR, generated
assets, and import provenance stay under the ignored `.rigel/` tree.

A checkout without `.rigel/` and without a JAR still supports source-only
configure, build, and CTest runs. It does not have the block and texture set
needed for either interactive world mode; interactive startup reports the asset
preparation instructions instead of opening an incomplete gallery.

## Launch selection

For a build directory named `build-release`, launch the gallery with either
command-line spelling:

```bash
./build-release/bin/Rigel --world-mode block-gallery
./build-release/bin/Rigel --world-mode=block-gallery
```

The environment fallback is equivalent when the command line does not select a
mode:

```bash
RIGEL_WORLD_MODE=block-gallery ./build-release/bin/Rigel
```

Launch selection has the following exact behavior:

- The only values are the case-sensitive strings `normal` and
  `block-gallery`.
- A command-line `--world-mode` takes precedence over `RIGEL_WORLD_MODE`; when
  the option is present, the environment variable is not read. For example,
  `RIGEL_WORLD_MODE=block-gallery ./build-release/bin/Rigel --world-mode normal`
  opens the normal world.
- With neither selection, Rigel defaults to the normal world.
- A missing or empty value, any other value, an unknown option, or more than one
  `--world-mode` is an error. Decoding fails before application startup and the
  process exits unsuccessfully. Missing, empty, and invalid values name the two
  accepted values; an unknown option names the supported `--world-mode` form;
  and a duplicate reports that `--world-mode` was provided more than once.

The launch mode is process-local. It is not a user preference or a world
setting, and gallery code does not read the environment.

## Navigate and identify specimens

The gallery opens at a deterministic overview and uses the normal free-fly
camera. The shipped bindings are:

| Input | Action |
| --- | --- |
| Mouse | Look while the cursor is captured |
| W / S | Fly forward / backward |
| A / D | Fly left / right |
| Space / Left Control | Fly up / down |
| Left Shift | Double movement speed while held |
| Tab | Release or recapture the mouse cursor |

Global input preferences may replace the seven player movement bindings; Tab
remains the fixed prototype mouse-capture binding.

Treat the center of the viewport as the targeting crosshair. Aim it at a
specimen from within eight blocks. The top-right **Block gallery target** window
then shows the full block-state identifier, one-based catalog position and
catalog size, zero-based grid coordinate, normalized model identifier, cuboid
count, orientation, base and effective render layers, compact texture-slot layer
mapping, collision shape, culling and opacity flags, and resolved
texture-binding count. It also reports the selected cuboid's one-based position
within the model, cardinal face, and exact ray distance in blocks. Collision is
reported as `none`, `full cube`, `one box`, or an exact box count; conservative
fallback provenance is appended when present. Culling diagnostic cells replace
the catalog position with their case label, case ordinal, and pair-cell ordinal
while retaining the same collision and hit lines. The current renderer does
not draw a separate reticle; the camera's center ray is the crosshair
direction. Empty space and the reference floor show
`No catalog specimen targeted.`

Effective layers are listed once in render order and include both the block
default and its texture-slot overrides. Single-layer specimens omit the
redundant slot mapping. Mixed-layer specimens show the first four slots in
normalized model order and report how many additional slots were omitted.
The presentation adapter derives render-layer strings from the immutable
runtime `BlockType` and its normalized model slot order, and derives collision
text directly from `BlockCollisionShape`. The ImGui layer displays those values
without inspecting importer state or reinterpreting visual model geometry.

The gallery does not draw targeted collision wireframes. The selection outline
reuses the entity-debug line shader and GPU buffers, but its bounds come from
the oriented visual model. It is always available as selection feedback and is
not controlled by the F1 diagnostics toggle. Collision inspection remains at
the compact presentation boundary rather than adding a separate renderer or
launch option.

Targeting uses the normal grid DDA with an overhang-aware owner search, then
intersects declared oriented model faces. Rays pass through slab empty space,
gaps, missing faces, and empty models; zero-thickness two-sided crop surfaces
remain selectable. Geometry outside its owning cell is found from the frozen
registry's aggregate visual extents, while catalog identity remains attached
to the owning coordinate. See `docs/BlockTargeting.md` for the exact hit,
inside-origin, tie, stopping, and outline behavior.

## Catalog and coordinates

The catalog is constructed only after normal asset loading has successfully
published and frozen the runtime registry. Failed or importer-omitted assets
cannot appear. The only catalog exclusion policy is an explicit runtime model
with no cuboids. The startup log reports loaded registrations, renderable
specimens, explicit-empty exclusions, grid dimensions, and spacing; the
exclusion list retains the identifier and `BlockID` for accounting.

Ordering and placement do not depend on registry or hash-map iteration:

1. Parse each identifier into namespace, base identifier, and state properties.
2. Sort by namespace, then base identifier, then property key/value pairs after
   sorting those pairs. The full identifier is the final tie-breaker.
3. Keep the resulting `namespace:base_identifier` families contiguous in that
   linear order.
4. Use `ceil(sqrt(renderable_count))` columns and map the linear sequence onto
   alternating left-to-right and right-to-left rows. This serpentine mapping
   keeps entries adjacent across row turns and leaves unused cells only in the
   final partial row.

For zero-based grid coordinate `(column, row)`, the specimen's owning block cell
is exactly:

```text
(world_x, world_y, world_z) = (4 * column, 1, 4 * row)
```

One catalog value owns the identifier, family, zero-based index, grid
coordinate, and world position used by generation and targeting. The floor, when
a loaded opaque full cube is available, occupies `y = 0` and is not part of the
catalog.

With the validated Cosmic Reach 0.6.1 import described in
`docs/CosmicReachImportParity.md`, the runtime census is 2,021 registrations,
2,020 gallery specimens, and one explicit-empty exclusion (`base:air`). These
numbers account for runtime registrations; they are not a claim that Rigel
supports every source block state.

## Culling diagnostic zone

The isolated catalog grid is followed, eight Z cells beyond its last specimen
row, by a small adjacent-pair diagnostic zone. When suitable loaded
registrations exist, its groups appear left to right as:

1. an opaque full-cube baseline;
2. a model that culls a shared face against the same block type; and
3. a partial model whose shared boundary is removed by complete opposite-face
   coverage.

Each group places two identical blocks in neighboring X cells, with groups four
cells apart and the same reference floor beneath them. The zone makes shared
boundary removal, conservative partial-model coverage, and same-type culling
easy to inspect through the production mesher. Diagnostic blocks deliberately
duplicate catalog registrations but are not specimens, so they never
participate in catalog index or grid lookup. Each cell is instead targetable
through immutable diagnostic placement metadata that owns the stable case kind,
label, pair position, source block identity, and world position consumed by
generation. Targeting either member identifies the case and the member's
one-based position in its pair. A missing suitable runtime registration simply
omits that group.

## Runtime and persistence semantics

Gallery placement enters ordinary chunks and follows the normal
`WorldGenerator`, asynchronous load, streaming, eviction, mesh invalidation,
`MeshBuilder`, `TextureAtlas`, render-layer, lighting, culling, `WorldView`, and
`FrameRenderer` paths. There is no gallery renderer. Generated gallery cells are
assigned full skylight so the gallery is readable, while scalar emitted light
and attenuation remain block-registration properties.

The mode is read-only: left-click removal, right-click placement, and F2 demo
entity spawning are suppressed. The normal world and any existing save are
never opened or modified.

Gallery bootstrap installs a process-private `InMemoryStorageBackend` at a
virtual root. Chunk eviction, retry, reload, and application close still perform
their normal serialization lifecycle against that backend, but no world data,
save directory, autosave, or user-world metadata reaches the filesystem. All
gallery world state disappears with the process and is regenerated on the next
launch. Global user preferences remain their normal separate owner and still
control display and input behavior.

## Import boundary and known limitations

The gallery is an inspection of Rigel's runtime boundary, not of the contents of
a JAR. `scripts/rigel_assets.py` owns Cosmic Reach parsing. A state omitted by
the importer never becomes a runtime registration and therefore is absent from
the gallery; it is not counted as an explicit-empty gallery exclusion. Inspect
`.rigel/cosmic-reach-import.json` and `docs/CosmicReachImportParity.md` for the
sorted omission census and importer provenance.

Current limitations are visible in the gallery and should not be interpreted as
broader Cosmic Reach coverage:

- Plane and mixed plane/cuboid primitives are unsupported and omitted by the
  importer rather than approximated.
- Animated block textures and non-16-by-16 block textures are unsupported;
  affected source states are omitted under the importer's disjoint provenance
  reasons.
- Targeting is independent of collision: declared visual model faces can hit
  with `collision: none`, while collision-only space cannot. It supports
  normalized cuboid faces, including measured zero-thickness two-sided
  surfaces, but not source plane primitives, arbitrary triangles, OBBs, convex
  selection, or entities.
- The outline draws every oriented cuboid independently. It is not a
  silhouette union, so internal edges may remain visible on multi-cuboid
  models.
- Model ambient occlusion is simplified. Cube-style AO applies only when a
  normalized face requests it and spans a complete unit-cell boundary; other
  model faces use the fully unoccluded level, and only a closed full-cell model
  is treated as a model AO occluder.
- Lighting is block-level, not model- or cuboid-level. `emits_light` and
  `light_attenuation` are scalar registration properties, geometry coverage does
  not derive them, and Cosmic Reach model emission texture maps are outside the
  normalized block contract.

## Framebuffer review

The real-asset integration test renders cube, slab, stair, multi-cuboid,
rotated, cropped-UV, transparent, out-of-cell, and alpha-cutout representatives
through the software-EGL/OpenGL production path. It always reads color and depth
results. To publish a complete capture set for manual inspection, use an
absolute destination outside the source tree:

```bash
rigel_gallery_captures=/absolute/path/to/gallery-captures
DISPLAY= WAYLAND_DISPLAY= LIBGL_ALWAYS_SOFTWARE=1 \
RIGEL_GALLERY_CAPTURE_DIRECTORY="$rigel_gallery_captures" \
  ./build-release/Rigel_generated_asset_integration_tests \
  --filter RenderGallerySpecimens
```

Inspect all PPM files listed by `capture-manifest.txt`. The images are external
review evidence, not repository fixtures or a cross-platform pixel-hash
contract. `docs/TestFramework.md` describes the capture publication guarantees
and the rest of the CTest workflow.

## Related documentation

- `docs/VoxelEngine.md`
- `docs/BlockTargeting.md`
- `docs/AssetOwnership.md`
- `docs/CosmicReachImportParity.md`
- `docs/RenderingPipeline.md`
- `docs/InputSystem.md`
- `docs/TestFramework.md`
