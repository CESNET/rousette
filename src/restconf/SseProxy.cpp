/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
*/

#include <boost/asio/post.hpp>
#include <fmt/core.h>
#include <libyang-cpp/DataNode.hpp>
#include <nghttp2/asio_http2_server.h>
#include <spdlog/spdlog.h>
#include <vector>
#include "restconf/SseProxy.h"
#include "restconf/SubscribedNotifications.h"
#include "restconf/utils/sysrepo.h"
#include "restconf/utils/yang.h"

namespace rousette::restconf {

namespace {
constexpr auto subscriptionsXPath = "/ietf-subscribed-notifications:subscriptions";
constexpr auto subscriptionListXPath = "/ietf-subscribed-notifications:subscriptions/subscription";
constexpr auto receiverInstanceListXPath = "/ietf-subscribed-notifications:subscriptions/ietf-subscribed-notif-receivers:receiver-instances/receiver-instance";
constexpr auto sseProxy = "rousette:sse-proxy";
constexpr auto sseProxyFeature = "configured-subscriptions-sse-proxy";

std::string receiverInstanceXPath(const std::string& name)
{
    return fmt::format("{}[name={}]", receiverInstanceListXPath, escapeListKey(name));
}

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
    std::lock_guard lock(m_endpointsMutex);
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

    m_sub = session.onModuleChange(
        "ietf-subscribed-notifications",
        [this](sysrepo::Session changeSession, auto, auto, auto, auto, auto) {
            reconfigure(changeSession);
            return sysrepo::ErrorCode::Ok;
        },
        std::nullopt,
        0,
        sysrepo::SubscribeOptions::Enabled | sysrepo::SubscribeOptions::DoneOnly);
}

void SseProxy::reconfigure(sysrepo::Session session)
{
    SourcesByEndpoint byEndpoint;
    if (auto data = session.getData(subscriptionsXPath)) {
        byEndpoint = collectSources(m_conn, *data);
    } // nothing configured at all leaves this empty, which then removes every endpoint we have

    auto& io = *m_server.io_services().front();

    // endpoints are destroyed on the IO thread.
    std::map<std::string, std::unique_ptr<SseProxyEndpoint>> toDelete;

    {
        std::lock_guard lock(m_endpointsMutex);

        for (auto it = m_endpoints.begin(); it != m_endpoints.end();) {
            if (byEndpoint.contains(it->first)) {
                ++it;
                continue;
            }
            spdlog::info("Terminating SSE proxy endpoint '{}'", it->first);
            toDelete.insert(m_endpoints.extract(it++));
        }

        for (auto& [name, feeds] : byEndpoint) {
            const auto count = feeds.size();
            if (auto it = m_endpoints.find(name); it != m_endpoints.end()) {
                it->second->replaceFeeds(std::move(feeds));
                spdlog::info("SSE proxy '{}' now fed by {} subscription(s)", name, count);
            } else {
                m_endpoints.emplace(name, std::make_unique<SseProxyEndpoint>(std::move(feeds), io));
                spdlog::info("SSE proxy '{}' established with {} subscription(s)", name, count);
            }
        }
    }

    boost::asio::post(io, [toDelete = std::move(toDelete)]() mutable { toDelete.clear(); });
}

void SseProxy::stop()
{
    // Unsubscribe first, so that nothing reconfigures the endpoints while they are going away.
    m_sub.reset();
    std::lock_guard lock(m_endpointsMutex);
    m_endpoints.clear();
}


/** @brief May the NACM user of @p session consume the SSE proxy endpoint @p name? */
bool hasAccessToSseProxy(sysrepo::Session session, const std::string& name)
{
    if (!sseProxyEnabled(session.getContext())) {
        return false;
    }

    ScopedDatastoreSwitch dsSwitch(session, sysrepo::Datastore::Running);
    return !!session.getData(receiverInstanceXPath(name) + "/" + sseProxy + "/nacm-access-check");
}
}
