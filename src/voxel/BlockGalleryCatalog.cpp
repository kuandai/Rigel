#include "Rigel/Voxel/BlockGalleryCatalog.h"

#include "Rigel/Voxel/BlockRegistry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace Rigel::Voxel {
namespace {

struct ParsedIdentifier {
    std::string nameSpace;
    std::string baseIdentifier;
    std::vector<std::pair<std::string, std::string>> properties;
    std::string family;
};

ParsedIdentifier parseIdentifier(std::string_view identifier) {
    ParsedIdentifier parsed;

    const size_t namespaceEnd = identifier.find(':');
    std::string_view localIdentifier = identifier;
    if (namespaceEnd != std::string_view::npos) {
        parsed.nameSpace = identifier.substr(0, namespaceEnd);
        localIdentifier = identifier.substr(namespaceEnd + 1);
    }

    std::string_view baseIdentifier = localIdentifier;
    const size_t propertiesBegin = localIdentifier.find('[');
    if (propertiesBegin != std::string_view::npos &&
        localIdentifier.ends_with(']')) {
        baseIdentifier = localIdentifier.substr(0, propertiesBegin);
        std::string_view properties = localIdentifier.substr(
            propertiesBegin + 1,
            localIdentifier.size() - propertiesBegin - 2);
        while (!properties.empty()) {
            const size_t separator = properties.find(',');
            const std::string_view property = properties.substr(0, separator);
            const size_t valueBegin = property.find('=');
            if (valueBegin == std::string_view::npos) {
                parsed.properties.emplace_back(property, std::string{});
            } else {
                parsed.properties.emplace_back(
                    property.substr(0, valueBegin),
                    property.substr(valueBegin + 1));
            }
            if (separator == std::string_view::npos) {
                break;
            }
            properties.remove_prefix(separator + 1);
        }
        std::sort(parsed.properties.begin(), parsed.properties.end());
    }

    parsed.baseIdentifier = baseIdentifier;
    parsed.family = parsed.nameSpace.empty()
        ? parsed.baseIdentifier
        : parsed.nameSpace + ":" + parsed.baseIdentifier;
    return parsed;
}

struct Candidate {
    const BlockType* type = nullptr;
    BlockID blockId;
    ParsedIdentifier parsed;
};

bool identifierLess(const Candidate& left, const Candidate& right) {
    if (left.parsed.nameSpace != right.parsed.nameSpace) {
        return left.parsed.nameSpace < right.parsed.nameSpace;
    }
    if (left.parsed.baseIdentifier != right.parsed.baseIdentifier) {
        return left.parsed.baseIdentifier < right.parsed.baseIdentifier;
    }
    if (left.parsed.properties != right.parsed.properties) {
        return left.parsed.properties < right.parsed.properties;
    }
    return left.type->identifier < right.type->identifier;
}

size_t squareGridWidth(size_t entryCount) {
    if (entryCount == 0) {
        return 0;
    }
    const double root = std::sqrt(static_cast<double>(entryCount));
    size_t width = static_cast<size_t>(root);
    if (width * width < entryCount) {
        ++width;
    }
    return width;
}

} // namespace

