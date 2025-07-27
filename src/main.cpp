#include "Rigel/Application.h"
#include <spdlog/spdlog.h>

int main() {
    try {
        Rigel::Application app;
        app.run();
    } catch (const std::exception& e) {
        spdlog::error("Application error: {}", e.what());
        return -1;
    }
    return 0;
}
