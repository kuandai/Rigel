#include "Rigel/Simulation/SimulationCheckpoint.h"

#include <atomic>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace Rigel::Simulation {
namespace {

constexpr uint32_t PointerMagic = 0x52474350; // RGCP
constexpr uint32_t PointerVersion = 1;
constexpr size_t MaximumPointerBytes = 512;

uint64_t readU64(Persistence::ByteReader& reader) {
    const uint32_t high = reader.readU32();
    const uint32_t low = reader.readU32();
    return (uint64_t{high} << 32) | low;
}

void writeU64(Persistence::ByteWriter& writer, uint64_t value) {
    writer.writeU32(static_cast<uint32_t>(value >> 32));
    writer.writeU32(static_cast<uint32_t>(value));
}

uint64_t hashBytes(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 1469598103934665603ULL;
    for (const uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct PointerRecord {
    uint64_t generation = 0;
    uint64_t parentHash = 0;
    uint64_t stateHash = 0;
    uint64_t payloadHash = 0;
    uint64_t payloadBytes = 0;
    Tick tick = 0;
    Revision revision = 0;
};

std::string payloadPath(const std::string& root, uint64_t generation) {
    return root + "/checkpoints/" + std::to_string(generation) + ".bin";
}

std::string pendingPublicationPath(const std::string& root) {
    return root + "/publication-pending";
}

PointerRecord readPointer(
    Persistence::StorageBackend& storage, const std::string& root
) {
    auto reader = storage.openRead(root + "/current");
    if (reader->size() > MaximumPointerBytes || reader->size() != 64) {
        throw std::runtime_error("checkpoint pointer size is invalid");
    }
    if (reader->readU32() != PointerMagic ||
        reader->readU32() != PointerVersion) {
        throw std::invalid_argument("checkpoint pointer format is incompatible");
    }
    PointerRecord result;
    result.generation = readU64(*reader);
    result.parentHash = readU64(*reader);
    result.stateHash = readU64(*reader);
    result.payloadHash = readU64(*reader);
    result.payloadBytes = readU64(*reader);
    result.tick = readU64(*reader);
    result.revision = readU64(*reader);
    if (result.generation == 0 || result.payloadBytes == 0) {
        throw std::runtime_error("checkpoint pointer fields are invalid");
    }
    return result;
}

void writePointer(Persistence::ByteWriter& writer, const PointerRecord& value) {
    writer.writeU32(PointerMagic); writer.writeU32(PointerVersion);
    writeU64(writer, value.generation); writeU64(writer, value.parentHash);
    writeU64(writer, value.stateHash); writeU64(writer, value.payloadHash);
    writeU64(writer, value.payloadBytes); writeU64(writer, value.tick);
    writeU64(writer, value.revision);
}

std::vector<uint8_t> readPayload(
    Persistence::StorageBackend& storage,
    const std::string& path,
    size_t expectedBytes
) {
    auto reader = storage.openRead(path);
    if (reader->size() != expectedBytes) {
        throw std::runtime_error("checkpoint payload size does not match pointer");
    }
    return reader->readAt(0, expectedBytes);
}

} // namespace

struct SimulationCheckpointManager::Impl {
    std::shared_ptr<Persistence::StorageBackend> storage;
    std::string root;
    std::unique_ptr<Persistence::WorldGenerationBootstrapLock> rootLock;
    std::thread writer;
    mutable std::mutex outcomeMutex;
    std::optional<CheckpointOutcome> terminal;
    std::atomic<bool> running{false};
    uint64_t latestGeneration = 0;
    uint64_t latestCutHash = 0;
    uint64_t pendingCutHash = 0;
    std::weak_ptr<const uint8_t> lineageOwner;
    std::weak_ptr<const uint8_t> pendingOwner;
    bool lineageVerified = true;
    bool uncertain = false;
    std::optional<CheckpointRecoveryStatus> invalidRoot;
    std::string invalidDetail;

    ~Impl() {
        if (writer.joinable()) writer.join();
    }

    void inspectRoot() {
        const auto rootKind = storage->entryKind(root);
        if (rootKind == Persistence::StorageEntryKind::Missing) {
            storage->mkdirs(root + "/checkpoints");
            return;
        }
        if (rootKind != Persistence::StorageEntryKind::Directory) {
            invalidRoot = CheckpointRecoveryStatus::Incompatible;
            invalidDetail = "checkpoint root is not a directory";
            return;
        }
        bool unknown = false;
        storage->forEachEntry(root, [&](const std::string& name) {
            const std::string leaf = std::filesystem::path(name).filename().string();
            if (leaf != "current" && leaf != "checkpoints" &&
                leaf != "publication-pending") unknown = true;
            return true;
        });
        if (unknown) {
            invalidRoot = CheckpointRecoveryStatus::Incompatible;
            invalidDetail = "checkpoint root contains an unknown format";
            return;
        }
        const auto currentKind = storage->entryKind(root + "/current");
        const auto checkpointsKind = storage->entryKind(root + "/checkpoints");
        if (checkpointsKind == Persistence::StorageEntryKind::Missing) {
            if (currentKind != Persistence::StorageEntryKind::Missing) {
                invalidRoot = CheckpointRecoveryStatus::Corrupt;
                invalidDetail = "checkpoint payload directory is missing";
                return;
            }
            storage->mkdirs(root + "/checkpoints");
        } else if (checkpointsKind != Persistence::StorageEntryKind::Directory) {
            invalidRoot = CheckpointRecoveryStatus::Incompatible;
            invalidDetail = "checkpoint payload path is incompatible";
            return;
        }
        if (storage->entryKind(pendingPublicationPath(root)) !=
            Persistence::StorageEntryKind::Missing) {
            uncertain = true;
        }
        if (currentKind == Persistence::StorageEntryKind::Missing) return;
        try {
            const PointerRecord pointer = readPointer(*storage, root);
            if (storage->entryKind(payloadPath(root, pointer.generation)) !=
                Persistence::StorageEntryKind::RegularFile) {
                throw std::runtime_error("checkpoint pointer payload is missing");
            }
            latestGeneration = pointer.generation;
            latestCutHash = pointer.payloadHash;
            lineageVerified = false;
        } catch (const std::invalid_argument& error) {
            invalidRoot = CheckpointRecoveryStatus::Incompatible;
            invalidDetail = error.what();
        } catch (const std::exception& error) {
            invalidRoot = CheckpointRecoveryStatus::Corrupt;
            invalidDetail = error.what();
        }
    }
};

SimulationCheckpointManager::SimulationCheckpointManager(
    std::shared_ptr<Persistence::StorageBackend> storage,
    std::string root
) : m_impl(std::make_unique<Impl>()) {
    if (!storage || root.empty()) {
        throw std::invalid_argument("checkpoint storage and root are required");
    }
    m_impl->storage = std::move(storage);
    m_impl->root = std::move(root);
    m_impl->rootLock =
        m_impl->storage->lockWorldGenerationBootstrap(m_impl->root);
    m_impl->inspectRoot();
}

SimulationCheckpointManager::~SimulationCheckpointManager() = default;

CheckpointRequestStatus SimulationCheckpointManager::request(
    const SimulationHost& host
) {
    if (m_impl->uncertain) return CheckpointRequestStatus::Uncertain;
    if (m_impl->invalidRoot) return CheckpointRequestStatus::UnsupportedState;
    if (m_impl->latestGeneration != 0 &&
        (!m_impl->lineageVerified ||
         m_impl->lineageOwner.lock() != host.m_authorityEditKey)) {
        return CheckpointRequestStatus::UnsupportedState;
    }
    if (m_impl->running.load(std::memory_order_acquire)) {
        return CheckpointRequestStatus::Coalesced;
    }
    if (m_impl->writer.joinable()) m_impl->writer.join();
    {
        std::lock_guard lock(m_impl->outcomeMutex);
        if (m_impl->terminal) return CheckpointRequestStatus::Coalesced;
    }
    if (m_impl->latestGeneration == std::numeric_limits<uint64_t>::max()) {
        return CheckpointRequestStatus::UnsupportedState;
    }
    uint64_t generation = m_impl->latestGeneration + 1;
    while (m_impl->storage->entryKind(payloadPath(m_impl->root, generation)) !=
           Persistence::StorageEntryKind::Missing) {
        if (generation == std::numeric_limits<uint64_t>::max()) {
            return CheckpointRequestStatus::UnsupportedState;
        }
        ++generation;
    }

    std::vector<uint8_t> payload;
    uint64_t stateHash = 0;
    try {
        stateHash = host.stateHash();
        payload = host.checkpointBytes(generation, m_impl->latestCutHash);
    } catch (...) {
        return CheckpointRequestStatus::UnsupportedState;
    }
    const PointerRecord pointer{
        .generation = generation,
        .parentHash = m_impl->latestCutHash,
        .stateHash = stateHash,
        .payloadHash = hashBytes(payload),
        .payloadBytes = payload.size(),
        .tick = host.tick(),
        .revision = host.revision(),
    };
    auto* impl = m_impl.get();
    impl->pendingOwner = host.m_authorityEditKey;
    impl->pendingCutHash = pointer.payloadHash;
    impl->running.store(true, std::memory_order_release);
    impl->writer = std::thread([
        impl, payload = std::move(payload), pointer]() mutable {
        CheckpointOutcome outcome{
            .status = CheckpointWriteStatus::NotPublished,
            .generation = pointer.generation,
            .tick = pointer.tick,
            .revision = pointer.revision,
            .stateHash = pointer.stateHash,
        };
        bool pointerCommitStarted = false;
        bool pendingPublicationWritten = false;
        try {
            auto payloadWrite = impl->storage->openWrite(
                payloadPath(impl->root, pointer.generation));
            payloadWrite->writer().writeBytes(payload.data(), payload.size());
            payloadWrite->writer().flush();
            payloadWrite->commit();

            auto pendingWrite = impl->storage->openWrite(
                pendingPublicationPath(impl->root));
            writeU64(pendingWrite->writer(), pointer.generation);
            writeU64(pendingWrite->writer(), pointer.payloadHash);
            pendingWrite->writer().flush();
            pendingWrite->commit();
            pendingPublicationWritten = true;

            auto pointerWrite = impl->storage->openWrite(impl->root + "/current");
            writePointer(pointerWrite->writer(), pointer);
            pointerWrite->writer().flush();
            pointerCommitStarted = true;
            pointerWrite->commit();
            impl->storage->remove(pendingPublicationPath(impl->root));
            pendingPublicationWritten = false;
            outcome.status = CheckpointWriteStatus::Durable;
        } catch (const Persistence::AtomicFilePublicationError& error) {
            if (pointerCommitStarted && error.state() ==
                    Persistence::AtomicFilePublicationState::PublishedDurabilityUncertain) {
                outcome.status = CheckpointWriteStatus::DurabilityUnknown;
            }
            outcome.detail = error.what();
        } catch (const std::exception& error) {
            outcome.status = pointerCommitStarted
                ? CheckpointWriteStatus::DurabilityUnknown
                : CheckpointWriteStatus::NotPublished;
            outcome.detail = error.what();
        } catch (...) {
            outcome.status = pointerCommitStarted
                ? CheckpointWriteStatus::DurabilityUnknown
                : CheckpointWriteStatus::NotPublished;
            outcome.detail = "unknown checkpoint storage failure";
        }
        if (outcome.status == CheckpointWriteStatus::NotPublished &&
            pendingPublicationWritten) {
            try {
                impl->storage->remove(pendingPublicationPath(impl->root));
            } catch (...) {
                outcome.status = CheckpointWriteStatus::DurabilityUnknown;
                outcome.detail = "checkpoint publication cleanup is uncertain";
            }
        }
        {
            std::lock_guard lock(impl->outcomeMutex);
            impl->terminal = std::move(outcome);
        }
        impl->running.store(false, std::memory_order_release);
    });
    return CheckpointRequestStatus::Started;
}

std::optional<CheckpointOutcome> SimulationCheckpointManager::poll() {
    if (m_impl->running.load(std::memory_order_acquire)) return std::nullopt;
    if (m_impl->writer.joinable()) m_impl->writer.join();
    std::optional<CheckpointOutcome> result;
    {
        std::lock_guard lock(m_impl->outcomeMutex);
        result = std::move(m_impl->terminal);
        m_impl->terminal.reset();
    }
    if (!result) return std::nullopt;
    if (result->status == CheckpointWriteStatus::Durable) {
        m_impl->latestGeneration = result->generation;
        m_impl->latestCutHash = m_impl->pendingCutHash;
        m_impl->lineageOwner = m_impl->pendingOwner;
        m_impl->lineageVerified = !m_impl->lineageOwner.expired();
    } else if (result->status == CheckpointWriteStatus::DurabilityUnknown) {
        m_impl->uncertain = true;
    }
    return result;
}

bool SimulationCheckpointManager::writeInFlight() const {
    return m_impl->running.load(std::memory_order_acquire);
}

bool SimulationCheckpointManager::durabilityUncertain() const {
    return m_impl->uncertain;
}

CheckpointRecovery SimulationCheckpointManager::recover(
    Voxel::WorldResources& resources,
    std::shared_ptr<const Voxel::WorldGenerator> generator
) {
    if (m_impl->running.load(std::memory_order_acquire) || m_impl->uncertain) {
        return {CheckpointRecoveryStatus::Corrupt, 0, nullptr,
                "checkpoint publication is not resolved"};
    }
    if (m_impl->invalidRoot) {
        return {*m_impl->invalidRoot, 0, nullptr, m_impl->invalidDetail};
    }
    if (m_impl->latestGeneration == 0) {
        return {CheckpointRecoveryStatus::Empty};
    }
    try {
        const PointerRecord pointer = readPointer(*m_impl->storage, m_impl->root);
        if (pointer.generation != m_impl->latestGeneration ||
            pointer.payloadHash != m_impl->latestCutHash ||
            pointer.payloadBytes > 256ULL * 1024 * 1024) {
            throw std::runtime_error("checkpoint pointer changed outside live owner");
        }
        auto payload = readPayload(
            *m_impl->storage, payloadPath(m_impl->root, pointer.generation),
            static_cast<size_t>(pointer.payloadBytes));
        if (hashBytes(payload) != pointer.payloadHash) {
            throw std::runtime_error("checkpoint payload hash mismatch");
        }
        auto host = SimulationHost::restoreCheckpointBytes(
            resources, std::move(generator), payload,
            pointer.generation, pointer.parentHash);
        if (host->tick() != pointer.tick ||
            host->revision() != pointer.revision ||
            host->stateHash() != pointer.stateHash) {
            throw std::runtime_error("checkpoint cut does not match pointer");
        }
        m_impl->lineageOwner = host->m_authorityEditKey;
        m_impl->lineageVerified = true;
        return {CheckpointRecoveryStatus::Recovered, pointer.generation,
                std::move(host), {}};
    } catch (const ContentManifestError& error) {
        return {CheckpointRecoveryStatus::Incompatible, 0, nullptr, error.what()};
    } catch (const std::invalid_argument& error) {
        return {CheckpointRecoveryStatus::Incompatible, 0, nullptr, error.what()};
    } catch (const std::exception& error) {
        return {CheckpointRecoveryStatus::Corrupt, 0, nullptr, error.what()};
    }
}

} // namespace Rigel::Simulation
