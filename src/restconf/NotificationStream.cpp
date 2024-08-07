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

using namespace std::string_literals;

namespace {

const auto streamListXPath = "/ietf-restconf-monitoring:restconf-state/streams/stream"s;

/** @brief Wraps a notification data tree with RESTCONF notification envelope. */
std::string as_restconf_notification(const libyang::Context& ctx, libyang::DataFormat dataFormat, libyang::DataNode notification, const sysrepo::NotificationTimeStamp& time)
{
    static const auto jsonNamespace = "ietf-restconf";
    static const auto xmlNamespace = "urn:ietf:params:xml:ns:netconf:notification:1.0";

    std::optional<libyang::DataNode> envelope;
    std::optional<libyang::DataNode> eventTime;
    std::string timeStr = libyang::yangTimeFormat(time, libyang::TimezoneInterpretation::Local);

    /* The namespaces for XML and JSON envelopes are different. See https://datatracker.ietf.org/doc/html/rfc8040#section-6.4 */
    if (dataFormat == libyang::DataFormat::JSON) {
        envelope = ctx.newOpaqueJSON(jsonNamespace, "notification", std::nullopt);
        eventTime = ctx.newOpaqueJSON(jsonNamespace, "eventTime", libyang::JSON{timeStr});
    } else {
        envelope = ctx.newOpaqueXML(xmlNamespace, "notification", std::nullopt);
        eventTime = ctx.newOpaqueXML(xmlNamespace, "eventTime", libyang::XML{timeStr});
    }

    // the notification data node holds only the notification data tree but for nested notification we should print the whole YANG data tree
    while (notification.parent()) {
        notification = *notification.parent();
    }

    envelope->insertChild(*eventTime);
    envelope->insertChild(notification);

    auto res = *envelope->printStr(dataFormat, libyang::PrintFlags::WithSiblings);

    // notification node comes from sysrepo and sysrepo will free this; if not unlinked then envelope destructor would try to free this as well
    notification.unlink();

    return res;
}

void subscribe(
    std::optional<sysrepo::Subscription>& sub,
    sysrepo::Session& session,
    const std::string& moduleName,
    rousette::http::EventStream::EventSignal& signal,
    libyang::DataFormat dataFormat,
    const std::optional<std::string>& filter,
    const std::optional<sysrepo::NotificationTimeStamp>& startTime,
    const std::optional<sysrepo::NotificationTimeStamp>& stopTime)
{
    auto notifCb = [&signal, dataFormat](auto session, auto, sysrepo::NotificationType type, const std::optional<libyang::DataNode>& notificationTree, const sysrepo::NotificationTimeStamp& time) {
        if (type != sysrepo::NotificationType::Realtime && type != sysrepo::NotificationType::Replay) {
            return;
        }

        signal(as_restconf_notification(session.getContext(), dataFormat, *notificationTree, time));
    };

    if (!sub) {
        sub = session.onNotification(moduleName, std::move(notifCb), filter, startTime, stopTime);
    } else {
        sub->onNotification(moduleName, std::move(notifCb), filter, startTime, stopTime);
    }
}

/** @brief Early filter for modules that can be subscribed to. This returns true for module without any notification node, but sysrepo will throw when subscribing */
bool canBeSubscribed(const libyang::Module& mod)
{
    return mod.implemented() && mod.name() != "sysrepo";
}
}

