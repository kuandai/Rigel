#include "TestFramework.h"

#include "ApplicationEntry.h"
#include "ApplicationTestAccess.h"
#include "Rigel/Persistence/Storage.h"

#include <cstdlib>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

namespace {

struct LifecycleCalls {
    std::vector<std::string> runtime;
    std::vector<Rigel::ApplicationShutdownStage> shutdown;
    GLFWwindow* window = reinterpret_cast<GLFWwindow*>(0x1);
    GLFWwindow* destroyedWindow = nullptr;
    bool runLoopEntered = false;
    size_t persistenceAttempts = 0;
    bool closeFailureObserved = false;
    bool dirtyAtCloseFailure = false;
    bool shutdownStartedAtCloseFailure = false;
    std::shared_ptr<Rigel::Persistence::StorageBackend> persistenceStorage;
    std::string persistenceRoot;
};

LifecycleCalls* g_calls = nullptr;

class ScopedLifecycleCalls {
public:
    explicit ScopedLifecycleCalls(LifecycleCalls& calls) {
        g_calls = &calls;
    }

    ~ScopedLifecycleCalls() {
        g_calls = nullptr;
    }
};

class LogCapture {
public:
    LogCapture()
        : m_previous(spdlog::default_logger())
        , m_logger(std::make_shared<spdlog::logger>(
              "application-lifecycle-test",
              std::make_shared<spdlog::sinks::ostream_sink_mt>(m_output))) {
        m_logger->set_level(spdlog::level::info);
        m_logger->set_pattern("%v");
        spdlog::set_default_logger(m_logger);
    }

    ~LogCapture() {
        spdlog::set_default_logger(m_previous);
    }

    std::string output() {
        m_logger->flush();
        return m_output.str();
    }

private:
    std::ostringstream m_output;
    std::shared_ptr<spdlog::logger> m_previous;
    std::shared_ptr<spdlog::logger> m_logger;
};

int initialize() {
    g_calls->runtime.emplace_back("initialize");
    return 1;
}

void terminate() {
    g_calls->runtime.emplace_back("terminate");
}

void windowHint(int, int) {
}

GLFWwindow* createWindow(int, int, const char*, GLFWmonitor*, GLFWwindow*) {
    g_calls->runtime.emplace_back("create window");
    return g_calls->window;
}

void destroyWindow(GLFWwindow* window) {
    g_calls->destroyedWindow = window;
    g_calls->runtime.emplace_back("destroy window");
}

void makeContextCurrent(GLFWwindow* window) {
    g_calls->runtime.emplace_back(
        window == nullptr ? "clear context" : "make context current");
}

void failAfterContextAcquired() {
    throw std::runtime_error("required bootstrap data unavailable");
}

void recordRunLoopEntry(Rigel::Application&) {
    g_calls->runLoopEntered = true;
}

void recordShutdownStage(Rigel::ApplicationShutdownStage stage) noexcept {
    g_calls->shutdown.push_back(stage);
}

enum class PersistenceFailurePoint {
    ChunkWrite,
    JournalPublication,
    EntityWrite,
};

class FailingStorageBackend final : public Rigel::Persistence::StorageBackend {
public:
    explicit FailingStorageBackend(PersistenceFailurePoint failurePoint)
        : m_failurePoint(failurePoint) {
    }

    std::unique_ptr<Rigel::Persistence::ByteReader> openRead(
        const std::string& path
    ) override {
        return m_storage.openRead(path);
    }

    std::unique_ptr<Rigel::Persistence::AtomicWriteSession> openWrite(
        const std::string& path
    ) override {
        if (matches(path)) {
            ++g_calls->persistenceAttempts;
            if (m_failuresRemaining > 0) {
                --m_failuresRemaining;
                fail(path);
            }
        }
        return m_storage.openWrite(path);
    }

    bool exists(const std::string& path) override {
        return m_storage.exists(path);
    }

    void forEachEntry(
        const std::string& path,
        const Rigel::Persistence::StorageEntryVisitor& visitor) override {
        m_storage.forEachEntry(path, visitor);
    }

    std::vector<std::string> list(const std::string& path) override {
        return m_storage.list(path);
    }

    void mkdirs(const std::string& path) override {
        m_storage.mkdirs(path);
    }

    void remove(const std::string& path) override {
        m_storage.remove(path);
    }

private:
    bool matches(const std::string& path) const {
        switch (m_failurePoint) {
            case PersistenceFailurePoint::ChunkWrite:
                return path.find("/regions/region_") != std::string::npos;
            case PersistenceFailurePoint::JournalPublication:
                return path.ends_with("/entity-regions.journal");
            case PersistenceFailurePoint::EntityWrite:
                return path.find("/entities/entityRegion_") != std::string::npos;
        }
        return false;
    }

    [[noreturn]] void fail(const std::string& path) {
        throw std::runtime_error(
            "injected storage failure for " + path);
    }

