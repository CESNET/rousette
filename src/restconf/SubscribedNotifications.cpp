/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <boost/uuid/uuid_io.hpp>
#include <nghttp2/asio_http2_server.h>
#include <libyang-cpp/Time.hpp>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/Subscription.hpp>
#include "restconf/Exceptions.h"
#include "restconf/SubscribedNotifications.h"
#include "restconf/utils/sysrepo.h"
#include "restconf/utils/yang.h"

namespace {
sysrepo::Datastore datastoreFromString(const std::string& str)
{
    if (str == "ietf-datastores:running") {
        return sysrepo::Datastore::Running;
    } else if (str == "ietf-datastores:startup") {
        return sysrepo::Datastore::Startup;
    } else if (str == "ietf-datastores:candidate") {
        return sysrepo::Datastore::Candidate;
    } else if (str == "ietf-datastores:operational") {
        return sysrepo::Datastore::Operational;
    } else if (str == "ietf-datastores:factory-default") {
        return sysrepo::Datastore::FactoryDefault;
    }

    throw rousette::restconf::ErrorResponse(400, "application", "invalid-value", "Invalid datastore value");
}
}

namespace rousette::restconf {
SubscribedNotifications::SubscribedNotifications(sysrepo::Session session)
    : m_session(std::move(session))
    , m_uuidGenerator(boost::uuids::random_generator())
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
}

sysrepo::ErrorCode SubscribedNotifications::establishSubscription(sysrepo::Session, const libyang::DataNode& input, libyang::DataNode& output)
{
    spdlog::trace("srsn establish-subscription input: {}", *input.printStr(libyang::DataFormat::JSON, libyang::PrintFlags::Shrink));

    auto datastoreNode = input.findPath("ietf-yang-push:datastore");
    if (!datastoreNode) {
        throw ErrorResponse(400, "application", "missing-datastore", "Missing datastore parameter");
    }

    std::optional<sysrepo::NotificationTimeStamp> stopTime;
    if (auto stopTimeNode = input.findPath("stop-time")) {
        stopTime = libyang::fromYangTimeFormat<sysrepo::NotificationTimeStamp::clock>(stopTimeNode->asTerm().valueStr());
    }

    std::shared_ptr<sysrepo::YangPushSubscription> ypSub;
    auto ypPeriodicNode = input.findPath("ietf-yang-push:periodic");
    auto ypOnChangeNode = input.findPath("ietf-yang-push:on-change");

    ScopedDatastoreSwitch sw(m_session, datastoreFromString(datastoreNode->asTerm().valueStr()));
    if (ypPeriodicNode) {
        auto periodNode = ypPeriodicNode->findPath("period"); // in centiseconds
        auto period = std::chrono::milliseconds(std::stoll(periodNode->asTerm().valueStr()) * 10);

        std::optional<sysrepo::NotificationTimeStamp> anchorTime;
        if (auto anchorTimeNode = ypPeriodicNode->findPath("anchor-time")) {
            anchorTime = libyang::fromYangTimeFormat<sysrepo::NotificationTimeStamp::clock>(anchorTimeNode->asTerm().valueStr());
        }

        ypSub = std::make_shared<sysrepo::YangPushSubscription>(m_session.yangPushPeriodic(std::nullopt, period, anchorTime, stopTime));
    } else if (ypOnChangeNode) {
        sysrepo::SyncOnStart syncOnStart = sysrepo::SyncOnStart::No;
        if (auto syncOnStartNode = ypOnChangeNode->findPath("sync-on-start"); syncOnStartNode) {
            if (syncOnStartNode->asTerm().valueStr() == "true") {
                syncOnStart = sysrepo::SyncOnStart::Yes;
            }
        }

        std::optional<std::chrono::milliseconds> dampeningPeriod; // in centiseconds
        if (auto dampeningPeriodNode = ypOnChangeNode->findPath("dampening-period")) {
            dampeningPeriod = std::chrono::milliseconds(std::stoll(dampeningPeriodNode->asTerm().valueStr()) * 10);
        }

        ypSub = std::make_shared<sysrepo::YangPushSubscription>(m_session.yangPushOnChange(std::nullopt, dampeningPeriod, syncOnStart, stopTime));
    } else {
        throw ErrorResponse(400, "application", "missing-datastore", "Missing datastore parameter");
    }

    // Generate a new UUID associated with the subscription. The UUID will be used as a part of the URI so that the URI is not predictable (RFC 8650, section 5)
    auto uuid = m_uuidGenerator();

    output.newPath("id", std::to_string(ypSub->subscriptionId()), libyang::CreationOptions::Output);
    output.newPath("ietf-restconf-subscribed-notifications:uri", "/streams/subscribed/" + boost::uuids::to_string(uuid), libyang::CreationOptions::Output);

    m_ypSubs.emplace(uuid, ypSub);
    return sysrepo::ErrorCode::Ok;
}

std::shared_ptr<sysrepo::YangPushSubscription> SubscribedNotifications::getSubscription(const boost::uuids::uuid uuid) const
{
    if (auto it = m_ypSubs.find(uuid); it != m_ypSubs.end()) {
        return it->second;
    }

    return nullptr;
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
    m_yangPushSubscription->processEvent([this](const std::optional<libyang::DataNode>& notificationTree, const sysrepo::NotificationTimeStamp& time) {
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
