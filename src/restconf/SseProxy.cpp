/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
*/

#include <libyang-cpp/DataNode.hpp>
#include <nghttp2/asio_http2_server.h>
#include <spdlog/spdlog.h>
#include <vector>
#include "restconf/SseProxy.h"
#include "restconf/SubscribedNotifications.h"

namespace rousette::restconf {

namespace {
constexpr auto subscriptionsXPath = "/ietf-subscribed-notifications:subscriptions";
constexpr auto subscriptionListXPath = "/ietf-subscribed-notifications:subscriptions/subscription";
constexpr auto receiverInstanceListXPath = "/ietf-subscribed-notifications:subscriptions/ietf-subscribed-notif-receivers:receiver-instances/receiver-instance";
constexpr auto sseProxy = "rousette:sse-proxy";
constexpr auto sseProxyFeature = "configured-subscriptions-sse-proxy";

/** @brief Is the YANG feature which brings in the rousette:sse-proxy transport enabled? */
bool sseProxyEnabled(const libyang::Context& ctx)
{
    auto mod = ctx.getModuleImplemented("rousette");
    return mod && mod->featureEnabled(sseProxyFeature);
}

/** @brief Established subscriptions, grouped by the endpoint they feed. */
using SourcesByEndpoint = std::map<std::string, std::vector<SseProxyEndpoint::SourceSubscription>>;

/** @brief Establishes a subscription for every configured subscription of ours, grouped by the endpoint it feeds. */
SourcesByEndpoint collectSources(sysrepo::Connection& conn, const libyang::DataNode& data)
{
    // collect all receiver-instance where name is rousette:sse-proxy along with nacm users
    std::map<std::string, std::string> nacmUserByEndpoint;
    for (const auto& instance : data.findXPath(receiverInstanceListXPath)) {
        if (auto user = instance.findPath(std::string{sseProxy} + "/nacm-username")) {
            nacmUserByEndpoint.emplace(instance.findPath("name")->asTerm().valueStr(), user->asTerm().valueStr());
        }
    }

    // create endpoints
    SourcesByEndpoint byEndpoint;
    for (const auto& entry : nacmUserByEndpoint) {
        byEndpoint.try_emplace(entry.first, std::vector<SseProxyEndpoint::SourceSubscription>{});
    }

    // Group the configured subscriptions by the endpoint their receivers reference.
    // A subscription feeding two of our endpoints is established twice, once per endpoint.
    for (const auto& sub : data.findXPath(subscriptionListXPath)) {
        for (const auto& receiver : sub.findXPath("receivers/receiver")) {
            auto ref = receiver.findPath("ietf-subscribed-notif-receivers:receiver-instance-ref");
            if (!ref) {
                continue;
            }

            const auto endpoint = ref->asTerm().valueStr();
            auto nacmUser = nacmUserByEndpoint.find(endpoint);
            if (nacmUser == nacmUserByEndpoint.end()) {
                continue; // not ours, e.g., a receiver-instance delivered over UDP by sysrepo-notifd
            }

            try {
                auto subSession = conn.sessionStart();
                subSession.setNacmUser(nacmUser->second);

                byEndpoint[endpoint].push_back({makeSubscription(subSession, sub), subscriptionEncoding(sub, libyang::DataFormat::JSON)});
            } catch (const std::exception& e) {
                spdlog::error("SSE proxy '{}': could not establish a subscription: {}", endpoint, e.what());
            }
        }
    }

    return byEndpoint;
}
}

SseProxy::SseProxy(sysrepo::Connection conn, nghttp2::asio_http2::server::http2& server)
    : m_conn(std::move(conn))
    , m_server(server)
{
}

SseProxyEndpoint* SseProxy::find(const std::string& name) const
{
    if (auto it = m_endpoints.find(name); it != m_endpoints.end()) {
        return it->second.get();
    }
    return nullptr;
}

void SseProxy::start()
{
    auto session = m_conn.sessionStart(sysrepo::Datastore::Running);

    if (!sseProxyEnabled(session.getContext())) {
        spdlog::info("The {} feature is not enabled, not serving any SSE proxy endpoints", sseProxyFeature);
        return;
    }

    // TODO: This does not react to changes now.
    auto data = session.getData(subscriptionsXPath);
    if (!data) {
        return;
    }

    auto& io = *m_server.io_services().front();
    for (auto& [name, feeds] : collectSources(m_conn, *data)) {
        const auto count = feeds.size();
        m_endpoints.emplace(name, std::make_unique<SseProxyEndpoint>(std::move(feeds), io));
        spdlog::info("SSE proxy '{}' established with {} subscription(s)", name, count);
    }
}

void SseProxy::stop()
{
    m_endpoints.clear();
}

}
