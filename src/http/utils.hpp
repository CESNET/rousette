/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#include <nghttp2/asio_http2_server.h>

namespace rousette::http {

struct ProtoAndHost {
    std::optional<std::string> proto;
    std::optional<std::string> host;

    bool operator==(const ProtoAndHost&) const = default;
};

std::string peer_from_request(const nghttp2::asio_http2::server::request& req);
std::vector<std::string> parseAcceptHeader(const std::string& headerValue);
ProtoAndHost getProtoAndHost(const std::string& forwardedHeaderValue);
}
