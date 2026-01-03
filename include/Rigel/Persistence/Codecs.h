#pragma once

#include "Rigel/Persistence/Types.h"

#include <string>

namespace Rigel::Persistence {

class ByteReader;
class ByteWriter;

class WorldMetadataCodec {
public:
    virtual ~WorldMetadataCodec() = default;

    virtual std::string metadataPath(const PersistenceContext& context) const = 0;
    virtual void write(const WorldMetadata& metadata, ByteWriter& writer) = 0;
    virtual WorldMetadata read(ByteReader& reader) = 0;
};

class ZoneMetadataCodec {
public:
    virtual ~ZoneMetadataCodec() = default;

    virtual std::string metadataPath(const ZoneKey& key, const PersistenceContext& context) const = 0;
    virtual void write(const ZoneMetadata& metadata, ByteWriter& writer) = 0;
    virtual ZoneMetadata read(ByteReader& reader) = 0;
};

class ChunkCodec {
public:
    virtual ~ChunkCodec() = default;

    virtual void write(const ChunkSnapshot& chunk, ByteWriter& writer) = 0;
    virtual ChunkSnapshot read(ByteReader& reader, const ChunkKey& keyHint) = 0;
};

class EntityRegionCodec {
public:
    virtual ~EntityRegionCodec() = default;

    virtual void write(const EntityRegionSnapshot& region, ByteWriter& writer) = 0;
    virtual EntityRegionSnapshot read(ByteReader& reader, const EntityRegionKey& keyHint) = 0;
};

} // namespace Rigel::Persistence