    PersistenceFailurePoint m_failurePoint;
    size_t m_failuresRemaining = 1;
    Rigel::Persistence::FilesystemBackend m_storage;
};

void observeCloseFailure(bool dirtyWorld) {
    g_calls->closeFailureObserved = true;
    g_calls->dirtyAtCloseFailure = dirtyWorld;
    g_calls->shutdownStartedAtCloseFailure = !g_calls->shutdown.empty();
}

Rigel::GlfwRuntime::Api fakeRuntimeApi() {
    return {
        &initialize,
        &terminate,
        &windowHint,
        &createWindow,
        &destroyWindow,
        &makeContextCurrent,
    };
}

void runFailingApplication() {
    Rigel::ApplicationTestAccess::constructAndRun(
        {
            fakeRuntimeApi(),
            &failAfterContextAcquired,
            &recordShutdownStage,
        },
        &recordRunLoopEntry);
}

void runApplicationWithCloseFailure() {
    Rigel::ApplicationTestAccess::closeReadyWorld({
        g_calls->persistenceStorage,
        &observeCloseFailure,
        &recordShutdownStage,
        g_calls->persistenceRoot,
    });
}

} // namespace

TEST_CASE(Application_ConstructionFailureUsesOrderedShutdownOnce) {
    LifecycleCalls calls;
    ScopedLifecycleCalls scopedCalls(calls);

    CHECK_THROWS(Rigel::ApplicationTestAccess::construct({
        fakeRuntimeApi(),
        &failAfterContextAcquired,
        &recordShutdownStage,
    }));

    const std::vector<Rigel::ApplicationShutdownStage> expectedShutdown = {
        Rigel::ApplicationShutdownStage::ContextMadeCurrent,
        Rigel::ApplicationShutdownStage::UserInterfaceReleased,
        Rigel::ApplicationShutdownStage::AsyncLoadingStopped,
        Rigel::ApplicationShutdownStage::WorldsReleased,
        Rigel::ApplicationShutdownStage::RenderResourcesReleased,
        Rigel::ApplicationShutdownStage::AssetCacheReleased,
        Rigel::ApplicationShutdownStage::RuntimeReleased,
    };
    CHECK_EQ(calls.shutdown, expectedShutdown);

    const std::vector<std::string> expectedRuntime = {
        "initialize",
        "create window",
        "make context current",
        "make context current",
        "clear context",
        "destroy window",
        "terminate",
    };
    CHECK_EQ(calls.runtime, expectedRuntime);
    CHECK_EQ(calls.destroyedWindow, calls.window);
}

TEST_CASE(Application_BootstrapFailureReturnsFailureBeforeRunLoop) {
    LifecycleCalls calls;
    ScopedLifecycleCalls scopedCalls(calls);
    LogCapture logs;

    const int result = Rigel::runApplication(&runFailingApplication);

    CHECK_EQ(result, EXIT_FAILURE);
    CHECK(!calls.runLoopEntered);
    CHECK(logs.output().find("required bootstrap data unavailable") !=
          std::string::npos);
}

TEST_CASE(Application_ClosePersistenceFailuresRetryDuringCleanup) {
    const std::vector<std::pair<PersistenceFailurePoint, std::string>> cases = {
        {PersistenceFailurePoint::ChunkWrite, "application-close-chunk"},
        {PersistenceFailurePoint::JournalPublication, "application-close-journal"},
        {PersistenceFailurePoint::EntityWrite, "application-close-entity"},
    };

    for (const auto& [failurePoint, root] : cases) {
        LifecycleCalls calls;
        ScopedLifecycleCalls scopedCalls(calls);
        LogCapture logs;
        calls.persistenceRoot = root;
        calls.persistenceStorage =
            std::make_shared<FailingStorageBackend>(failurePoint);

        const int result = Rigel::runApplication(&runApplicationWithCloseFailure);

        CHECK_EQ(result, EXIT_FAILURE);
        CHECK(calls.closeFailureObserved);
        CHECK(calls.dirtyAtCloseFailure);
        CHECK(!calls.shutdownStartedAtCloseFailure);
        const size_t expectedAttempts =
            failurePoint == PersistenceFailurePoint::EntityWrite ? 3 : 2;
        CHECK_EQ(calls.persistenceAttempts, expectedAttempts);
        CHECK(!calls.shutdown.empty());
        CHECK(!calls.persistenceStorage->exists(
            root + "/entity-regions.journal"));
        CHECK(calls.persistenceStorage->exists(
            root +
            "/zones/rigel/default/entities/entityRegion_0_0_0.mem"));
        CHECK(calls.persistenceStorage->exists(
            root + "/zones/rigel/default/regions/region_0_0_0.mem"));
        CHECK(logs.output().find(
                  "Failed to save world during application close") !=
              std::string::npos);
        CHECK(logs.output().find(
                  "injected storage failure for " + root) !=
              std::string::npos);
        CHECK(logs.output().find("Application terminated successfully") ==
              std::string::npos);
    }
}
