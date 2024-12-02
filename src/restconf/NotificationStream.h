/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#pragma once
#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/uuid/random_generator.hpp>
#include <optional>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo-cpp/Subscription.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include <thread>
#include "configure.cmake.h"
#include "http/EventStream.h"

namespace libyang {
enum class DataFormat;
}

namespace nghttp2::asio_http2::server {
class http2;
}

namespace rousette::restconf {

/** @brief Subscribes to NETCONF notifications and sends them via HTTP/2 Event stream.
 *
 * The class must be instantiated as a shared_ptr. Once the instance is created
 * and activate() is called, a self-referencing shared_ptr instance is bound to
 * HTTP-level callbacks which keep an instance around as long as the HTTP
 * request is alive. This magic is performed by the base class (EventStream).
 *
 * The notification signal is required to be passed from the outside context because
 * we have to pass already constructed signal to parent class.
 *
 * @see rousette::http::EventStream
 * */
class NotificationStream : public rousette::http::EventStream {
    std::shared_ptr<rousette::http::EventStream::EventSignal> m_notificationSignal;
    sysrepo::Session m_session;
    libyang::DataFormat m_dataFormat;
    std::optional<std::string> m_filter;
    std::optional<sysrepo::NotificationTimeStamp> m_startTime;
    std::optional<sysrepo::NotificationTimeStamp> m_stopTime;
    std::optional<sysrepo::Subscription> m_notifSubs;

public:
    NotificationStream(
        const nghttp2::asio_http2::server::request& req,
        const nghttp2::asio_http2::server::response& res,
        rousette::http::EventStream::Termination& termination,
        std::shared_ptr<rousette::http::EventStream::EventSignal> signal,
        sysrepo::Session sess,
        libyang::DataFormat dataFormat,
        const std::optional<std::string>& filter,
        const std::optional<sysrepo::NotificationTimeStamp>& startTime,
        const std::optional<sysrepo::NotificationTimeStamp>& stopTime);
    void activate();
};

/** Dynamic subscriptions manager.
 *
 * Stores all dynamic subscriptions and provides a way to retrieve them by the UUID.
 * */
class DynamicSubscriptions {
public:
    struct SubscriptionData {
        sysrepo::DynamicSubscription subscription;
        libyang::DataFormat dataFormat; ///< Encoding of the notification stream
        boost::uuids::uuid uuid; ///< UUID that used in the URI for GET to make guessing hard
        std::string user; ///< User who initiated the establish-subscription RPC
        boost::asio::system_timer clientInactiveTimer; //< Timer used for auto-destruction of subscriptions that are unused
        std::function<void()> onClientInactiveCallback;

        SubscriptionData(
            sysrepo::DynamicSubscription sub,
            libyang::DataFormat format,
            boost::uuids::uuid uuid,
            const std::string& user,
            boost::asio::io_context& io,
            std::function<void()> onInactiveCallback);
        ~SubscriptionData();
        void clientDisconnected();
        void clientConnected();
        void setupInactivityTimer();
        friend std::ostream& operator<<(std::ostream& os, const SubscriptionData& sub);
    };

    DynamicSubscriptions(sysrepo::Session& session, const std::string& streamRootUri, const nghttp2::asio_http2::server::http2& server);
    std::shared_ptr<SubscriptionData> getSubscriptionForUser(const boost::uuids::uuid& uuid, const std::optional<std::string>& user);
    void establishSubscription(sysrepo::Session& session, const libyang::DataFormat requestEncoding, const libyang::DataNode& rpcInput, libyang::DataNode& rpcOutput);

private:
    const nghttp2::asio_http2::server::http2& m_server; // for getting io_contexts
    std::mutex m_mutex;
    std::string m_restconfStreamUri;
    std::map<boost::uuids::uuid, std::shared_ptr<SubscriptionData>> m_subscriptions;
    boost::uuids::random_generator m_uuidGenerator;
    std::optional<sysrepo::Subscription> m_notificationStreamListSub;

    void terminateSubscription(const uint32_t subId);
};

/** @brief Subscribes to subscribed notification and sends the notifications via HTTP/2 Event stream.
 *
 * @see rousette::http::EventStream
 * @see rousette::http::NotificationStream
 * */
class DynamicSubscriptionHttpStream : public http::EventStream {
public:
    DynamicSubscriptionHttpStream(
        const nghttp2::asio_http2::server::request& req,
        const nghttp2::asio_http2::server::response& res,
        rousette::http::EventStream::Termination& termination,
        std::shared_ptr<rousette::http::EventStream::EventSignal> signal,
        const std::shared_ptr<DynamicSubscriptions::SubscriptionData>& subscriptionData);
    void activate();
    ~DynamicSubscriptionHttpStream();

private:
    std::shared_ptr<DynamicSubscriptions::SubscriptionData> m_subscriptionData;
    std::shared_ptr<rousette::http::EventStream::EventSignal> m_signal;
    boost::asio::posix::stream_descriptor m_stream;

    void awaitNextNotification();
};

void notificationStreamList(sysrepo::Session& session, std::optional<libyang::DataNode>& parent, const std::string& streamsPrefix);
libyang::DataNode replaceStreamLocations(const std::optional<std::string>& schemeAndHost, libyang::DataNode& node);
}
