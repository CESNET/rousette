/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
*/
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Session.hpp>
#include "http/EventStream.h"
#include "restconf/SseProxyEndpoint.h"

namespace nghttp2::asio_http2::server {
class http2;
}

namespace rousette::restconf {

/** @brief Registry of the SSE proxy endpoints. */
class SseProxy {
public:
    SseProxy(sysrepo::Connection conn, nghttp2::asio_http2::server::http2& server);
    /** @brief The endpoint named @p name, or null. Owned by this registry, so do not store the pointer; attach to its
     * events() and termination() and forget it. */
    SseProxyEndpoint* find(const std::string& name) const;

    void start();
    void stop();

    /** @brief Established subscriptions, grouped by the endpoint they feed. */
    using SourcesByEndpoint = std::map<std::string, std::vector<SseProxyEndpoint::SourceSubscription>>;

private:
    sysrepo::Connection m_conn;
    nghttp2::asio_http2::server::http2& m_server;
    std::map<std::string, std::unique_ptr<SseProxyEndpoint>> m_endpoints;
};

bool hasAccessToSseProxy(sysrepo::Session session, const std::string& name);
}
