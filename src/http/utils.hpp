/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#include <chrono>
#include <nghttp2/asio_http2_server.h>
#include <optional>

namespace rousette::http {

struct ProtoAndHost {
    std::optional<std::string> proto;
    std::optional<std::string> host;

    bool operator==(const ProtoAndHost&) const = default;
};

struct EntityTag {
    bool weak;
    std::string opaqueTag; ///< Including the enclosing double quotes

    bool operator==(const EntityTag&) const = default;
};

std::string peer_from_request(const nghttp2::asio_http2::server::request& req);
std::vector<std::string> parseAcceptHeader(const std::string& headerValue);
ProtoAndHost parseForwardedHeader(const std::string& headerValue);
std::optional<std::string> parseUrlPrefix(const nghttp2::asio_http2::header_map& headers);
std::optional<std::string> getHeaderValue(const nghttp2::asio_http2::header_map& headers, const std::string& header);
std::string httpDate(const std::chrono::system_clock::time_point& time);
std::optional<std::chrono::sys_seconds> parseHttpDate(const std::string& str);
std::optional<std::vector<EntityTag>> parseEntityTags(const std::string& headerValue);
bool weakMatch(const EntityTag& a, const EntityTag& b);
}
