#pragma once
#include <memory>

namespace Rigel {

class ApplicationTestAccess;

class Application {
public:
    Application();
    ~Application();

    void run();

private:
    struct Impl;
    explicit Application(std::unique_ptr<Impl> impl);

    void initialize();

    friend class ApplicationTestAccess;
    std::unique_ptr<Impl> m_impl;
};

}
