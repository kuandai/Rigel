#pragma once

#include "Rigel/Persistence/Storage.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Rigel::Persistence::Backends::CR {

enum class CRSchemaType : uint8_t {
    SchemaEnd = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    Long = 4,
    Float = 5,
    Double = 6,
    Boolean = 7,
    String = 9,
    Object = 10,
    ByteArray = 11,
    ShortArray = 12,
    IntArray = 13,
    LongArray = 14,
    FloatArray = 15,
    DoubleArray = 16,
    BooleanArray = 17,
    StringArray = 18,
    ObjectArray = 19
};

CRSchemaType schemaTypeFromByte(uint8_t value);

struct CRSchemaEntry {
    std::string name;
    CRSchemaType type = CRSchemaType::SchemaEnd;
};

struct CRSchema {
    std::vector<CRSchemaEntry> entries;
};

struct CRBinValue;

struct CRBinObject {
    int32_t schemaIndex = -1;
    std::unordered_map<std::string, CRBinValue> fields;
};

struct CRBinValue {
    using Array = std::vector<CRBinValue>;
    using Variant = std::variant<std::monostate, int64_t, double, float, bool, std::string, Array, CRBinObject>;

    Variant value;

    static CRBinValue fromInt(int64_t v);
    static CRBinValue fromFloat(float v);
    static CRBinValue fromDouble(double v);
    static CRBinValue fromBool(bool v);
    static CRBinValue fromString(std::string v);
    static CRBinValue fromArray(Array v);
    static CRBinValue fromObject(CRBinObject v);
};

struct CRBinDocument {
    CRSchema schema;
    std::vector<CRSchema> altSchemas;
    CRBinObject root;
};

class CRBinReader {
public:
    static CRBinDocument read(ByteReader& reader);
};

class CRBinWriter {
public:
    static void write(ByteWriter& writer, const CRBinDocument& doc);
};

} // namespace Rigel::Persistence::Backends::CR
