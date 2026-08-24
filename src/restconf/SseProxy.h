/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
*/
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo-cpp/Subscription.hpp>
#include <vector>
#include "restconf/SseProxyEndpoint.h"

namespace nghttp2::asio_http2::server {
class http2;
}

namespace rousette::restconf {

/** @brief Registry of the SSE proxy endpoints. */
class SseProxy {
public:
    SseProxy(sysrepo::Connection conn, nghttp2::asio_http2::server::http2& server);
    SseProxyEndpoint* find(const std::string& name) const;

    void start();
    void stop();

    /** @brief Established subscriptions, grouped by the endpoint they feed. */
    using SourcesByEndpoint = std::map<std::string, std::vector<SseProxyEndpoint::SourceSubscription>>;

private:
    void reconfigure(sysrepo::Session session);

    sysrepo::Connection m_conn;
    nghttp2::asio_http2::server::http2& m_server;
    mutable std::mutex m_endpointsMutex;
    std::map<std::string, std::unique_ptr<SseProxyEndpoint>> m_endpoints;
    std::optional<sysrepo::Subscription> m_sub; ///< last, so that no callback can run against endpoints which are gone
};

bool hasAccessToSseProxy(sysrepo::Session session, const std::string& name);
}
