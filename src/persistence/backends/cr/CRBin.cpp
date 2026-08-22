#include "Rigel/Persistence/Backends/CR/CRBin.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace Rigel::Persistence::Backends::CR {

namespace {

constexpr size_t kMaxDocumentBytes = 64 * 1024 * 1024;
constexpr size_t kMaxStringBytes = 1024 * 1024;
constexpr size_t kMaxStringTableEntries = 65'536;
constexpr size_t kMaxSchemaEntries = 4'096;
constexpr size_t kMaxTotalSchemaEntries = 65'536;
constexpr size_t kMaxAlternateSchemas = 4'096;
constexpr size_t kMaxArrayEntries = 1'048'576;
constexpr size_t kMaxTotalValues = 1'048'576;
constexpr size_t kMaxNestingDepth = 64;

size_t remainingInput(const ByteReader& reader) {
    const size_t position = reader.tell();
    const size_t size = reader.size();
    if (position > size) {
        throw std::runtime_error("CRBinReader: invalid reader position");
    }
    return size - position;
}

void requireRemaining(const ByteReader& reader,
                      size_t required,
                      const char* diagnostic) {
    if (required > remainingInput(reader)) {
        throw std::runtime_error(diagnostic);
    }
}

class BufferWriter final : public ByteWriter {
public:
    void writeU8(uint8_t value) override {
        writeBytes(&value, sizeof(value));
    }

    void writeU16(uint16_t value) override {
        const uint8_t bytes[] = {
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)};
        writeBytes(bytes, sizeof(bytes));
    }

    void writeU32(uint32_t value) override {
        const uint8_t bytes[] = {
            static_cast<uint8_t>((value >> 24) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)};
        writeBytes(bytes, sizeof(bytes));
    }

    void writeI32(int32_t value) override {
        writeU32(static_cast<uint32_t>(value));
    }

    void writeBytes(const uint8_t* src, size_t len) override {
        requireRange(m_pos, len);
        if (m_pos + len > m_data.size()) {
            m_data.resize(m_pos + len);
        }
        if (len != 0) {
            std::memcpy(m_data.data() + m_pos, src, len);
        }
        m_pos += len;
    }

    size_t size() const override {
        return m_data.size();
    }

    size_t tell() const override {
        return m_pos;
    }

    void seek(size_t offset) override {
        requireRange(offset, 0);
        if (offset > m_data.size()) {
            m_data.resize(offset);
        }
        m_pos = offset;
    }

    void writeAt(size_t offset, const uint8_t* src, size_t len) override {
        requireRange(offset, len);
        if (offset + len > m_data.size()) {
            m_data.resize(offset + len);
        }
        if (len != 0) {
            std::memcpy(m_data.data() + offset, src, len);
        }
    }

    void flush() override {
    }

    const std::vector<uint8_t>& data() const {
        return m_data;
    }

private:
    void requireRange(size_t offset, size_t len) const {
        if (offset > kMaxDocumentBytes || len > kMaxDocumentBytes - offset) {
            throw std::runtime_error("CRBinWriter: document exceeds format limit");
        }
    }

    std::vector<uint8_t> m_data;
    size_t m_pos = 0;
};

int64_t readI64(ByteReader& reader) {
    const uint64_t high = static_cast<uint64_t>(reader.readU32());
    const uint64_t low = static_cast<uint64_t>(reader.readU32());
    return std::bit_cast<int64_t>((high << 32) | low);
}

void writeI64(ByteWriter& writer, int64_t value) {
    const uint64_t bits = std::bit_cast<uint64_t>(value);
    writer.writeU32(static_cast<uint32_t>(bits >> 32));
    writer.writeU32(static_cast<uint32_t>(bits & 0xFFFFFFFF));
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
    requireRemaining(
        reader, sizeof(int32_t),
        "CRBinReader: truncated string length");
    const int32_t len = reader.readI32();
    if (len < 0) {
        throw std::runtime_error("CRBinReader: invalid string length");
    }
    if (static_cast<size_t>(len) > kMaxStringBytes) {
        throw std::runtime_error("CRBinReader: string length exceeds format limit");
    }
    if (static_cast<size_t>(len) > remainingInput(reader)) {
        throw std::runtime_error("CRBinReader: string length exceeds remaining input");
    }
    std::string out;
    out.resize(static_cast<size_t>(len));
    if (len > 0) {
        reader.readBytes(reinterpret_cast<uint8_t*>(&out[0]), static_cast<size_t>(len));
    }
    return out;
}

