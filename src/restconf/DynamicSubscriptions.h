/*
 * Copyright (C) 2025 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
*/
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/uuid/random_generator.hpp>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <sysrepo-cpp/Subscription.hpp>
#include "http/EventStream.h"
#include "restconf/SubscriptionBroadcaster.h"

namespace libyang {
enum class DataFormat;
}

namespace nghttp2::asio_http2::server {
class http2;
}

namespace rousette::restconf {

/** @brief A configured filter referenced by name from the RPC input (stream-filter-name / selection-filter-ref). */
struct ReferencedFilter {
    /** @brief Which configured filter list the subscription refers to. */
    enum class Kind {
        StreamFilter, ///< ietf-subscribed-notifications stream-filter
        SelectionFilter, ///< ietf-yang-push selection-filter
    };

    std::string name;
    Kind kind;

    std::string configuredXPath() const;
};

/** Dynamic subscriptions manager.
 *
 * Stores all dynamic subscriptions and provides a way to retrieve them by the UUID.
 * */
class DynamicSubscriptions {
public:
    struct SubscriptionData : public std::enable_shared_from_this<SubscriptionData> {
        mutable std::mutex mutex;
        sysrepo::DynamicSubscription subscription;
        libyang::DataFormat dataFormat; ///< Encoding of the notification stream
        boost::uuids::uuid uuid; ///< UUID is part of the GET URI, it identifies subscriptions for clients
        std::string user; ///< User who initiated the establish-subscription RPC
        std::optional<ReferencedFilter> configuredFilter; ///< The configured filter this subscription refers to, if any
        std::weak_ptr<http::EventStream::EventSignal> notificationSink; ///< Event signal of the connected client's stream

        enum class State {
            Start, ///< Subscription is ready to be consumed by a client
            ReceiverActive, ///< Subscription is being consumed by a client
            Terminating, ///< Subscription is being terminated by the server shutdown
        } state;

        std::chrono::seconds inactivityTimeout; ///< Time after which the subscription is considered inactive and can be removed if no client is connected
        boost::asio::steady_timer clientInactiveTimer; ///< Timer used for auto-destruction of subscriptions that are unused
        std::function<void()> onClientInactiveCallback;

        SubscriptionData(
            sysrepo::DynamicSubscription sub,
            libyang::DataFormat format,
            boost::uuids::uuid uuid,
            const std::string& user,
            const std::optional<ReferencedFilter>& configuredFilter,
            boost::asio::io_context& io,
            std::chrono::seconds inactivityTimeout,
            std::function<void()> onClientInactiveCallback);
        ~SubscriptionData();
        void clientDisconnected();
        void clientConnected(const std::shared_ptr<http::EventStream::EventSignal>& notificationSink);
        void terminate(const std::optional<std::string>& reason = std::nullopt);
        bool isReadyToAcceptClient() const;
        void stop();

    private:
        void inactivityStart();
        void inactivityCancel();
    };

    DynamicSubscriptions(sysrepo::Session& session, const std::string& streamRootUri, const nghttp2::asio_http2::server::http2& server, const std::chrono::seconds inactivityTimeout);
    ~DynamicSubscriptions();
    std::shared_ptr<SubscriptionData> getSubscriptionForUser(const boost::uuids::uuid& uuid, const std::optional<std::string>& user);
    std::shared_ptr<SubscriptionData> getSubscriptionForUser(const uint32_t subId, const std::optional<std::string>& user);
    void establishSubscription(sysrepo::Session& session, const std::optional<std::string>& requestSchemeAndHost, const libyang::DataFormat requestEncoding, const libyang::DataNode& rpcInput, libyang::DataNode& rpcOutput);
    void stop();
    void deleteSubscription(sysrepo::Session& session, const std::optional<std::string>& requestSchemeAndHost, libyang::DataFormat, const libyang::DataNode& rpcInput, libyang::DataNode&);
    void modifySubscription(sysrepo::Session& session, const std::optional<std::string>& requestSchemeAndHost, libyang::DataFormat, const libyang::DataNode& rpcInput, libyang::DataNode&);

private:
    std::mutex m_mutex; ///< Lock for shared data (subscriptions storage and uuid generator)
    std::string m_restconfStreamUri;
    const nghttp2::asio_http2::server::http2& m_server;
    std::map<boost::uuids::uuid, std::shared_ptr<SubscriptionData>> m_subscriptions;
    boost::uuids::random_generator m_uuidGenerator;
    std::chrono::seconds m_inactivityTimeout;
    std::optional<sysrepo::Subscription> m_notificationStreamListSub;
    std::optional<sysrepo::Subscription> m_filtersChangeSub;

    void terminateSubscription(const uint32_t subId);
    sysrepo::ErrorCode onConfiguredFilterChange(sysrepo::Session session);

    boost::uuids::uuid makeUUID();
};

/** @brief Subscribes to sysrepo's subscribed notification and sends the notifications via HTTP/2 Event stream.
 *
 * @see rousette::http::EventStream
 * @see rousette::http::NotificationStream
 * */
class DynamicSubscriptionHttpStream : public http::EventStream {
public:
    static std::shared_ptr<DynamicSubscriptionHttpStream> create(
        const nghttp2::asio_http2::server::request& req,
        const nghttp2::asio_http2::server::response& res,
        rousette::http::EventStream::Termination& termination,
        const std::chrono::seconds keepAlivePingInterval,
        const std::shared_ptr<DynamicSubscriptions::SubscriptionData>& subscriptionData);

private:
    std::shared_ptr<DynamicSubscriptions::SubscriptionData> m_subscriptionData;
    std::shared_ptr<rousette::http::EventStream::EventSignal> m_signal;
    boost::asio::io_context& m_io;
    std::unique_ptr<SubscriptionBroadcaster> m_broadcaster;

protected:
    DynamicSubscriptionHttpStream(
        const nghttp2::asio_http2::server::request& req,
        const nghttp2::asio_http2::server::response& res,
        rousette::http::EventStream::Termination& termination,
        std::shared_ptr<rousette::http::EventStream::EventSignal> signal,
        const std::chrono::seconds keepAlivePingInterval,
        const std::shared_ptr<DynamicSubscriptions::SubscriptionData>& subscriptionData);
    void activate();
};
}
