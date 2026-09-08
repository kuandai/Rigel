#include "GraphicalAuthorityClient.h"

#include "Rigel/Asset/AssetManager.h"
#include "Rigel/Entity/EntityModel.h"
#include "Rigel/Voxel/BlockTargeting.h"
#include "Rigel/Voxel/Chunk.h"
#include "Rigel/Voxel/World.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace Rigel::detail {
namespace {

Simulation::CellAddress addressOf(const glm::ivec3& value) {
    return {value.x, value.y, value.z};
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
    drainPublications();
    if (m_replica.revision() != host.revision() ||
        m_replica.tick() != host.tick()) {
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
    m_changedChunks.clear();
    m_outcomes.clear();
    const Simulation::AdvanceResult result = m_host->advance(elapsed);
    drainPublications();
    return result;
}

void GraphicalAuthorityClient::drainPublications() {
    for (;;) {
        const Simulation::ReplicaPumpStatus status = m_replica.pumpOne();
        if (status == Simulation::ReplicaPumpStatus::Idle) break;
        if (status == Simulation::ReplicaPumpStatus::NeedsResnapshot) {
            if (m_host->resnapshot(m_replica) !=
                Simulation::ReplicaConnectStatus::Connected) {
                throw std::runtime_error("bounded graphical replica lost continuity");
            }
            continue;
        }
        if (status == Simulation::ReplicaPumpStatus::Applied) {
            const auto* publication = m_replica.appliedPublication();
            if (!publication) {
                throw std::runtime_error("graphical replica applied no publication");
            }
            apply(*publication);
        }
    }
    while (auto outcome = m_replica.takeOutcome()) {
        m_pendingSubmissions.erase({outcome->session, outcome->command});
        m_outcomes.push_back(*outcome);
    }
}

void GraphicalAuthorityClient::apply(
    const Simulation::PublicationMessage& publication
) {
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        const auto& cells = [&]() -> const std::vector<Simulation::PublishedCell>& {
            if constexpr (std::is_same_v<T, Simulation::WorldBaseline>) {
                return value.cells;
            } else {
                return value.changes;
            }
        }();
        for (const auto& cell : cells) applyCell(cell);
        applyEntities(value.entities);
    }, publication);
}

void GraphicalAuthorityClient::applyCell(
    const Simulation::PublishedCell& cell
) {
    m_replicaWorld->setBlock(
        cell.address.x, cell.address.y, cell.address.z,
        m_host->content().localState(cell.state));
    const Voxel::ChunkCoord chunk = Voxel::worldToChunk(
        cell.address.x, cell.address.y, cell.address.z);
    Voxel::Chunk* installed =
        m_replicaWorld->chunkManager().getChunk(chunk);
    const auto& generator = m_replicaWorld->generator();
    if (!installed || !generator) {
        throw std::runtime_error(
            "graphical replica could not retain its published chunk");
    }
    installed->setWorldGenVersion(generator->semanticsVersion());
    if (std::find(m_changedChunks.begin(), m_changedChunks.end(), chunk) ==
        m_changedChunks.end()) {
        m_changedChunks.push_back(chunk);
    }
}

void GraphicalAuthorityClient::applyEntities(
    const std::vector<Simulation::PublishedEntity>& entities
) {
    const auto current = m_replicaWorld->entities().sortedIds();
    for (const Entity::EntityId id : current) {
        const bool present = std::any_of(
            entities.begin(), entities.end(), [&](const auto& entity) {
                return entity.state.id == id;
            });
        if (!present) m_replicaWorld->entities().despawn(id);
    }

    for (const auto& published : entities) {
        Entity::Entity* entity =
            m_replicaWorld->entities().get(published.state.id);
        Asset::Handle<Entity::EntityModelAsset> retainedModel;
        if (entity &&
            entity->modelIdentifier() == published.state.modelIdentifier) {
            retainedModel = entity->model();
        }
        if (!entity) {
            auto created = std::make_unique<Entity::Entity>(
                published.state.typeId);
            created->restoreSimulationState(published.state);
            if (m_replicaWorld->entities().spawn(std::move(created)) !=
                published.state.id) {
                throw std::runtime_error("graphical entity replica could not spawn");
            }
            entity = m_replicaWorld->entities().get(published.state.id);
        } else {
            entity->restoreSimulationState(published.state);
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
    }
}

} // namespace Rigel::detail
