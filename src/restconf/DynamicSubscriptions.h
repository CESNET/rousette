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
#include <sysrepo-cpp/Subscription.hpp>
#include "http/EventStream.h"

namespace libyang {
enum class DataFormat;
}

namespace rousette::restconf {

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

        enum class State {
            Start, ///< Subscription is ready to be consumed by a client
            ReceiverActive, ///< Subscription is being consumed by a client
            Terminating, ///< Subscription is being terminated by the server shutdown
        } state;

        SubscriptionData(
            sysrepo::DynamicSubscription sub,
            libyang::DataFormat format,
            boost::uuids::uuid uuid,
            const std::string& user);
        ~SubscriptionData();
        void clientDisconnected();
        void clientConnected();
        void terminate(const std::optional<std::string>& reason = std::nullopt);
        bool isReadyToAcceptClient() const;
    };

    DynamicSubscriptions(const std::string& streamRootUri);
    ~DynamicSubscriptions();
    std::shared_ptr<SubscriptionData> getSubscriptionForUser(const boost::uuids::uuid& uuid, const std::optional<std::string>& user);
    void establishSubscription(sysrepo::Session& session, const libyang::DataFormat requestEncoding, const libyang::DataNode& rpcInput, libyang::DataNode& rpcOutput);

private:
    std::mutex m_mutex; ///< Lock for shared data (subscriptions storage and uuid generator)
    std::string m_restconfStreamUri;
    std::map<boost::uuids::uuid, std::shared_ptr<SubscriptionData>> m_subscriptions;
    boost::uuids::random_generator m_uuidGenerator;

    void terminateSubscription(const uint32_t subId);

    boost::uuids::uuid makeUUID();
};

/** @brief Subscribes to sysrepo's subscribed notification and sends the notifications via HTTP/2 Event stream.
 *
 * @see rousette::http::EventStream
 * @see rousette::http::NotificationStream
 * */
class DynamicSubscriptionHttpStream : public http::EventStream {
public:
    ~DynamicSubscriptionHttpStream();

    static std::shared_ptr<DynamicSubscriptionHttpStream> create(
        const nghttp2::asio_http2::server::request& req,
        const nghttp2::asio_http2::server::response& res,
        rousette::http::EventStream::Termination& termination,
        const std::chrono::seconds keepAlivePingInterval,
        const std::shared_ptr<DynamicSubscriptions::SubscriptionData>& subscriptionData);

private:
    std::shared_ptr<DynamicSubscriptions::SubscriptionData> m_subscriptionData;
    std::shared_ptr<rousette::http::EventStream::EventSignal> m_signal;
    boost::asio::posix::stream_descriptor m_stream;

    void awaitNextNotification();

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
