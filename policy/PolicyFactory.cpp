#include "policy/PolicyFactory.h"

#include "policy/LRUPolicy.h"

#include <algorithm>

namespace bufman {

PolicyFactory& PolicyFactory::instance() {
    static PolicyFactory factory;
    return factory;
}

PolicyFactory::PolicyFactory() {
    creators_["lru"] = [] { return std::make_unique<LRUPolicy>(); };
}

void PolicyFactory::register_policy(const std::string& name, Creator creator) {
    creators_[name] = std::move(creator);
}

std::unique_ptr<ReplacementPolicy> PolicyFactory::create(
        const std::string& name, std::string& error) const {
    error.clear();
    const auto found = creators_.find(name);
    if (found == creators_.end()) {
        std::string available;
        for (const auto& entry : known()) {
            if (!available.empty()) {
                available += ", ";
            }
            available += entry;
        }
        error = "unknown replacement policy '" + name +
                "' (available: " + available + ")";
        return nullptr;
    }
    return found->second();
}

std::vector<std::string> PolicyFactory::known() const {
    std::vector<std::string> names;
    names.reserve(creators_.size());
    for (const auto& entry : creators_) {
        names.push_back(entry.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

}