void writeString(ByteWriter& writer, const std::string& value) {
    if (value.size() > kMaxStringBytes) {
        throw std::runtime_error("CRBinWriter: string length exceeds format limit");
    }
    writer.writeI32(static_cast<int32_t>(value.size()));
    if (!value.empty()) {
        writer.writeBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
}

CRSchema readSchema(ByteReader& reader, size_t& totalEntries) {
    CRSchema schema;
    std::unordered_set<std::string> names;
    while (true) {
        requireRemaining(
            reader, sizeof(uint8_t),
            "CRBinReader: schema exceeds remaining input");
        uint8_t typeByte = reader.readU8();
        auto type = schemaTypeFromByte(typeByte);
        if (type == CRSchemaType::SchemaEnd) {
            break;
        }
        if (schema.entries.size() >= kMaxSchemaEntries ||
            totalEntries >= kMaxTotalSchemaEntries) {
            throw std::runtime_error(
                "CRBinReader: schema entry count exceeds format limit");
        }
        CRSchemaEntry entry;
        entry.type = type;
        entry.name = readString(reader);
        if (!names.insert(entry.name).second) {
            throw std::runtime_error("CRBinReader: duplicate schema field");
        }
        schema.entries.push_back(std::move(entry));
        ++totalEntries;
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
        if (value.size() > kMaxStringBytes) {
            throw std::runtime_error("CRBinWriter: string length exceeds format limit");
        }
        if (strings.size() >= kMaxStringTableEntries) {
            throw std::runtime_error(
                "CRBinWriter: string table size exceeds format limit");
        }
        int32_t id = static_cast<int32_t>(strings.size());
        indices[value] = id;
        strings.push_back(value);
        return id;
    }

    int32_t indexOf(const std::string& value) const {
        auto it = indices.find(value);
        if (it == indices.end()) {
            throw std::runtime_error("CRBinWriter: missing string table entry");
        }
        return it->second;
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

bool isValueSchemaType(CRSchemaType type) {
    switch (type) {
    case CRSchemaType::Byte:
    case CRSchemaType::Short:
    case CRSchemaType::Int:
    case CRSchemaType::Long:
    case CRSchemaType::Float:
    case CRSchemaType::Double:
    case CRSchemaType::Boolean:
    case CRSchemaType::String:
    case CRSchemaType::Object:
    case CRSchemaType::ByteArray:
    case CRSchemaType::ShortArray:
    case CRSchemaType::IntArray:
    case CRSchemaType::LongArray:
    case CRSchemaType::FloatArray:
    case CRSchemaType::DoubleArray:
    case CRSchemaType::BooleanArray:
    case CRSchemaType::StringArray:
    case CRSchemaType::ObjectArray:
        return true;
    case CRSchemaType::SchemaEnd:
        return false;
    }
    return false;
}

void validateSchemaForWrite(const CRSchema& schema, size_t& totalEntries) {
    if (schema.entries.size() > kMaxSchemaEntries ||
        schema.entries.size() > kMaxTotalSchemaEntries - totalEntries) {
        throw std::runtime_error(
            "CRBinWriter: schema entry count exceeds format limit");
    }
    std::unordered_set<std::string> names;
    for (const auto& entry : schema.entries) {
        if (!isValueSchemaType(entry.type)) {
            throw std::runtime_error("CRBinWriter: invalid schema type");
        }
        if (entry.name.size() > kMaxStringBytes) {
            throw std::runtime_error("CRBinWriter: string length exceeds format limit");
        }
        if (!names.insert(entry.name).second) {
            throw std::runtime_error("CRBinWriter: duplicate schema field");
        }
    }
    totalEntries += schema.entries.size();
}

const CRBinValue* findValue(const CRBinObject& obj, const std::string& name) {
    auto it = obj.fields.find(name);
    if (it == obj.fields.end()) {
        return nullptr;
    }
    return &it->second;
}

void validateValueForWrite(const CRBinValue& value,
                           const std::vector<CRSchema>& altSchemas,
                           size_t depth,
                           size_t& totalValues) {
    if (++totalValues > kMaxTotalValues) {
        throw std::runtime_error("CRBinWriter: value count exceeds format limit");
    }
    if (depth > kMaxNestingDepth) {
        throw std::runtime_error("CRBinWriter: nesting depth exceeds format limit");
    }
    if (std::holds_alternative<std::string>(value.value)) {
        if (std::get<std::string>(value.value).size() > kMaxStringBytes) {
            throw std::runtime_error("CRBinWriter: string length exceeds format limit");
        }
        return;
    }
    if (std::holds_alternative<CRBinValue::Array>(value.value)) {
        const auto& array = std::get<CRBinValue::Array>(value.value);
        if (array.size() > kMaxArrayEntries) {
            throw std::runtime_error("CRBinWriter: array length exceeds format limit");
        }
        for (const auto& item : array) {
            validateValueForWrite(item, altSchemas, depth + 1, totalValues);
        }
        return;
    }
    if (std::holds_alternative<CRBinObject>(value.value)) {
        const auto& object = std::get<CRBinObject>(value.value);
        if (object.schemaIndex < 0 ||
            object.schemaIndex >= static_cast<int32_t>(altSchemas.size())) {
            throw std::runtime_error("CRBinWriter: schema reference out of range");
        }
        const auto& schema =
            altSchemas[static_cast<size_t>(object.schemaIndex)];
        for (const auto& entry : schema.entries) {
            const CRBinValue* field = findValue(object, entry.name);
            if (field) {
                validateValueForWrite(
                    *field, altSchemas, depth + 1, totalValues);
            } else {
                validateValueForWrite(
                    CRBinValue{}, altSchemas, depth + 1, totalValues);
            }
        }
    }
}

void validateDocumentForWrite(const CRBinDocument& doc) {
    if (doc.altSchemas.size() > kMaxAlternateSchemas) {
        throw std::runtime_error(
            "CRBinWriter: alternate schema count exceeds format limit");
    }
    size_t totalEntries = 0;
    validateSchemaForWrite(doc.schema, totalEntries);
    for (const auto& schema : doc.altSchemas) {
        validateSchemaForWrite(schema, totalEntries);
    }

    size_t totalValues = 0;
    for (const auto& entry : doc.schema.entries) {
        const CRBinValue* value = findValue(doc.root, entry.name);
        if (value) {
            validateValueForWrite(*value, doc.altSchemas, 0, totalValues);
        } else {
            validateValueForWrite(
                CRBinValue{}, doc.altSchemas, 0, totalValues);
        }
    }
}

int64_t toInt(const CRBinValue& value) {
    if (std::holds_alternative<int64_t>(value.value)) {
        return std::get<int64_t>(value.value);
    }
    if (std::holds_alternative<bool>(value.value)) {
        return std::get<bool>(value.value) ? 1 : 0;
    }
    if (std::holds_alternative<float>(value.value)) {
        const float input = std::get<float>(value.value);
        if (!std::isfinite(input) ||
            static_cast<long double>(input) <
                static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
            static_cast<long double>(input) >
                static_cast<long double>(std::numeric_limits<int64_t>::max())) {
            throw std::runtime_error("CRBinWriter: integer value out of range");
        }
        return static_cast<int64_t>(input);
    }
    if (std::holds_alternative<double>(value.value)) {
        const double input = std::get<double>(value.value);
        if (!std::isfinite(input) ||
            static_cast<long double>(input) <
                static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
            static_cast<long double>(input) >
                static_cast<long double>(std::numeric_limits<int64_t>::max())) {
            throw std::runtime_error("CRBinWriter: integer value out of range");
        }
        return static_cast<int64_t>(input);
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

size_t arrayElementBytes(CRSchemaType type) {
    switch (type) {
    case CRSchemaType::ByteArray:
    case CRSchemaType::BooleanArray:
        return 1;
    case CRSchemaType::ShortArray:
        return 2;
    case CRSchemaType::IntArray:
    case CRSchemaType::FloatArray:
    case CRSchemaType::StringArray:
    case CRSchemaType::ObjectArray:
        return 4;
    case CRSchemaType::LongArray:
    case CRSchemaType::DoubleArray:
        return 8;
    default:
        throw std::runtime_error("CRBinReader: unexpected array type");
    }
}

CRBinValue readStringReference(ByteReader& reader,
                               const std::vector<std::string>& strings) {
    requireRemaining(
        reader, sizeof(int32_t),
        "CRBinReader: truncated string reference");
    const int32_t id = reader.readI32();
    if (id == -1) {
        return CRBinValue{};
    }
    if (id < -1) {
        throw std::runtime_error(
            "CRBinReader: string reference is below null sentinel");
    }
    if (id >= static_cast<int32_t>(strings.size())) {
        throw std::runtime_error("CRBinReader: string reference out of range");
    }
    return CRBinValue::fromString(strings[static_cast<size_t>(id)]);
}

CRBinValue readValue(ByteReader& reader, CRSchemaType type, const std::vector<std::string>& strings,
    const std::vector<CRSchema>& altSchemas, size_t depth, size_t& totalValues) {
    if (depth > kMaxNestingDepth) {
        throw std::runtime_error("CRBinReader: nesting depth exceeds format limit");
    }
    if (++totalValues > kMaxTotalValues) {
        throw std::runtime_error("CRBinReader: value count exceeds format limit");
    }
    switch (type) {
    case CRSchemaType::Byte:
        requireRemaining(reader, 1, "CRBinReader: truncated value");
        return CRBinValue::fromInt(static_cast<int8_t>(reader.readU8()));
    case CRSchemaType::Short:
        requireRemaining(reader, 2, "CRBinReader: truncated value");
        return CRBinValue::fromInt(static_cast<int16_t>(reader.readU16()));
    case CRSchemaType::Int:
        requireRemaining(reader, 4, "CRBinReader: truncated value");
        return CRBinValue::fromInt(reader.readI32());
    case CRSchemaType::Long:
        requireRemaining(reader, 8, "CRBinReader: truncated value");
        return CRBinValue::fromInt(readI64(reader));
    case CRSchemaType::Float:
        requireRemaining(reader, 4, "CRBinReader: truncated value");
        return CRBinValue::fromFloat(readFloat(reader));
    case CRSchemaType::Double:
        requireRemaining(reader, 8, "CRBinReader: truncated value");
        return CRBinValue::fromDouble(readDouble(reader));
    case CRSchemaType::Boolean:
        requireRemaining(reader, 1, "CRBinReader: truncated value");
        return CRBinValue::fromBool(reader.readU8() != 0);
    case CRSchemaType::String:
        return readStringReference(reader, strings);
    case CRSchemaType::Object: {
        requireRemaining(
            reader, sizeof(int32_t),
            "CRBinReader: truncated schema reference");
        const int32_t schemaIndex = reader.readI32();
        if (schemaIndex == -1) {
            return CRBinValue{};
        }
        if (schemaIndex < -1) {
            throw std::runtime_error(
                "CRBinReader: schema reference is below null sentinel");
        }
        if (schemaIndex >= static_cast<int32_t>(altSchemas.size())) {
            throw std::runtime_error("CRBinReader: schema reference out of range");
        }
        CRBinObject obj;
        obj.schemaIndex = schemaIndex;
        const auto& schema = altSchemas[static_cast<size_t>(schemaIndex)];
        for (const auto& entry : schema.entries) {
            obj.fields[entry.name] = readValue(
                reader, entry.type, strings, altSchemas, depth + 1, totalValues);
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
        requireRemaining(
            reader, sizeof(int32_t),
            "CRBinReader: truncated array length");
        const int32_t length = reader.readI32();
        if (length == -1) {
            return CRBinValue{};
        }
        if (length < -1) {
            throw std::runtime_error(
                "CRBinReader: array length is below null sentinel");
        }
        if (static_cast<size_t>(length) > kMaxArrayEntries) {
            throw std::runtime_error("CRBinReader: array length exceeds format limit");
        }
        const size_t elementBytes = arrayElementBytes(type);
        if (static_cast<size_t>(length) > remainingInput(reader) / elementBytes) {
            throw std::runtime_error("CRBinReader: array exceeds remaining input");
        }
        if (type != CRSchemaType::ObjectArray) {
            if (static_cast<size_t>(length) > kMaxTotalValues - totalValues) {
                throw std::runtime_error("CRBinReader: value count exceeds format limit");
            }
            totalValues += static_cast<size_t>(length);
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
                array.push_back(readStringReference(reader, strings));
            } else if (type == CRSchemaType::ObjectArray) {
                CRBinValue objValue = readValue(
                    reader, CRSchemaType::Object, strings, altSchemas,
                    depth + 1, totalValues);
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
        if (std::holds_alternative<std::monostate>(value.value)) {
            writer.writeI32(-1);
            return;
        }
        throw std::runtime_error("CRBinWriter: array value type mismatch");
    }
    const auto& array = std::get<CRBinValue::Array>(value.value);
    if (array.size() > kMaxArrayEntries) {
        throw std::runtime_error("CRBinWriter: array length exceeds format limit");
    }
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
                writer.writeI32(table.indexOf(std::get<std::string>(item.value)));
            } else if (std::holds_alternative<std::monostate>(item.value)) {
                writer.writeI32(-1);
            } else {
                throw std::runtime_error("CRBinWriter: string value type mismatch");
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
            writer.writeI32(table.indexOf(std::get<std::string>(value.value)));
        } else if (std::holds_alternative<std::monostate>(value.value)) {
            writer.writeI32(-1);
        } else {
            throw std::runtime_error("CRBinWriter: string value type mismatch");
        }
        break;
    case CRSchemaType::Object: {
        if (!std::holds_alternative<CRBinObject>(value.value)) {
            if (std::holds_alternative<std::monostate>(value.value)) {
                writer.writeI32(-1);
                break;
            }
            throw std::runtime_error("CRBinWriter: object value type mismatch");
        }
        const auto& obj = std::get<CRBinObject>(value.value);
        if (obj.schemaIndex < 0 || obj.schemaIndex >= static_cast<int32_t>(altSchemas.size())) {
            throw std::runtime_error("CRBinWriter: schema reference out of range");
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
    if (remainingInput(reader) > kMaxDocumentBytes) {
        throw std::runtime_error("CRBinReader: document exceeds format limit");
    }
    CRBinDocument doc;

    requireRemaining(
        reader, sizeof(int32_t),
        "CRBinReader: truncated string table size");
    const int32_t numStrings = reader.readI32();
    if (numStrings < 0) {
        throw std::runtime_error("CRBinReader: invalid string table size");
    }
    if (static_cast<size_t>(numStrings) > kMaxStringTableEntries) {
        throw std::runtime_error(
            "CRBinReader: string table size exceeds format limit");
    }
    if (static_cast<size_t>(numStrings) >
        remainingInput(reader) / sizeof(int32_t)) {
        throw std::runtime_error(
            "CRBinReader: string table exceeds remaining input");
    }
    std::vector<std::string> strings;
    strings.reserve(static_cast<size_t>(numStrings));
    for (int32_t i = 0; i < numStrings; ++i) {
        strings.push_back(readString(reader));
    }

    size_t totalSchemaEntries = 0;
    doc.schema = readSchema(reader, totalSchemaEntries);

    requireRemaining(
        reader, sizeof(int32_t),
        "CRBinReader: truncated alternate schema count");
    const int32_t numAltSchemas = reader.readI32();
    if (numAltSchemas < 0) {
        throw std::runtime_error("CRBinReader: invalid alt schema count");
    }
    if (static_cast<size_t>(numAltSchemas) > kMaxAlternateSchemas) {
        throw std::runtime_error(
            "CRBinReader: alternate schema count exceeds format limit");
    }
    if (static_cast<size_t>(numAltSchemas) > remainingInput(reader)) {
        throw std::runtime_error(
            "CRBinReader: alternate schema table exceeds remaining input");
    }
    doc.altSchemas.reserve(static_cast<size_t>(numAltSchemas));
    for (int32_t i = 0; i < numAltSchemas; ++i) {
        doc.altSchemas.push_back(readSchema(reader, totalSchemaEntries));
    }

    doc.root.schemaIndex = -1;
    size_t totalValues = 0;
    for (const auto& entry : doc.schema.entries) {
        doc.root.fields[entry.name] = readValue(
            reader, entry.type, strings, doc.altSchemas, 0, totalValues);
    }
    if (reader.tell() != reader.size()) {
        throw std::runtime_error("CRBinReader: trailing data");
    }

    return doc;
}

void CRBinWriter::write(ByteWriter& writer, const CRBinDocument& doc) {
    validateDocumentForWrite(doc);

    StringTable table;
    collectSchemaStrings(table, doc.schema);
    for (const auto& schema : doc.altSchemas) {
        collectSchemaStrings(table, schema);
    }
    collectStrings(table, doc.root);

    BufferWriter buffer;
    buffer.writeI32(static_cast<int32_t>(table.strings.size()));
    for (const auto& value : table.strings) {
        writeString(buffer, value);
    }

    writeSchema(buffer, doc.schema);

    buffer.writeI32(static_cast<int32_t>(doc.altSchemas.size()));
    for (const auto& schema : doc.altSchemas) {
        writeSchema(buffer, schema);
    }

    for (const auto& entry : doc.schema.entries) {
        const CRBinValue* value = findValue(doc.root, entry.name);
        if (value) {
            writeValue(buffer, table, *value, entry.type, doc.altSchemas);
        } else {
            writeValue(buffer, table, CRBinValue{}, entry.type, doc.altSchemas);
        }
    }

    if (!buffer.data().empty()) {
        writer.writeBytes(buffer.data().data(), buffer.data().size());
    }
}

} // namespace Rigel::Persistence::Backends::CR
