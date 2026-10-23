/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <libyang-cpp/Collection.hpp>
#include <libyang-cpp/Module.hpp>
#include <libyang-cpp/Time.hpp>
#include "conditional.h"

namespace rousette::restconf {

/** @brief Strips sysrepo's last-modif metadata from the top-level nodes and returns the most recent of the timestamps */
std::optional<std::chrono::system_clock::time_point> takeLastModified(libyang::DataNode& tree)
{
    std::optional<std::chrono::system_clock::time_point> res;

    for (const auto& node : tree.firstSibling().siblings()) {
        auto meta = node.meta();
        for (auto it = meta.begin(); it != meta.end();) {
            if (it->module().name() == "sysrepo" && it->name() == "last-modif") {
                auto timestamp = libyang::fromYangTimeFormat<std::chrono::system_clock>(it->valueStr());
                res = res ? std::max(*res, timestamp) : timestamp;
                it = meta.erase(it);
            } else {
                ++it;
            }
        }
    }

    return res;
}
}
