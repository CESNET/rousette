/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#include <boost/lexical_cast.hpp>
#include <regex>
#include "http/utils.hpp"

namespace rousette::http {

/** @short Reasonably unique, but free-form string for identifying client connections */
std::string peer_from_request(const nghttp2::asio_http2::server::request& req)
{
    std::string peer = boost::lexical_cast<std::string>(req.remote_endpoint());
    if (auto forwarded = req.header().find("forwarded"); forwarded != req.header().end()) {
        peer += '(' + forwarded->second.value + ')';
    }
    return peer;
}

/** @short Returns a vector of media types (strings) parsed from accept header sorted by preference (quality) descending.
 *
 * @return empty vector for invalid header values, otherwise sorted vector by quality descending (also order is kept for same quality entries)
 */
std::vector<std::string> parseAcceptHeader(const std::string& headerValue)
{
    struct MediaType {
        std::string mediaType;
        double quality;
    };

    std::vector<MediaType> mediaTypes;
    static const std::string name = "(?:(?:[A-z0-9][A-z0-9!#$&\\-^_+]*)|\\*)";
    static const std::regex regex("(" + name + "/" + name + ")(?:\\s*;\\s*q=(\\d(?:.\\d*)?))?");

    for (auto iter = std::sregex_iterator(headerValue.begin(), headerValue.end(), regex); iter != std::sregex_iterator(); ++iter) {
        std::smatch match = *iter;
        double quality = match[2].matched ? std::stof(match[2]) : 1.0;

        mediaTypes.push_back({match[1], quality});
    }

    std::stable_sort(mediaTypes.begin(), mediaTypes.end(), [](const auto& a, const auto& b) { return a.quality > b.quality; }); // if two types share the same quality then prefer the first

    std::vector<std::string> res;
    res.reserve(mediaTypes.size());

    std::transform(mediaTypes.begin(), mediaTypes.end(), std::back_inserter(res), [](const auto& e) { return e.mediaType; });
    return res;
}
}
