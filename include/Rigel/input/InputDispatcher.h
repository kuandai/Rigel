#pragma once

#include "Rigel/input/InputBindings.h"

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace Rigel::Input {

class InputListener {
public:
    virtual ~InputListener() = default;

    virtual void onActionPressed(std::string_view action) {
        (void)action;
    }

    virtual void onActionReleased(std::string_view action) {
        (void)action;
    }
};

class InputDispatcher {
public:
    void setBindings(std::shared_ptr<InputBindings> bindings);
    void addListener(InputListener* listener);
    void removeListener(InputListener* listener);
    void update();
    bool isActionPressed(std::string_view action) const;
    bool isActionJustPressed(std::string_view action) const;
    bool isActionJustReleased(std::string_view action) const;

private:
    std::optional<int> resolveKey(std::string_view action) const;

    std::shared_ptr<InputBindings> m_bindings;
    std::vector<InputListener*> m_listeners;
};

} // namespace Rigel::Input
