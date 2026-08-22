#pragma once
#include <memory>

namespace Rigel {

class ApplicationTestAccess;

class Application {
public:
    Application();
    ~Application();

    void run();
    void close();

private:
    enum class Initialization {
        Run,
        Skip,
    };

    struct Impl;
    explicit Application(std::unique_ptr<Impl> impl);
    Application(std::unique_ptr<Impl> impl, Initialization initialization);

    void initialize();

    friend class ApplicationTestAccess;
    std::unique_ptr<Impl> m_impl;
};

}
