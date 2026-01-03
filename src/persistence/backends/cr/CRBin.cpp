#include "Rigel/Persistence/Backends/CR/CRBin.h"

#include <cstring>
#include <stdexcept>

namespace Rigel::Persistence::Backends::CR {

namespace {

int64_t readI64(ByteReader& reader) {
    int64_t high = static_cast<int32_t>(reader.readI32());
    uint32_t low = reader.readU32();
    return (high << 32) | low;
}

void writeI64(ByteWriter& writer, int64_t value) {
    writer.writeI32(static_cast<int32_t>(value >> 32));
    writer.writeU32(static_cast<uint32_t>(value & 0xFFFFFFFF));
}

float readFloat(ByteReader& reader) {
    uint32_t bits = reader.readU32();
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

double readDouble(ByteReader& reader) {
    uint64_t bits = 0;
    bits |= static_cast<uint64_t>(reader.readU32()) << 32;
    bits |= static_cast<uint64_t>(reader.readU32());
    double out = 0.0;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

void writeFloat(ByteWriter& writer, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writer.writeU32(bits);
}

void writeDouble(ByteWriter& writer, double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writer.writeU32(static_cast<uint32_t>((bits >> 32) & 0xFFFFFFFF));
    writer.writeU32(static_cast<uint32_t>(bits & 0xFFFFFFFF));
}

std::string readString(ByteReader& reader) {
    int32_t len = reader.readI32();
    if (len < 0) {
        return std::string();
    }
    std::string out;
    out.resize(static_cast<size_t>(len));
    if (len > 0) {
        reader.readBytes(reinterpret_cast<uint8_t*>(&out[0]), static_cast<size_t>(len));
    }
    return out;
}

void writeString(ByteWriter& writer, const std::string& value) {
    writer.writeI32(static_cast<int32_t>(value.size()));
    if (!value.empty()) {
        writer.writeBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
}

CRSchema readSchema(ByteReader& reader) {
    CRSchema schema;
    while (true) {
        uint8_t typeByte = reader.readU8();
        auto type = schemaTypeFromByte(typeByte);
        if (type == CRSchemaType::SchemaEnd) {
            break;
        }
        CRSchemaEntry entry;
        entry.type = type;
        entry.name = readString(reader);
        schema.entries.push_back(std::move(entry));
    }
    return schema;
}

void writeSchema(ByteWriter& writer, const CRSchema& schema) {
    for (const auto& entry : schema.entries) {
        writer.writeU8(static_cast<uint8_t>(entry.type));
        writeString(writer, entry.name);
    }
    writer.writeU8(static_cast<uint8_t>(CRSchemaType::SchemaEnd));
}

struct StringTable {
    std::unordered_map<std::string, int32_t> indices;
    std::vector<std::string> strings;

    int32_t add(const std::string& value) {
        auto it = indices.find(value);
        if (it != indices.end()) {
            return it->second;
        }
        int32_t id = static_cast<int32_t>(strings.size());
        indices[value] = id;
        strings.push_back(value);
        return id;
    }
};

void collectStrings(StringTable& table, const CRBinValue& value);

void collectStrings(StringTable& table, const CRBinObject& obj) {
    for (const auto& [name, field] : obj.fields) {
        table.add(name);
        collectStrings(table, field);
    }
}

void collectStrings(StringTable& table, const CRBinValue& value) {
    if (std::holds_alternative<std::string>(value.value)) {
        table.add(std::get<std::string>(value.value));
        return;
    }
    if (std::holds_alternative<CRBinValue::Array>(value.value)) {
        for (const auto& item : std::get<CRBinValue::Array>(value.value)) {
            collectStrings(table, item);
        }
        return;
    }
    if (std::holds_alternative<CRBinObject>(value.value)) {
        collectStrings(table, std::get<CRBinObject>(value.value));
        return;
    }
}

void collectSchemaStrings(StringTable& table, const CRSchema& schema) {
    for (const auto& entry : schema.entries) {
        table.add(entry.name);
    }
}

const CRBinValue* findValue(const CRBinObject& obj, const std::string& name) {
    auto it = obj.fields.find(name);
    if (it == obj.fields.end()) {
        return nullptr;
    }
    return &it->second;
}

int64_t toInt(const CRBinValue& value) {
    if (std::holds_alternative<int64_t>(value.value)) {
        return std::get<int64_t>(value.value);
    }
    if (std::holds_alternative<bool>(value.value)) {
        return std::get<bool>(value.value) ? 1 : 0;
    }
    if (std::holds_alternative<float>(value.value)) {
        return static_cast<int64_t>(std::get<float>(value.value));
    }
    if (std::holds_alternative<double>(value.value)) {
        return static_cast<int64_t>(std::get<double>(value.value));
    }
    return 0;
}

float toFloat(const CRBinValue& value) {
    if (std::holds_alternative<float>(value.value)) {
        return std::get<float>(value.value);
    }
    if (std::holds_alternative<double>(value.value)) {
        return static_cast<float>(std::get<double>(value.value));
    }
    if (std::holds_alternative<int64_t>(value.value)) {
        return static_cast<float>(std::get<int64_t>(value.value));
    }
    return 0.0f;
}

double toDouble(const CRBinValue& value) {
    if (std::holds_alternative<double>(value.value)) {
        return std::get<double>(value.value);
    }
    if (std::holds_alternative<float>(value.value)) {
        return static_cast<double>(std::get<float>(value.value));
    }
    if (std::holds_alternative<int64_t>(value.value)) {
        return static_cast<double>(std::get<int64_t>(value.value));
    }
    return 0.0;
}

bool toBool(const CRBinValue& value) {
    if (std::holds_alternative<bool>(value.value)) {
        return std::get<bool>(value.value);
    }
    if (std::holds_alternative<int64_t>(value.value)) {
        return std::get<int64_t>(value.value) != 0;
    }
    return false;
}

void writeValue(ByteWriter& writer, StringTable& table, const CRBinValue& value, CRSchemaType type,
    const std::vector<CRSchema>& altSchemas);

CRBinValue readValue(ByteReader& reader, CRSchemaType type, const std::vector<std::string>& strings,
    const std::vector<CRSchema>& altSchemas) {
    switch (type) {
    case CRSchemaType::Byte:
        return CRBinValue::fromInt(static_cast<int8_t>(reader.readU8()));
    case CRSchemaType::Short:
        return CRBinValue::fromInt(static_cast<int16_t>(reader.readU16()));
    case CRSchemaType::Int:
        return CRBinValue::fromInt(reader.readI32());
    case CRSchemaType::Long:
        return CRBinValue::fromInt(readI64(reader));
    case CRSchemaType::Float:
        return CRBinValue::fromFloat(readFloat(reader));
    case CRSchemaType::Double:
        return CRBinValue::fromDouble(readDouble(reader));
    case CRSchemaType::Boolean:
        return CRBinValue::fromBool(reader.readU8() != 0);
    case CRSchemaType::String: {
        int32_t id = reader.readI32();
        if (id < 0 || id >= static_cast<int32_t>(strings.size())) {
            return CRBinValue::fromString(std::string());
        }
        return CRBinValue::fromString(strings[static_cast<size_t>(id)]);
    }
    case CRSchemaType::Object: {
        int32_t schemaIndex = reader.readI32();
        if (schemaIndex < 0 || schemaIndex >= static_cast<int32_t>(altSchemas.size())) {
            return CRBinValue{};
        }
        CRBinObject obj;
        obj.schemaIndex = schemaIndex;
        const auto& schema = altSchemas[static_cast<size_t>(schemaIndex)];
        for (const auto& entry : schema.entries) {
            obj.fields[entry.name] = readValue(reader, entry.type, strings, altSchemas);
        }
        return CRBinValue::fromObject(std::move(obj));
    }
    case CRSchemaType::ByteArray:
    case CRSchemaType::ShortArray:
    case CRSchemaType::IntArray:
    case CRSchemaType::LongArray:
    case CRSchemaType::FloatArray:
    case CRSchemaType::DoubleArray:
    case CRSchemaType::BooleanArray:
    case CRSchemaType::StringArray:
    case CRSchemaType::ObjectArray: {
        int32_t length = reader.readI32();
        if (length < 0) {
            return CRBinValue{};
        }
        CRBinValue::Array array;
        array.reserve(static_cast<size_t>(length));
        for (int32_t i = 0; i < length; ++i) {
            if (type == CRSchemaType::ByteArray) {
                array.push_back(CRBinValue::fromInt(static_cast<int8_t>(reader.readU8())));
            } else if (type == CRSchemaType::ShortArray) {
                array.push_back(CRBinValue::fromInt(static_cast<int16_t>(reader.readU16())));
            } else if (type == CRSchemaType::IntArray) {
                array.push_back(CRBinValue::fromInt(reader.readI32()));
            } else if (type == CRSchemaType::LongArray) {
                array.push_back(CRBinValue::fromInt(readI64(reader)));
            } else if (type == CRSchemaType::FloatArray) {
                array.push_back(CRBinValue::fromFloat(readFloat(reader)));
            } else if (type == CRSchemaType::DoubleArray) {
                array.push_back(CRBinValue::fromDouble(readDouble(reader)));
            } else if (type == CRSchemaType::BooleanArray) {
                array.push_back(CRBinValue::fromBool(reader.readU8() != 0));
            } else if (type == CRSchemaType::StringArray) {
                int32_t id = reader.readI32();
                if (id >= 0 && id < static_cast<int32_t>(strings.size())) {
                    array.push_back(CRBinValue::fromString(strings[static_cast<size_t>(id)]));
                } else {
                    array.push_back(CRBinValue::fromString(std::string()));
                }
            } else if (type == CRSchemaType::ObjectArray) {
                CRBinValue objValue = readValue(reader, CRSchemaType::Object, strings, altSchemas);
                array.push_back(std::move(objValue));
            } else {
                throw std::runtime_error("CRBinReader: unexpected array type");
            }
        }
        return CRBinValue::fromArray(std::move(array));
    }
    default:
        throw std::runtime_error("CRBinReader: unknown schema type");
    }
}

void writeArray(ByteWriter& writer, StringTable& table, const CRBinValue& value, CRSchemaType type,
    const std::vector<CRSchema>& altSchemas) {
    if (!std::holds_alternative<CRBinValue::Array>(value.value)) {
        writer.writeI32(-1);
        return;
    }
    const auto& array = std::get<CRBinValue::Array>(value.value);
    writer.writeI32(static_cast<int32_t>(array.size()));
    for (const auto& item : array) {
        switch (type) {
        case CRSchemaType::ByteArray:
            writer.writeU8(static_cast<uint8_t>(toInt(item)));
            break;
        case CRSchemaType::ShortArray:
            writer.writeU16(static_cast<uint16_t>(toInt(item)));
            break;
        case CRSchemaType::IntArray:
            writer.writeI32(static_cast<int32_t>(toInt(item)));
            break;
        case CRSchemaType::LongArray:
            writeI64(writer, toInt(item));
            break;
        case CRSchemaType::FloatArray:
            writeFloat(writer, toFloat(item));
            break;
        case CRSchemaType::DoubleArray:
            writeDouble(writer, toDouble(item));
            break;
        case CRSchemaType::BooleanArray:
            writer.writeU8(toBool(item) ? 1 : 0);
            break;
        case CRSchemaType::StringArray: {
            if (std::holds_alternative<std::string>(item.value)) {
                writer.writeI32(table.add(std::get<std::string>(item.value)));
            } else {
                writer.writeI32(-1);
            }
            break;
        }
        case CRSchemaType::ObjectArray:
            writeValue(writer, table, item, CRSchemaType::Object, altSchemas);
            break;
        default:
            throw std::runtime_error("CRBinWriter: unexpected array type");
        }
    }
}

void writeValue(ByteWriter& writer, StringTable& table, const CRBinValue& value, CRSchemaType type,
    const std::vector<CRSchema>& altSchemas) {
    switch (type) {
    case CRSchemaType::Byte:
        writer.writeU8(static_cast<uint8_t>(toInt(value)));
        break;
    case CRSchemaType::Short:
        writer.writeU16(static_cast<uint16_t>(toInt(value)));
        break;
    case CRSchemaType::Int:
        writer.writeI32(static_cast<int32_t>(toInt(value)));
        break;
    case CRSchemaType::Long:
        writeI64(writer, toInt(value));
        break;
    case CRSchemaType::Float:
        writeFloat(writer, toFloat(value));
        break;
    case CRSchemaType::Double:
        writeDouble(writer, toDouble(value));
        break;
    case CRSchemaType::Boolean:
        writer.writeU8(toBool(value) ? 1 : 0);
        break;
    case CRSchemaType::String:
        if (std::holds_alternative<std::string>(value.value)) {
            writer.writeI32(table.add(std::get<std::string>(value.value)));
        } else {
            writer.writeI32(-1);
        }
        break;
    case CRSchemaType::Object: {
        if (!std::holds_alternative<CRBinObject>(value.value)) {
            writer.writeI32(-1);
            break;
        }
        const auto& obj = std::get<CRBinObject>(value.value);
        if (obj.schemaIndex < 0 || obj.schemaIndex >= static_cast<int32_t>(altSchemas.size())) {
            writer.writeI32(-1);
            break;
        }
        writer.writeI32(obj.schemaIndex);
        const auto& schema = altSchemas[static_cast<size_t>(obj.schemaIndex)];
        for (const auto& entry : schema.entries) {
            const CRBinValue* field = findValue(obj, entry.name);
            if (field) {
                writeValue(writer, table, *field, entry.type, altSchemas);
            } else {
                writeValue(writer, table, CRBinValue{}, entry.type, altSchemas);
            }
        }
        break;
    }
    case CRSchemaType::ByteArray:
    case CRSchemaType::ShortArray:
    case CRSchemaType::IntArray:
    case CRSchemaType::LongArray:
    case CRSchemaType::FloatArray:
    case CRSchemaType::DoubleArray:
    case CRSchemaType::BooleanArray:
    case CRSchemaType::StringArray:
    case CRSchemaType::ObjectArray:
        writeArray(writer, table, value, type, altSchemas);
        break;
    default:
        throw std::runtime_error("CRBinWriter: unknown schema type");
    }
}

} // namespace

CRSchemaType schemaTypeFromByte(uint8_t value) {
    switch (value) {
    case 0:
        return CRSchemaType::SchemaEnd;
    case 1:
        return CRSchemaType::Byte;
    case 2:
        return CRSchemaType::Short;
    case 3:
        return CRSchemaType::Int;
    case 4:
        return CRSchemaType::Long;
    case 5:
        return CRSchemaType::Float;
    case 6:
        return CRSchemaType::Double;
    case 7:
        return CRSchemaType::Boolean;
    case 9:
        return CRSchemaType::String;
    case 10:
        return CRSchemaType::Object;
    case 11:
        return CRSchemaType::ByteArray;
    case 12:
        return CRSchemaType::ShortArray;
    case 13:
        return CRSchemaType::IntArray;
    case 14:
        return CRSchemaType::LongArray;
    case 15:
        return CRSchemaType::FloatArray;
    case 16:
        return CRSchemaType::DoubleArray;
    case 17:
        return CRSchemaType::BooleanArray;
    case 18:
        return CRSchemaType::StringArray;
    case 19:
        return CRSchemaType::ObjectArray;
    default:
        throw std::runtime_error("CRBin: unknown schema type byte");
    }
}

CRBinValue CRBinValue::fromInt(int64_t v) {
    CRBinValue out;
    out.value = v;
    return out;
}

CRBinValue CRBinValue::fromFloat(float v) {
    CRBinValue out;
    out.value = v;
    return out;
}

CRBinValue CRBinValue::fromDouble(double v) {
    CRBinValue out;
    out.value = v;
    return out;
}

CRBinValue CRBinValue::fromBool(bool v) {
    CRBinValue out;
    out.value = v;
    return out;
}

CRBinValue CRBinValue::fromString(std::string v) {
    CRBinValue out;
    out.value = std::move(v);
    return out;
}

CRBinValue CRBinValue::fromArray(Array v) {
    CRBinValue out;
    out.value = std::move(v);
    return out;
}

CRBinValue CRBinValue::fromObject(CRBinObject v) {
    CRBinValue out;
    out.value = std::move(v);
    return out;
}

CRBinDocument CRBinReader::read(ByteReader& reader) {
    CRBinDocument doc;

    int32_t numStrings = reader.readI32();
    if (numStrings < 0) {
        throw std::runtime_error("CRBinReader: invalid string table size");
    }
    std::vector<std::string> strings;
    strings.reserve(static_cast<size_t>(numStrings));
    for (int32_t i = 0; i < numStrings; ++i) {
        strings.push_back(readString(reader));
    }

    doc.schema = readSchema(reader);

    int32_t numAltSchemas = reader.readI32();
    if (numAltSchemas < 0) {
        throw std::runtime_error("CRBinReader: invalid alt schema count");
    }
    doc.altSchemas.reserve(static_cast<size_t>(numAltSchemas));
    for (int32_t i = 0; i < numAltSchemas; ++i) {
        doc.altSchemas.push_back(readSchema(reader));
    }

    doc.root.schemaIndex = -1;
    for (const auto& entry : doc.schema.entries) {
        doc.root.fields[entry.name] = readValue(reader, entry.type, strings, doc.altSchemas);
    }

    return doc;
}

void CRBinWriter::write(ByteWriter& writer, const CRBinDocument& doc) {
    StringTable table;
    collectSchemaStrings(table, doc.schema);
    for (const auto& schema : doc.altSchemas) {
        collectSchemaStrings(table, schema);
    }
    collectStrings(table, doc.root);

    writer.writeI32(static_cast<int32_t>(table.strings.size()));
    for (const auto& value : table.strings) {
        writeString(writer, value);
    }

    writeSchema(writer, doc.schema);

    writer.writeI32(static_cast<int32_t>(doc.altSchemas.size()));
    for (const auto& schema : doc.altSchemas) {
        writeSchema(writer, schema);
    }

    for (const auto& entry : doc.schema.entries) {
        const CRBinValue* value = findValue(doc.root, entry.name);
        if (value) {
            writeValue(writer, table, *value, entry.type, doc.altSchemas);
        } else {
            writeValue(writer, table, CRBinValue{}, entry.type, doc.altSchemas);
        }
    }
}

} // namespace Rigel::Persistence::Backends::CR
