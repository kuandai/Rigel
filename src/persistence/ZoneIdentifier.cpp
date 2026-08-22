#include "ZoneIdentifier.h"

#include <stdexcept>
#include <string>

namespace Rigel::Persistence::detail {

namespace {

constexpr size_t kMaximumSegmentLength = 64;

[[noreturn]] void throwIdentifierError(std::string_view zoneId,
                                       const char* reason) {
    throw std::runtime_error(
        "Persistence configuration error: zone identifier '" +
        std::string(zoneId) + "' " + reason +
        "; expected one or two lowercase ASCII name segments separated by "
        "a single ':', with each segment starting and ending in a letter or "
        "digit and containing only letters, digits, '.', '_', or '-'; "
        "namespace-local segments cannot use persistence storage names");
}

bool isLowercaseLetterOrDigit(char value) {
    return (value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9');
}

bool isSegmentCharacter(char value) {
    return isLowercaseLetterOrDigit(value) ||
        value == '.' || value == '_' || value == '-';
}

bool isReservedPlatformName(std::string_view segment) {
    const auto extension = segment.find('.');
    const std::string_view base = segment.substr(0, extension);
    if (base == "con" || base == "prn" || base == "aux" || base == "nul") {
        return true;
    }
    if (base.size() == 4 &&
        (base.substr(0, 3) == "com" || base.substr(0, 3) == "lpt") &&
        base[3] >= '1' && base[3] <= '9') {
        return true;
    }
    return false;
}

bool isBackendStorageChild(std::string_view segment) {
    return segment == "regions" ||
        segment == "entities" ||
        segment == "chunks" ||
        segment == "zone.meta" ||
        segment == "zoneinfo.json";
}

void validateSegment(std::string_view zoneId, std::string_view segment) {
    if (segment.empty()) {
        throwIdentifierError(zoneId, "contains an empty name segment");
    }
    if (segment.size() > kMaximumSegmentLength) {
        throwIdentifierError(
            zoneId, "contains a name segment longer than 64 characters");
    }
    if (!isLowercaseLetterOrDigit(segment.front()) ||
        !isLowercaseLetterOrDigit(segment.back())) {
        throwIdentifierError(zoneId, "contains a non-canonical name segment");
    }
    for (const char value : segment) {
        if (!isSegmentCharacter(value)) {
            throwIdentifierError(
                zoneId, "contains a path separator or unsupported character");
        }
    }
    if (isReservedPlatformName(segment)) {
        throwIdentifierError(zoneId, "contains a reserved platform name");
    }
}

} // namespace

void validateZoneIdentifier(std::string_view zoneId) {
    const auto separator = zoneId.find(':');
    if (separator == std::string_view::npos) {
        validateSegment(zoneId, zoneId);
        return;
    }
    if (zoneId.find(':', separator + 1) != std::string_view::npos) {
        throwIdentifierError(
            zoneId, "contains more than one namespace separator");
    }
    if (separator == 1 && zoneId.front() >= 'a' && zoneId.front() <= 'z') {
        throwIdentifierError(zoneId, "resembles a drive-relative path");
    }
    validateSegment(zoneId, zoneId.substr(0, separator));
    const auto localName = zoneId.substr(separator + 1);
    validateSegment(zoneId, localName);
    if (isBackendStorageChild(localName)) {
        throwIdentifierError(
            zoneId, "uses a name reserved for persistence storage");
    }
}

std::string zoneIdentifierStoragePath(std::string_view zoneId) {
    validateZoneIdentifier(zoneId);
    std::string path(zoneId);
    const auto separator = path.find(':');
    if (separator != std::string::npos) {
        path[separator] = '/';
    }
    return path;
}

} // namespace Rigel::Persistence::detail
