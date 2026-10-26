/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <libyang-cpp/Collection.hpp>
#include <libyang-cpp/Module.hpp>
#include <libyang-cpp/Time.hpp>
#include "conditional.h"
#include "http/utils.hpp"

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

/** @brief Evaluates the If-Modified-Since precondition (RFC 9110, sec. 13.1.3). Returns true if the response should be 304 Not Modified. */
bool isNotModified(const nghttp2::asio_http2::header_map& headers, const std::chrono::system_clock::time_point& lastModified)
{
    // If-None-Match takes precedence. We do not support entity-tags yet, so there is nothing to evaluate.
    if (headers.contains("if-none-match")) {
        return false;
    }

    //  If-Modified-Since header is ignored if it does not contain valid date or the field value has more than one member
    if (headers.count("if-modified-since") != 1) {
        return false;
    }

    auto since = http::parseHttpDate(headers.find("if-modified-since")->second.value);
    if (!since) {
        return false;
    }

    return std::chrono::floor<std::chrono::seconds>(lastModified) <= *since;
}
}
