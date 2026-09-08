#include "GraphicalAuthorityClient.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/EntityModel.h"
#include "Rigel/Voxel/BlockTargeting.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/World.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace Rigel::detail {
namespace {

Simulation::CellAddress addressOf(const glm::ivec3& value) {
    return {value.x, value.y, value.z};
}

struct PreparedCell {
    Simulation::CellAddress address;
    Voxel::BlockState state;
    Voxel::ChunkCoord chunk;
};

Simulation::Revision revisionOf(
    const Simulation::PublicationMessage& publication
) {
    return std::visit([](const auto& value) { return value.revision; },
                      publication);
}

Simulation::Tick tickOf(const Simulation::PublicationMessage& publication) {
    return std::visit([](const auto& value) { return value.tick; }, publication);
}

size_t outcomeCountOf(
    const Simulation::PublicationMessage& publication
) {
    return std::visit([](const auto& value) -> size_t {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Simulation::WorldChangeBatch>) {
            return value.outcomes.size();
        }
        return 0;
    }, publication);
}

} // namespace

GraphicalAuthorityClient::GraphicalAuthorityClient(
    Simulation::SimulationHost& host,
    Voxel::World& replicaWorld,
    Asset::AssetManager& assets,
    Simulation::CellBounds interest,
    Entity::EntityId observer,
    Simulation::SessionId session,
    std::string placeBlockKey,
    Simulation::CommandId nextCommand,
    std::vector<Simulation::CommandId> pendingCommands
) : m_host(&host),
    m_replicaWorld(&replicaWorld),
    m_assets(&assets),
    m_replica([&] {
        auto connection = host.connectReplica(interest);
        if (connection.status != Simulation::ReplicaConnectStatus::Connected ||
            !connection.replica) {
            throw std::runtime_error("bounded graphical replica could not connect");
        }
        return std::move(*connection.replica);
    }()),
    m_observerCapability(host.localObserverCapability()),
    m_observer(observer),
    m_session(session),
    m_nextCommand(nextCommand),
    m_placeBlockKey(std::move(placeBlockKey)) {
    if (session == 0 || observer.isNull() || nextCommand == 0 ||
        !host.content().contains(m_placeBlockKey) ||
        m_placeBlockKey == "base:air") {
        throw std::invalid_argument("invalid bounded graphical client session");
    }
    for (const Simulation::CommandId command : pendingCommands) {
        if (command == 0 || command >= nextCommand ||
            !m_pendingSubmissions.emplace(session, command).second) {
            throw std::invalid_argument(
                "invalid recovered graphical client command cursor");
        }
    }
    if (!drainPublications() || !m_hasVisiblePublication ||
        m_visibleRevision != host.revision() ||
        m_visibleTick != host.tick()) {
        throw std::runtime_error("bounded graphical replica baseline is incomplete");
    }
}

