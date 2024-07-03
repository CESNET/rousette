/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <libyang-cpp/Time.hpp>
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Subscription.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include "http/EventStream.h"
#include "restconf/Exceptions.h"
#include "restconf/NotificationStream.h"
#include "utils/yang.h"

namespace {
/** @brief Wraps a notification data tree with RESTCONF notification envelope. */
std::string as_restconf_notification(const libyang::Context& ctx, libyang::DataFormat dataFormat, libyang::DataNode notification, const sysrepo::NotificationTimeStamp& time)
{
    static const auto jsonNamespace = "ietf-restconf";
    static const auto xmlNamespace = "urn:ietf:params:xml:ns:netconf:notification:1.0";

    std::optional<libyang::DataNode> envelope;
    std::optional<libyang::DataNode> eventTime;
    std::string timeStr = libyang::yangTimeFormat(time, libyang::TimezoneInterpretation::Local);

    /* The namespaces for XML and JSON evenlopes are different. See https://datatracker.ietf.org/doc/html/rfc8040#section-6.4 */
    if (dataFormat == libyang::DataFormat::JSON) {
        envelope = ctx.newOpaqueJSON(jsonNamespace, "notification", std::nullopt);
        eventTime = ctx.newOpaqueJSON(jsonNamespace, "eventTime", libyang::JSON{timeStr});
    } else {
        envelope = ctx.newOpaqueXML(xmlNamespace, "notification", std::nullopt);
        eventTime = ctx.newOpaqueXML(xmlNamespace, "eventTime", libyang::XML{timeStr});
    }

    envelope->insertChild(*eventTime);
    envelope->insertChild(notification);

    auto res = *envelope->printStr(dataFormat, libyang::PrintFlags::WithSiblings);

    notification.unlink();

    return res;
}

sysrepo::Subscription createNotificationSub(
    sysrepo::Session& session,
    const std::string& moduleName,
    rousette::http::EventStream::Signal& signal,
    libyang::DataFormat dataFormat,
    const std::optional<std::string>& filter,
    const std::optional<sysrepo::NotificationTimeStamp>& startTime,
    const std::optional<sysrepo::NotificationTimeStamp>& stopTime)
{
    return session.onNotification(
        moduleName,
        [&signal, dataFormat](auto session, auto, sysrepo::NotificationType type, const std::optional<libyang::DataNode>& notificationTree, const sysrepo::NotificationTimeStamp& time) {
            if (type != sysrepo::NotificationType::Realtime) {
                return;
            }

            signal(as_restconf_notification(session.getContext(), dataFormat, *notificationTree, time));
        },
        filter,
        startTime,
        stopTime);
}
}

namespace rousette::restconf {

NotificationStream::NotificationStream(
    const nghttp2::asio_http2::server::request& req,
    const nghttp2::asio_http2::server::response& res,
    sysrepo::Session session,
    libyang::DataFormat dataFormat,
    const std::optional<std::string>& filter,
    const std::optional<sysrepo::NotificationTimeStamp>& startTime,
    const std::optional<sysrepo::NotificationTimeStamp>& stopTime)
    : EventStream(req, res)
{
    for (const auto& mod : session.getContext().modules()) {
        if (mod.implemented()) {
            try {
                m_notifSubs.emplace_back(createNotificationSub(session, mod.name(), m_notificationSignal, dataFormat, filter, startTime, stopTime));
            } catch (sysrepo::ErrorWithCode& e) {
                if (e.code() == sysrepo::ErrorCode::InvalidArgument) {
                    throw ErrorResponse(400, "application", "invalid-argument", e.what());
                }
            }
        }
    }
}

void NotificationStream::activate()
{
    EventStream::activate(m_notificationSignal);
}

/** @brief */
void notificationStreamList(sysrepo::Session& session, std::optional<libyang::DataNode>& parent)
{
    static const auto prefix = "/ietf-restconf-monitoring:restconf-state/streams/stream[name='NETCONF']"s;

    decltype(sysrepo::ModuleReplaySupport::earliestNotification) globalEarliestNotification;

    for (const auto& mod : session.getContext().modules()) {
        if (mod.implemented()) {
            try {
                auto replay = session.getConnection().getModuleReplaySupport(mod.name());
                if (replay.earliestNotification) {
                    globalEarliestNotification = std::min(*replay.earliestNotification, *globalEarliestNotification);
                }
            } catch (sysrepo::ErrorWithCode& e) {
            }
        }
    }

    if (!parent) {
        parent = session.getContext().newPath(prefix + "/description", "Default NETCONF notification stream");
    } else {
        parent->newPath(prefix + "/description", "Default NETCONF notification stream");
    }
    parent->newPath(prefix + "/access[encoding='xml']/location", "http://example.com/streams/NETCONF/XML");
    parent->newPath(prefix + "/access[encoding='json']/location", "http://example.com/streams/NETCONF/json");

    if (globalEarliestNotification) {
        parent->newPath(prefix + "/replay-support", "true");
        parent->newPath(prefix + "/replay-log-creation-time", libyang::yangTimeFormat(*globalEarliestNotification, libyang::TimezoneInterpretation::Local));
    }
}
}
