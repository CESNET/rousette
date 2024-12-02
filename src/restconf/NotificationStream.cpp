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
#include "restconf/utils/sysrepo.h"
#include "utils/yang.h"

using namespace std::string_literals;

namespace {

const auto streamListXPath = "/ietf-restconf-monitoring:restconf-state/streams/stream"s;
const auto rousetteURIScheme = "x-cesnet-rousette:"s;

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

        signal(rousette::restconf::as_restconf_notification(session.getContext(), dataFormat, *notificationTree, time));
    };

    if (!sub) {
        sub = session.onNotification(moduleName, std::move(notifCb), filter, startTime, stopTime);
    } else {
        sub->onNotification(moduleName, std::move(notifCb), filter, startTime, stopTime);
    }
}
}

namespace rousette::restconf {

NotificationStream::NotificationStream(
    const nghttp2::asio_http2::server::request& req,
    const nghttp2::asio_http2::server::response& res,
    rousette::http::EventStream::Termination& termination,
    std::shared_ptr<rousette::http::EventStream::EventSignal> signal,
    const std::chrono::seconds keepAlivePingInterval,
    sysrepo::Session session,
    libyang::DataFormat dataFormat,
    const std::optional<std::string>& filter,
    const std::optional<sysrepo::NotificationTimeStamp>& startTime,
    const std::optional<sysrepo::NotificationTimeStamp>& stopTime)
    : EventStream(req, res, termination, *signal, keepAlivePingInterval)
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
    const auto replayInfo = sysrepoReplayInfo(session);
    static const auto prefix = "/ietf-restconf-monitoring:restconf-state/streams/stream[name='NETCONF']"s;

    if (!parent) {
        parent = session.getContext().newPath(prefix + "/description", "Default NETCONF notification stream");
    } else {
        parent->newPath(prefix + "/description", "Default NETCONF notification stream");
    }
    parent->newPath(prefix + "/access[encoding='xml']/location", rousetteURIScheme + streamsPrefix + "NETCONF/XML");
    parent->newPath(prefix + "/access[encoding='json']/location", rousetteURIScheme + streamsPrefix + "NETCONF/JSON");

    if (replayInfo.enabled) {
        parent->newPath(prefix + "/replay-support", "true");

        if (replayInfo.earliestNotification) {
            parent->newPath(prefix + "/replay-log-creation-time", libyang::yangTimeFormat(*replayInfo.earliestNotification, libyang::TimezoneInterpretation::Local));
        }
    }
}

libyang::DataNode replaceStreamLocations(const std::optional<std::string>& schemeAndHost, libyang::DataNode& node)
{
    std::vector<libyang::DataNode> streamAccessNodes;
    for(const auto& e : node.findXPath(streamListXPath + "/access")) {
        streamAccessNodes.emplace_back(e);
    }

    for (const auto& n : streamAccessNodes) {
        auto locationNode = n.findPath("location");

        // if no scheme and host provided erase the location node entirely
        if (!schemeAndHost) {
            locationNode->unlink();
            continue;
        }

        std::string val = locationNode->asTerm().valueStr();
        n.newPath("location",
                  // remove the temporary rousetteURI prefix from the value and prepend the actual scheme and host
                  *schemeAndHost + val.erase(0, rousetteURIScheme.length()),
                  libyang::CreationOptions::Update);
    }

    return node;
}

/** @brief Create a new NotificationStream instance and activate it immediately.
 *
 * The stream is created with the given parameters and activated, which means it starts listening for
 * NETCONF notifications and sending them to the client.
 */
std::shared_ptr<NotificationStream> NotificationStream::create(
    const nghttp2::asio_http2::server::request& req,
    const nghttp2::asio_http2::server::response& res,
    rousette::http::EventStream::Termination& termination,
    const std::chrono::seconds keepAlivePingInterval,
    sysrepo::Session sess,
    libyang::DataFormat dataFormat,
    const std::optional<std::string>& filter,
    const std::optional<sysrepo::NotificationTimeStamp>& startTime,
    const std::optional<sysrepo::NotificationTimeStamp>& stopTime)
{
    auto signal = std::make_shared<rousette::http::EventStream::EventSignal>();
    auto stream = std::shared_ptr<NotificationStream>(new NotificationStream(req, res, termination, signal, keepAlivePingInterval, std::move(sess), dataFormat, filter, startTime, stopTime));
    stream->activate();
    return stream;
}
}
