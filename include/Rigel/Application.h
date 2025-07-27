#pragma once
#include <memory>

namespace Rigel {

class Application {
public:
    Application();
    ~Application();
    
    void run();
    
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}