GraphicalEditSubmitResult GraphicalAuthorityClient::submit(
    Input::GameplayBlockEditAction action,
    const Voxel::BlockTarget& target,
    const Input::CameraState& camera
) {
    if (m_nextCommand == 0 ||
        m_host->admitLocalObserverPose(
            m_observer, camera.position, m_observerCapability) !=
            Simulation::ObserverPoseStatus::Applied) {
        ++m_submissionStats.rejected;
        return {.status = GraphicalEditSubmitStatus::ObserverRejected};
    }

    Simulation::EditCommand command{
        .session = m_session,
        .command = m_nextCommand,
        .actor = m_observer,
        .world = m_host->world().id(),
        .zone = "base:default",
        .content = m_host->content().identity(),
        .action = action == Input::GameplayBlockEditAction::Remove
            ? Simulation::EditAction::Remove
            : Simulation::EditAction::Place,
        .interaction = Simulation::InteractionIntent{
            .origin = camera.position,
            .direction = camera.forward,
            .maxDistance = 8.0f,
            .expectedTarget = addressOf(target.block),
            .expectedFace = target.face,
            .expectedTargetState = m_host->content().semanticState(target.state),
        },
    };

    const glm::ivec3 destination = action == Input::GameplayBlockEditAction::Remove
        ? target.block : target.block + target.normal;
    const Simulation::CellAddress destinationAddress = addressOf(destination);
    const Simulation::ExactBlockRead expected = m_replica.read(destinationAddress);
    if (expected.status != Simulation::ExactReadStatus::Known) {
        ++m_submissionStats.rejected;
        return {.status =
                    GraphicalEditSubmitStatus::ReplicaStateUnavailable};
    }
    command.mutations.push_back({
        .address = destinationAddress,
        .expected = expected.state,
        .replacement = action == Input::GameplayBlockEditAction::Remove
            ? Simulation::SemanticBlockState{"base:air", 0, 0}
            : Simulation::SemanticBlockState{m_placeBlockKey, 0, 0},
    });

    Simulation::SubmitResult result = m_host->submit(command);
    bool replacedSession = false;
    if (result.status == Simulation::SubmitStatus::ReceiptCapacity) {
        if (!m_pendingSubmissions.empty()) {
            ++m_submissionStats.rejected;
            return {
                .status =
                    GraphicalEditSubmitStatus::SessionReplacementDeferred,
                .hostStatus = result.status,
            };
        }
        const Simulation::SessionId replacement = m_host->nextSessionId();
        if (replacement == 0 ||
            m_host->startSession(
                replacement, m_observer, m_host->content().identity()) !=
                Simulation::SessionStartStatus::Started) {
            ++m_submissionStats.rejected;
            return {
                .status =
                    GraphicalEditSubmitStatus::SessionReplacementFailed,
                .hostStatus = result.status,
            };
        }
        m_session = replacement;
        m_nextCommand = 1;
        command.session = m_session;
        command.command = m_nextCommand;
        result = m_host->submit(command);
        replacedSession = true;
    }
    if (result.status != Simulation::SubmitStatus::Accepted) {
        ++m_submissionStats.rejected;
        return {
            .status = GraphicalEditSubmitStatus::HostRejected,
            .hostStatus = result.status,
        };
    }
    m_pendingSubmissions.emplace(m_session, m_nextCommand);
    ++m_nextCommand;
    ++m_submissionStats.accepted;
    if (replacedSession) {
        ++m_submissionStats.sessionReplacements;
    }
    return {
        .status = replacedSession
            ? GraphicalEditSubmitStatus::AcceptedAfterSessionReplacement
            : GraphicalEditSubmitStatus::Accepted,
        .hostStatus = result.status,
    };
}

Simulation::AdvanceResult GraphicalAuthorityClient::advance(
    std::chrono::nanoseconds elapsed
) {
    if (elapsed.count() < 0) {
        throw std::invalid_argument("simulation elapsed time cannot be negative");
    }
    m_changedChunks.clear();
    m_outcomes.clear();
    if (!drainPublications()) {
        deferElapsed(elapsed);
        return {.timeDebtRemaining = true};
    }
    if (m_deferredElapsed.count() != 0) {
        if (elapsed > std::chrono::nanoseconds::max() - m_deferredElapsed) {
            throw std::overflow_error("deferred graphical simulation time overflow");
        }
        elapsed += m_deferredElapsed;
        m_deferredElapsed = std::chrono::nanoseconds{0};
    }
    const Simulation::AdvanceResult result = m_host->advance(elapsed);
    drainPublications();
    return result;
}

void GraphicalAuthorityClient::deferElapsed(
    std::chrono::nanoseconds elapsed
) {
    if (elapsed > std::chrono::nanoseconds::max() - m_deferredElapsed) {
        throw std::overflow_error("deferred graphical simulation time overflow");
    }
    m_deferredElapsed += elapsed;
}