namespace rousette::restconf {

NotificationStream::NotificationStream(
    const nghttp2::asio_http2::server::request& req,
    const nghttp2::asio_http2::server::response& res,
    rousette::http::EventStream::Termination& termination,
    std::shared_ptr<rousette::http::EventStream::EventSignal> signal,
    sysrepo::Session session,
    libyang::DataFormat dataFormat,
    const std::optional<std::string>& filter,
    const std::optional<sysrepo::NotificationTimeStamp>& startTime,
    const std::optional<sysrepo::NotificationTimeStamp>& stopTime)
    : EventStream(req, res, termination, *signal)
    , m_notificationSignal(signal)
    , m_session(std::move(session))
    , m_dataFormat(dataFormat)
    , m_filter(filter)
    , m_startTime(startTime)
    , m_stopTime(stopTime)
{
    auto now = std::chrono::system_clock::now();

    if (startTime && stopTime && startTime >= stopTime) {
        throw ErrorResponse(400, "application", "invalid-argument", "stop-time must be greater than start-time");
    } else if (startTime && startTime > now) {
        throw ErrorResponse(400, "application", "invalid-argument", "start-time is in the future");
    } else if (!startTime && stopTime) {
        throw ErrorResponse(400, "application", "invalid-argument", "stop-time must be used with start-time");
    }
}

void NotificationStream::activate()
{
    for (const auto& mod : m_session.getContext().modules()) {
        if (!canBeSubscribed(mod)) {
            continue;
        }

        try {
            subscribe(m_notifSubs, m_session, mod.name(), *m_notificationSignal, m_dataFormat, m_filter, m_startTime, m_stopTime);
        } catch (sysrepo::ErrorWithCode& e) {
            if (e.code() == sysrepo::ErrorCode::InvalidArgument) {
                throw ErrorResponse(400, "application", "invalid-argument", e.what());
            }

            /* We are iterating through all modules in order to subscribe to every possible module.
             * If the module does not define any notifications or the module does not exist then sysrepo throws with ErrorCode::NotFound
             * (see sysrepo's sr_notif_subscribe and sr_subscr_notif_xpath_check).
             *
             * We can either scan the YANG schema and search for notifications nodes (like netopeer2) or ignore this particular exception.
             */
            if (e.code() != sysrepo::ErrorCode::NotFound) {
                throw;
            }
        }
    }

    EventStream::activate();
}

/** @brief Creates and fills ietf-restconf-monitoring:restconf-state/stream. To be called in oper callback. */
void notificationStreamList(sysrepo::Session& session, std::optional<libyang::DataNode>& parent, const std::string& streamsPrefix)
{
    static const auto prefix = "/ietf-restconf-monitoring:restconf-state/streams/stream[name='NETCONF']"s;

    decltype(sysrepo::ModuleReplaySupport::earliestNotification) globalEarliestNotification;
    bool replayEnabled = false;

    for (const auto& mod : session.getContext().modules()) {
        if (!canBeSubscribed(mod)) {
            continue;
        }

        auto replay = session.getConnection().getModuleReplaySupport(mod.name());
        replayEnabled |= replay.enabled;

        if (replay.earliestNotification) {
            if (!globalEarliestNotification) {
                globalEarliestNotification = replay.earliestNotification;
            } else {
                globalEarliestNotification = std::min(*replay.earliestNotification, *globalEarliestNotification);
            }
        }
    }

    if (!parent) {
        parent = session.getContext().newPath(prefix + "/description", "Default NETCONF notification stream");
    } else {
        parent->newPath(prefix + "/description", "Default NETCONF notification stream");
    }
    parent->newPath(prefix + "/access[encoding='xml']/location", streamsPrefix + "NETCONF/XML");
    parent->newPath(prefix + "/access[encoding='json']/location", streamsPrefix + "NETCONF/JSON");

    if (replayEnabled) {
        parent->newPath(prefix + "/replay-support", "true");

        if (globalEarliestNotification) {
            parent->newPath(prefix + "/replay-log-creation-time", libyang::yangTimeFormat(*globalEarliestNotification, libyang::TimezoneInterpretation::Local));
        }
    }
}

libyang::DataNode replaceStreamLocations(const std::optional<std::string>& schemeAndHost, libyang::DataNode& node)
{
    // if we were unable to parse scheme and hosts, end without doing any changes
    // this will, however, result in locations like "/streams/NETCONF/XML"
    if (!schemeAndHost) {
        return node;
    }

    auto accessNodes = node.findXPath(streamListXPath + "/access");

    for (const auto& n : accessNodes) {
        std::string val = n.findPath("location")->asTerm().valueStr();
        n.newPath("location", *schemeAndHost + val, libyang::CreationOptions::Update);
    }

    return node;
}
}
