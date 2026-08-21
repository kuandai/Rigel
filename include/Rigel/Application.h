#pragma once
#include <memory>

namespace Rigel {

class Application {
public:
    Application();
    ~Application();

    void run();

private:
    void initialize();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