bool GraphicalAuthorityClient::drainPublications() noexcept {
    if (m_projectionPending) {
        if (!m_pendingPublication) {
            m_projectionBlocked = true;
            return false;
        }
        try {
            apply(*m_pendingPublication);
        } catch (...) {
            m_projectionBlocked = true;
            return false;
        }
        m_projectionPending = false;
        m_pendingPublication.reset();
    }
    for (;;) {
        const Simulation::ReplicaPumpStatus status = m_replica.pumpOne();
        if (status == Simulation::ReplicaPumpStatus::Idle) break;
        if (status == Simulation::ReplicaPumpStatus::NeedsResnapshot) {
            try {
                if (m_host->resnapshot(m_replica) !=
                    Simulation::ReplicaConnectStatus::Connected) {
                    m_projectionBlocked = true;
                    return false;
                }
            } catch (...) {
                m_projectionBlocked = true;
                return false;
            }
            continue;
        }
        if (status == Simulation::ReplicaPumpStatus::Applied) {
            m_pendingPublication = m_replica.appliedPublicationHandle();
            if (!m_pendingPublication) {
                m_projectionBlocked = true;
                return false;
            }
            m_projectionPending = true;
            try {
                apply(*m_pendingPublication);
            } catch (...) {
                m_projectionBlocked = true;
                return false;
            }
            m_projectionPending = false;
            m_pendingPublication.reset();
        }
    }
    m_projectionBlocked = false;
    return true;
}

