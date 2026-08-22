#pragma once

#include "Rigel/Persistence/Codecs.h"
#include "Rigel/Persistence/Containers.h"
#include "Rigel/Persistence/RegionLayout.h"
#include "Rigel/Persistence/Types.h"

#include <functional>
#include <optional>
#include <string>

namespace Rigel::Persistence {

struct FormatCapabilities {
    bool supportsEntityRegions = true;
    bool fillMissingChunkSpans = false;
};

struct FormatDescriptor {
    std::string id;
    int version = 1;
    FormatCapabilities capabilities{};
};

struct ProbeResult {
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
