/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#pragma once
#include <boost/asio.hpp>
#include <optional>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo-cpp/Subscription.hpp>
#include "http/EventStream.h"

namespace rousette::restconf {
class SubscribedNotifications {
public:
    SubscribedNotifications(sysrepo::Session session);
    std::shared_ptr<sysrepo::YangPushSubscription> getSubscription(const uint64_t id) const;

private:
    sysrepo::Session m_session;
    std::map<uint64_t, std::shared_ptr<sysrepo::YangPushSubscription>> m_ypSubs;
    std::optional<sysrepo::Subscription> m_notifSubs;

    sysrepo::ErrorCode establishSubscription(sysrepo::Session, const libyang::DataNode& input, libyang::DataNode& output);
};

class SubscribedNotificationStream : public http::EventStream {
public:
    SubscribedNotificationStream(
        const nghttp2::asio_http2::server::request& req,
        const nghttp2::asio_http2::server::response& res,
        std::shared_ptr<rousette::http::EventStream::Signal> signal,
        sysrepo::Session session,
        libyang::DataFormat dataFormat,
        const std::shared_ptr<sysrepo::YangPushSubscription>& subscription);
    void activate();

private:
    sysrepo::Session m_session;
    std::shared_ptr<sysrepo::YangPushSubscription> m_yangPushSubscription;
    std::shared_ptr<rousette::http::EventStream::Signal> m_signal;
    boost::asio::posix::stream_descriptor m_stream;
    libyang::DataFormat m_dataFormat;

    void cb();
};
}