BlockGalleryCatalog::BlockGalleryCatalog(const BlockRegistry& registry)
    : m_sourceRegistry(&registry) {
    if (!registry.frozen()) {
        throw std::invalid_argument(
            "Block gallery catalog requires a frozen block registry");
    }

    m_diagnostics.loadedRegistrationCount = registry.size();
    m_blockIdToCatalogIndex.assign(registry.size(), MissingIndex);

    std::vector<Candidate> candidates;
    std::vector<Candidate> emptyCandidates;
    candidates.reserve(registry.size());
    for (size_t index = 0; index < registry.size(); ++index) {
        const BlockID blockId{static_cast<uint16_t>(index)};
        const BlockType& type = registry.getType(blockId);
        if (type.model->isEmpty()) {
            emptyCandidates.push_back(
                {&type, blockId, parseIdentifier(type.identifier)});
            continue;
        }
        candidates.push_back({&type, blockId, parseIdentifier(type.identifier)});
    }

    std::sort(candidates.begin(), candidates.end(), &identifierLess);
    std::sort(emptyCandidates.begin(), emptyCandidates.end(), &identifierLess);
    m_emptyGeometryExclusions.reserve(emptyCandidates.size());
    for (const Candidate& excluded : emptyCandidates) {
        m_emptyGeometryExclusions.push_back(
            {excluded.type->identifier, excluded.blockId});
    }
    m_diagnostics.renderableCount = candidates.size();
    m_diagnostics.explicitEmptyGeometryCount =
        m_emptyGeometryExclusions.size();
    m_gridDimensions.columns = squareGridWidth(candidates.size());

    m_entries.reserve(candidates.size());
    size_t column = 0;
    size_t row = 0;
    for (size_t familyBegin = 0; familyBegin < candidates.size();) {
        size_t familyEnd = familyBegin + 1;
        while (familyEnd < candidates.size() &&
               candidates[familyEnd].parsed.family ==
                   candidates[familyBegin].parsed.family) {
            ++familyEnd;
        }

        const size_t familySize = familyEnd - familyBegin;
        const size_t remainingColumns = m_gridDimensions.columns - column;
        if (column != 0 &&
            (familySize > m_gridDimensions.columns ||
             familySize > remainingColumns)) {
            column = 0;
            ++row;
        }

        for (size_t index = familyBegin; index < familyEnd; ++index) {
            const Candidate& candidate = candidates[index];
            const size_t catalogIndex = m_entries.size();
            m_entries.push_back({
                .identifier = candidate.type->identifier,
                .blockId = candidate.blockId,
                .family = candidate.parsed.family,
                .catalogIndex = catalogIndex,
                .gridCoordinate = {column, row},
                .specimenPosition = {
                    static_cast<int>(column) * SpecimenSpacing,
                    SpecimenHeight,
                    static_cast<int>(row) * SpecimenSpacing,
                },
            });
            m_blockIdToCatalogIndex[candidate.blockId.type] = catalogIndex;

            ++column;
            if (column == m_gridDimensions.columns) {
                column = 0;
                ++row;
            }
        }
        familyBegin = familyEnd;
    }

    if (!m_entries.empty()) {
        m_gridDimensions.rows = m_entries.back().gridCoordinate.row + 1;
    }
    m_gridToCatalogIndex.assign(
        m_gridDimensions.columns * m_gridDimensions.rows, MissingIndex);
    for (const BlockGalleryCatalogEntry& entry : m_entries) {
        const size_t gridIndex =
            entry.gridCoordinate.row * m_gridDimensions.columns +
            entry.gridCoordinate.column;
        m_gridToCatalogIndex[gridIndex] = entry.catalogIndex;
    }

    if (m_entries.empty()) {
        spdlog::warn(
            "Block gallery catalog has no renderable registrations: "
            "loaded={}, excluded_explicit_empty_geometry={}",
            m_diagnostics.loadedRegistrationCount,
            m_diagnostics.explicitEmptyGeometryCount);
    } else {
        spdlog::info(
            "Block gallery catalog prepared: renderable={}, "
            "excluded_explicit_empty_geometry={}, grid={}x{}, spacing={}",
            m_diagnostics.renderableCount,
            m_diagnostics.explicitEmptyGeometryCount,
            m_gridDimensions.columns,
            m_gridDimensions.rows,
            SpecimenSpacing);
    }
}

const BlockGalleryCatalogEntry* BlockGalleryCatalog::findByIndex(
    size_t catalogIndex
) const {
    return catalogIndex < m_entries.size() ? &m_entries[catalogIndex] : nullptr;
}

const BlockGalleryCatalogEntry* BlockGalleryCatalog::findByBlockId(
    BlockID blockId
) const {
    if (blockId.type >= m_blockIdToCatalogIndex.size()) {
        return nullptr;
    }
    return findByIndex(m_blockIdToCatalogIndex[blockId.type]);
}

const BlockGalleryCatalogEntry* BlockGalleryCatalog::findByGridCoordinate(
    BlockGalleryGridCoordinate coordinate
) const {
    if (coordinate.column >= m_gridDimensions.columns ||
        coordinate.row >= m_gridDimensions.rows) {
        return nullptr;
    }
    const size_t gridIndex =
        coordinate.row * m_gridDimensions.columns + coordinate.column;
    return findByIndex(m_gridToCatalogIndex[gridIndex]);
}

const BlockGalleryCatalogEntry* BlockGalleryCatalog::findBySpecimenPosition(
    BlockGalleryWorldPosition position
) const {
    if (position.y != SpecimenHeight || position.x < 0 || position.z < 0 ||
        position.x % SpecimenSpacing != 0 ||
        position.z % SpecimenSpacing != 0) {
        return nullptr;
    }
    return findByGridCoordinate({
        static_cast<size_t>(position.x / SpecimenSpacing),
        static_cast<size_t>(position.z / SpecimenSpacing),
    });
}

} // namespace Rigel::Voxel
