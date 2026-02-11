#pragma once

#include "Rigel/Persistence/Codecs.h"
#include "Rigel/Persistence/Containers.h"
#include "Rigel/Persistence/RegionLayout.h"
#include "Rigel/Persistence/Types.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Rigel::Persistence {

enum class CompressionType {
    None,
    Lz4,
    Custom
};

struct FormatCapabilities {
    CompressionType compression = CompressionType::None;
    bool supportsPartialChunkSave = false;
    bool supportsRandomAccess = false;
    bool supportsEntityRegions = true;
    bool supportsVersions = true;
    bool fillMissingChunkSpans = true;
    std::string metadataFormat = "custom";
    std::string regionIndexType = "int";
};

struct FormatDescriptor {
    std::string id;
    int version = 1;
    std::vector<std::string> extensions;
    FormatCapabilities capabilities{};
};

struct ProbeResult {
    std::string formatId;
    int version = 0;
    float confidence = 0.0f;
};

class PersistenceFormat {
public:
    virtual ~PersistenceFormat() = default;

    virtual const FormatDescriptor& descriptor() const = 0;
    virtual WorldMetadataCodec& worldMetadataCodec() = 0;
    virtual ZoneMetadataCodec& zoneMetadataCodec() = 0;
    virtual ChunkContainer& chunkContainer() = 0;
    virtual EntityContainer& entityContainer() = 0;
    virtual RegionLayout& regionLayout() = 0;
};

using FormatFactory = std::function<std::unique_ptr<PersistenceFormat>(const PersistenceContext&)>;
using FormatProbe = std::function<std::optional<ProbeResult>(StorageBackend&, const PersistenceContext&)>;

} // namespace Rigel::Persistence
