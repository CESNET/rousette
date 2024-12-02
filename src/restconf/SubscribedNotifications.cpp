/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <nghttp2/asio_http2_server.h>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/Subscription.hpp>
#include "restconf/SubscribedNotifications.h"
#include "restconf/utils/yang.h"

namespace rousette::restconf {
SubscribedNotifications::SubscribedNotifications(sysrepo::Session session)
    : m_session(std::move(session))
{
    m_notifSubs = m_session.onRPCAction("/ietf-subscribed-notifications:establish-subscription",
                                        [&](sysrepo::Session session, auto, auto, const libyang::DataNode& input, auto, auto, libyang::DataNode output) {
                                            return establishSubscription(session, input, output);
                                        });

    m_notifSubs->onRPCAction("/ietf-subscribed-notifications:modify-subscription",
                             [&](auto, auto, auto, auto, auto, auto, auto) {
                                 return sysrepo::ErrorCode::Unsupported;
                             });
    m_notifSubs->onRPCAction("/ietf-subscribed-notifications:delete-subscription",
                             [&](auto, auto, auto, auto, auto, auto, auto) {
                                 return sysrepo::ErrorCode::Unsupported;
                             });
    m_notifSubs->onRPCAction("/ietf-subscribed-notifications:kill-subscription",
                             [&](auto, auto, auto, auto, auto, auto, auto) {
                                 return sysrepo::ErrorCode::Unsupported;
                             });
    m_notifSubs->onRPCAction("/ietf-yang-push:resync-subscription",
                             [&](auto, auto, auto, auto, auto, auto, auto) {
                                 return sysrepo::ErrorCode::Unsupported;
                             });
}

sysrepo::ErrorCode SubscribedNotifications::establishSubscription(sysrepo::Session, const libyang::DataNode& input, libyang::DataNode& output)
{
    spdlog::trace("srsn establish-subscription input: {}", *input.printStr(libyang::DataFormat::JSON, libyang::PrintFlags::Shrink));

    auto ypSub = std::make_shared<sysrepo::YangPushSubscription>(m_session.yangPushOnChange(sysrepo::Datastore::Running, std::nullopt, false, std::chrono::milliseconds{0}, std::nullopt));

    output.newPath("id", std::to_string(ypSub->subscriptionId()), libyang::CreationOptions::Output);
    output.newPath("ietf-restconf-subscribed-notifications:uri", "/streams/subscribed/1", libyang::CreationOptions::Output);

    m_ypSubs.emplace(ypSub->subscriptionId(), std::move(ypSub));
    return sysrepo::ErrorCode::Ok;
}

std::shared_ptr<sysrepo::YangPushSubscription> SubscribedNotifications::getSubscription(const uint64_t id) const
{
    if (auto it = m_ypSubs.find(id); it != m_ypSubs.end()) {
        return it->second;
    }

    throw std::runtime_error("Subscription not found");
}

SubscribedNotificationStream::SubscribedNotificationStream(
    const nghttp2::asio_http2::server::request& req,
    const nghttp2::asio_http2::server::response& res,
    std::shared_ptr<rousette::http::EventStream::Signal> signal,
    sysrepo::Session session,
    libyang::DataFormat dataFormat,
    const std::shared_ptr<sysrepo::YangPushSubscription>& yangPushSubscription)
    : EventStream(req, res, *signal)
    , m_session(std::move(session))
    , m_yangPushSubscription(yangPushSubscription)
    , m_signal(signal)
    , m_stream(res.io_service(), m_yangPushSubscription->fd())
    , m_dataFormat(dataFormat)
{
}

void SubscribedNotificationStream::activate()
{
    m_stream.async_read_some(boost::asio::null_buffers(), [this](const boost::system::error_code& err, auto) {
        spdlog::trace("SubscribedNotificationStream::activate async_read_some: {}", err.message());
        if (err == boost::asio::error::operation_aborted) {
            return;
        }
        cb();
    });

    EventStream::activate();
}

void SubscribedNotificationStream::cb()
{
    // read one notification
    m_yangPushSubscription->processEvents([this](const std::optional<libyang::DataNode>& notificationTree, const sysrepo::NotificationTimeStamp& time) {
        (*m_signal)(rousette::restconf::as_restconf_notification(m_session.getContext(), m_dataFormat, *notificationTree, time));
    });

    // and register fd watcher for the next notification
    m_stream.async_read_some(boost::asio::null_buffers(), [this](const boost::system::error_code& err, auto) {
        spdlog::trace("SubscribedNotificationStream::activate async_read_some: {}", err.message());
        if (err == boost::asio::error::operation_aborted) {
            return;
        }
        cb();
    });
}
}
