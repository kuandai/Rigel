#include "Rigel/input/InputBindingsLoader.h"

#include "Rigel/Preferences/UserPreferences.h"
#include "Rigel/input/InputBindings.h"

#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rigel::Input {
namespace {

std::string nodeText(ryml::ConstNodeRef node) {
    if (!node.readable() || !node.has_val() || node.is_container()) {
        return {};
    }
    return std::string(node.val().data(), node.val().size());
}

[[noreturn]] void invalidBindings(
    std::string_view assetId,
    const std::string& problem) {
    throw std::runtime_error(
        "Input bindings '" + std::string(assetId) + "' " + problem);
}

} // namespace

std::shared_ptr<Asset::AssetBase> InputBindingsLoader::load(
    const Asset::LoadContext& ctx) {
    if (!ctx.config.readable() || !ctx.config.has_child("bindings")) {
        invalidBindings(ctx.id, "must contain a bindings mapping");
    }

    const ryml::ConstNodeRef bindingsNode = ctx.config["bindings"];
    if (!bindingsNode.is_map()) {
        invalidBindings(ctx.id, "bindings must be a mapping");
    }

    auto bindings = std::make_shared<InputBindings>();
    std::unordered_set<std::string> actions;
    for (const ryml::ConstNodeRef entry : bindingsNode.children()) {
        if (!entry.has_key()) {
            invalidBindings(ctx.id, "contains an action without a name");
        }
        const std::string action(entry.key().data(), entry.key().size());
        if (action.empty() || !actions.insert(action).second) {
            invalidBindings(
                ctx.id,
                "contains an empty or duplicate action '" + action + "'");
        }
        if (!entry.is_keyval() && !entry.is_seq()) {
            invalidBindings(
                ctx.id,
                "action '" + action + "' must be a token or token list");
        }
        if (entry.is_seq() &&
            entry.num_children() >
                Preferences::kMaximumBindingsPerAction) {
            invalidBindings(
                ctx.id,
                "action '" + action + "' has too many binding tokens");
        }

        std::vector<PhysicalInput> inputs;
        const auto appendToken = [&](ryml::ConstNodeRef tokenNode) {
            const std::string token = nodeText(tokenNode);
            const auto decoded = decodeBindingToken(token);
            if (!decoded) {
                invalidBindings(
                    ctx.id,
                    "action '" + action + "' has unknown token '" +
                        token + "'");
            }
            inputs.push_back(*decoded);
        };
        if (entry.is_keyval()) {
            appendToken(entry);
        } else {
            inputs.reserve(entry.num_children());
            for (const ryml::ConstNodeRef tokenNode : entry.children()) {
                appendToken(tokenNode);
            }
        }
        bindings->setBindings(action, std::move(inputs));
    }
    return bindings;
}

} // namespace Rigel::Input
