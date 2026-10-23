/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <format>
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

/** @brief Formats the time as the IMF-fixdate (RFC 9110, sec. 5.6.7), e.g., "Thu, 26 Jan 2017 16:00:14 GMT" */
std::string httpDate(const std::chrono::system_clock::time_point& time)
{
    return std::format("{:%a, %d %b %Y %H:%M:%S} GMT", std::chrono::floor<std::chrono::seconds>(time));
}
}
