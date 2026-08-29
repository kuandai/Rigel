# Embedded Asset System (ResourceRegistry)

The `ResourceRegistry` allows Rigel to access static assets compiled directly
into the executable. It combines Rigel-owned source assets with locally
generated Cosmic Reach derivatives without exposing their physical roots at
runtime.

Logical paths start at the root of either physical input:

- tracked `assets/` for Rigel-owned content;
- ignored `.rigel/assets/` for importer-owned CR content.

The physical prefix is never part of the logical path.

* **Wrong:** `ResourceRegistry::Get("assets/logo.png");`
* **Correct:** `ResourceRegistry::Get("logo.png");`

If the same logical path appears in both roots, CMake fails. Neither root
silently overrides the other.

## Relationship to AssetManager

`AssetManager` consumes embedded bytes from `ResourceRegistry` when it loads
manifest entries. The registry handles build-time embedding; the asset system
handles runtime parsing, loading, and caching.

## 1. Preparing Assets

For a Rigel-owned asset, place the file under tracked `assets/` and reconfigure
or build. CMake watches both resource trees for additions and removals.

Do not copy Cosmic Reach content into `assets/`. Instead run:

```bash
python3 scripts/rigel_assets.py stage /path/to/Cosmic-Reach.jar
python3 scripts/rigel_assets.py sync
```

CMake also synchronizes automatically, before it enumerates resources, when a
JAR is supplied by `-DRIGEL_COSMIC_REACH_JAR=...`, the matching environment
variable, or the canonical staged path. When none is available, only the
Rigel-owned root is embedded and source-only tests remain supported.

## 2. Accessing Assets in C++

Include the registry header:

```cpp
#include "ResourceRegistry.h"
```

Use the static `Get` method to retrieve a view of the file contents.

### API Reference

#### `static std::span<const char> ResourceRegistry::Get(const std::string& path)`

Retrieves a memory view of an embedded file.

* **Parameters:**
  * `path`: The logical path relative to either resource root. Use forward slashes (`/`).
* **Returns:**
  * `std::span<const char>`: A lightweight view of the memory. The data is read-only and lives for the lifetime of the application (static storage).
* **Throws:**
  * `std::runtime_error`: If the file path does not exist in the registry.

## 3. Usage Examples

### Example A: Loading text (Config/Shaders)

Since the data is returned as raw bytes, construct a `std::string` if you need text processing.

```cpp
try {
    auto data = ResourceRegistry::Get("config/settings.json");
    
    // Construct string from pointer and size
    std::string jsonString(data.data(), data.size());
    
    std::cout << "Config loaded: " << jsonString << "\n";
} catch (const std::exception& e) {
    std::cerr << "Asset missing: " << e.what() << "\n";
}
```

### Embedded Generator Content

The build embeds `generators/default.yaml`. Production new-world generation
resolves that named definition through the manifest. Published worlds reload
their save-owned normalized snapshot instead.

Streaming, renderer, and persistence policy are internal code and are not
embedded configs.

### Example B: Loading Binary Data (OpenGL Textures)

You can pass the data pointer directly to libraries like `stb_image` or OpenGL.

```cpp
auto pngData = ResourceRegistry::Get("textures/player.png");

int width, height, channels;
unsigned char* img = stbi_load_from_memory(
    reinterpret_cast<const unsigned char*>(pngData.data()), 
    static_cast<int>(pngData.size()), 
    &width, &height, &channels, 4
);
```

---

## Related Docs

- `docs/AssetSystem.md`
- `docs/AssetOwnership.md`
- `docs/ConfigurationSystem.md`
