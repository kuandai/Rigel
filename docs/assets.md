# Embedded Asset System (ResourceRegistry)

The `ResourceRegistry` allows the `Rigel` application to access static assets (images, shaders, config files) that have been compiled directly into the executable binary. This removes the need for distributing loose files and prevents I/O errors at runtime.

Asset paths start at the root of the `assets/` directory.

* **Wrong:** `ResourceRegistry::Get("assets/logo.png");`
* **Correct:** `ResourceRegistry::Get("logo.png");`

## 1. Adding Assets

To add a file to the application:

1. Place the file anywhere inside the **`assets/`** directory in the project root.
   * You can use subdirectories (e.g., `assets/textures/logo.png`).
2. Run CMake or compile. The build system automatically detects new files.

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
  * `path`: The relative path to the file inside the `assets/` directory. Use forward slashes (`/`).
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