void GraphicalAuthorityClient::apply(
    const Simulation::PublicationMessage& publication
) {
    std::vector<PreparedCell> preparedCells;
    const std::vector<Simulation::PublishedEntity>* publishedEntities = nullptr;
    const std::vector<Simulation::CommandOutcome>* publishedOutcomes = nullptr;
    const bool baseline = std::holds_alternative<Simulation::WorldBaseline>(
        publication);
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        const auto& cells = [&]() -> const std::vector<Simulation::PublishedCell>& {
            if constexpr (std::is_same_v<T, Simulation::WorldBaseline>) {
                return value.cells;
            } else {
                return value.changes;
            }
        }();
        preparedCells.reserve(cells.size());
        for (const auto& cell : cells) {
            preparedCells.push_back({
                .address = cell.address,
                .state = m_host->content().localState(cell.state),
                .chunk = Voxel::worldToChunk(
                    cell.address.x, cell.address.y, cell.address.z),
            });
        }
        publishedEntities = &value.entities;
        if constexpr (std::is_same_v<T, Simulation::WorldChangeBatch>) {
            publishedOutcomes = &value.outcomes;
        }
    }, publication);

    const auto& generator = m_replicaWorld->generator();
    if (!generator) {
        throw std::runtime_error(
            "graphical replica has no publication generator");
    }
    std::sort(preparedCells.begin(), preparedCells.end(),
              [](const PreparedCell& first, const PreparedCell& second) {
                  if (first.chunk != second.chunk) {
                      return first.chunk < second.chunk;
                  }
                  return first.address < second.address;
              });

    std::vector<std::unique_ptr<Voxel::Chunk>> replacements;
    std::vector<Voxel::ChunkCoord> changedChunks;
    size_t affectedChunkCount = 0;
    for (size_t index = 0; index < preparedCells.size(); ++index) {
        if (index == 0 || preparedCells[index - 1].chunk !=
                              preparedCells[index].chunk) {
            ++affectedChunkCount;
        }
    }
    replacements.reserve(affectedChunkCount);
    changedChunks.reserve(affectedChunkCount);
    for (size_t first = 0; first < preparedCells.size();) {
        const Voxel::ChunkCoord coord = preparedCells[first].chunk;
        size_t last = first + 1;
        while (last < preparedCells.size() &&
               preparedCells[last].chunk == coord) {
            ++last;
        }
        const Voxel::Chunk* current =
            m_replicaWorld->chunkManager().getChunk(coord);
        std::unique_ptr<Voxel::Chunk> replacement = baseline || !current
            ? std::make_unique<Voxel::Chunk>(coord)
            : current->cloneForReplacement();
        for (size_t index = first; index < last; ++index) {
            int localX = 0;
            int localY = 0;
            int localZ = 0;
            const auto& cell = preparedCells[index];
            Voxel::worldToLocal(
                cell.address.x, cell.address.y, cell.address.z,
                localX, localY, localZ);
            replacement->setBlock(
                localX, localY, localZ, cell.state,
                m_replicaWorld->blockRegistry());
        }
        replacement->setWorldGenVersion(generator->semanticsVersion());
        replacements.push_back(std::move(replacement));
        changedChunks.push_back(coord);
        first = last;
    }

    Entity::WorldEntities stagedEntities;
    stagedEntities.bind(m_replicaWorld);
    for (const auto& published : *publishedEntities) {
        auto entity = std::make_unique<Entity::Entity>(published.state.typeId);
        entity->restoreSimulationState(published.state);
        Asset::Handle<Entity::EntityModelAsset> retainedModel;
        if (const Entity::Entity* current =
                m_replicaWorld->entities().get(published.state.id);
            current && current->modelIdentifier() ==
                           published.state.modelIdentifier) {
            retainedModel = current->model();
        }
        if (!published.state.modelIdentifier.empty()) {
            if (!retainedModel &&
                !m_assets->exists(published.state.modelIdentifier)) {
                throw std::runtime_error(
                    "graphical entity model is unavailable: " +
                    published.state.modelIdentifier);
            }
            entity->setModel(retainedModel
                ? std::move(retainedModel)
                : m_assets->get<Entity::EntityModelAsset>(
                      published.state.modelIdentifier));
            entity->setLocalBounds(published.state.localBounds);
        }
        if (stagedEntities.spawn(std::move(entity)) != published.state.id) {
            throw std::runtime_error("graphical entity replica could not stage");
        }
    }

    m_changedChunks.reserve(m_changedChunks.size() + changedChunks.size());
    std::vector<Simulation::CommandOutcome> recoveredOutcomes;
    if (baseline) {
        recoveredOutcomes.reserve(m_pendingSubmissions.size());
        for (const auto& [session, command] : m_pendingSubmissions) {
            if (auto outcome = m_host->completedOutcome(session, command)) {
                recoveredOutcomes.push_back(*outcome);
            }
        }
    }
    const size_t outcomeCount = baseline
        ? recoveredOutcomes.size() : outcomeCountOf(publication);
    m_outcomes.reserve(m_outcomes.size() + outcomeCount);
    auto preparedChunks =
        m_replicaWorld->chunkManager().prepareChunkReplacements(
            std::move(replacements));

    m_replicaWorld->chunkManager().installChunkReplacements(
        std::move(preparedChunks));
    m_replicaWorld->entities().installPreparedEntities(stagedEntities);
    for (const Voxel::ChunkCoord coord : changedChunks) {
        if (std::find(m_changedChunks.begin(), m_changedChunks.end(), coord) ==
            m_changedChunks.end()) {
            m_changedChunks.push_back(coord);
        }
    }
    m_visibleRevision = revisionOf(publication);
    m_visibleTick = tickOf(publication);
    m_hasVisiblePublication = true;
    for (size_t index = 0; index < outcomeCount; ++index) {
        const Simulation::CommandOutcome& outcome = baseline
            ? recoveredOutcomes[index] : (*publishedOutcomes)[index];
        if (auto replicaOutcome = m_replica.takeOutcome();
            replicaOutcome && *replicaOutcome != outcome) {
            std::terminate();
        }
        m_pendingSubmissions.erase({outcome.session, outcome.command});
        m_outcomes.push_back(outcome);
    }
}

} // namespace Rigel::detail
