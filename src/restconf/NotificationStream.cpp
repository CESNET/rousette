/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <boost/uuid/uuid_io.hpp>
#include <libyang-cpp/Time.hpp>
#include <nghttp2/asio_http2_server.h>
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

void subscribe(
    std::optional<sysrepo::Subscription>& sub,
    sysrepo::Session& session,
    const std::string& moduleName,
    rousette::http::EventStream::Signal& signal,
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

/** @brief Early filter for modules that can be subscribed to. This returns true for module without any notification node, but sysrepo will throw when subscribing */
bool canBeSubscribed(const libyang::Module& mod)
{
    return mod.implemented() && mod.name() != "sysrepo";
}

struct SysrepoReplayInfo {
    bool enabled;
    std::optional<sysrepo::NotificationTimeStamp> earliestNotification;
};

SysrepoReplayInfo sysrepoReplayInfo(sysrepo::Session& session)
{
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

    return {replayEnabled, globalEarliestNotification};
}
}

namespace rousette::restconf {

NotificationStream::NotificationStream(
    const nghttp2::asio_http2::server::request& req,
    const nghttp2::asio_http2::server::response& res,
    std::shared_ptr<rousette::http::EventStream::Signal> signal,
    sysrepo::Session session,
    libyang::DataFormat dataFormat,
    const std::optional<std::string>& filter,
    const std::optional<sysrepo::NotificationTimeStamp>& startTime,
    const std::optional<sysrepo::NotificationTimeStamp>& stopTime)
    : EventStream(req, res, *signal)
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
    parent->newPath(prefix + "/access[encoding='xml']/location", streamsPrefix + "NETCONF/XML");
    parent->newPath(prefix + "/access[encoding='json']/location", streamsPrefix + "NETCONF/JSON");

    if (replayInfo.enabled) {
        parent->newPath(prefix + "/replay-support", "true");

        if (replayInfo.earliestNotification) {
            parent->newPath(prefix + "/replay-log-creation-time", libyang::yangTimeFormat(*replayInfo.earliestNotification, libyang::TimezoneInterpretation::Local));
        }
    }
}

/** @brief Creates and fills ietf-subscribed-notifications:streams. To be called in oper callback. */
void notificationStreamListSubscribed(sysrepo::Session& session, std::optional<libyang::DataNode>& parent)
{
    static const auto prefix = "/ietf-subscribed-notifications:streams"s;
    const auto replayInfo = sysrepoReplayInfo(session);

    if (!parent) {
        parent = session.getContext().newPath(prefix + "/stream[name='NETCONF']/description", "Default NETCONF notification stream");
    } else {
        parent->newPath(prefix + "/stream[name='NETCONF']/description", "Default NETCONF notification stream");
    }

    if (replayInfo.enabled) {
        session.setItem(prefix + "/stream[name='NETCONF']/replay-support", std::nullopt);
        if (replayInfo.earliestNotification) {
            session.setItem(prefix + "/stream[name='NETCONF']/replay-log-creation-time", libyang::yangTimeFormat(*replayInfo.earliestNotification, libyang::TimezoneInterpretation::Local));
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

DynamicSubscriptions::DynamicSubscriptions(sysrepo::Connection& conn)
    : m_uuidGenerator(boost::uuids::random_generator())
    , m_session(conn.sessionStart(sysrepo::Datastore::Operational))
{
    m_notificationStreamListSub = m_session.onOperGet(
        "ietf-subscribed-notifications",
        [](auto session, auto, auto, auto, auto, auto, auto& parent) {
            notificationStreamListSubscribed(session, parent);
            return sysrepo::ErrorCode::Ok;
        },
        "/ietf-subscribed-notifications:streams");
}

void DynamicSubscriptions::establishSubscription(sysrepo::Session& session, const libyang::DataFormat requestEncoding, const libyang::DataNode& rpcInput, libyang::DataNode& rpcOutput)
{
    std::string stream;

    if (auto streamNode = rpcInput.findPath("stream")) {
        stream = streamNode->asTerm().valueStr();
    } else {
        throw ErrorResponse(400, "application", "missing-attribute", "Missing stream leaf (stream-filters are not yet supported)");
    }

    std::optional<sysrepo::NotificationTimeStamp> stopTime;
    if (auto stopTimeNode = rpcInput.findPath("stop-time")) {
        stopTime = libyang::fromYangTimeFormat<sysrepo::NotificationTimeStamp::clock>(stopTimeNode->asTerm().valueStr());
    }

    std::optional<libyang::DataFormat> encoding;
    if (auto encodingNode = rpcInput.findPath("encoding")) {
        const auto encodingStr = encodingNode->asTerm().valueStr();
        if (encodingStr == "ietf-subscribed-notifications:encode-json") {
            encoding = libyang::DataFormat::JSON;
        } else if (encodingStr == "ietf-subscribed-notifications:encode-xml") {
            encoding = libyang::DataFormat::XML;
        } else {
            throw ErrorResponse(400, "application", "invalid-attribute", "Unknown encoding in establish-subscription: '" + encodingStr + "'");
        }
    } else {
        encoding = requestEncoding;
    }

    // TODO: We are not yet following the state machine from RFC8639 2.4.1, we are always in state "receiver active"

    // Generate a new UUID associated with the subscription. The UUID will be used as a part of the URI so that the URI is not predictable (RFC 8650, section 5)
    auto uuid = m_uuidGenerator();
    auto sub = session.subscribeNotifications(
        std::nullopt /* TODO filter */,
        stream,
        stopTime,
        std::nullopt,
        std::nullopt /* TODO replayStart */);

    rpcOutput.newPath("id", std::to_string(sub.subscriptionId()), libyang::CreationOptions::Output);
    rpcOutput.newPath("ietf-restconf-subscribed-notifications:uri", "/streams/subscribed/" + boost::uuids::to_string(uuid), libyang::CreationOptions::Output);

    std::unique_lock lock(m_mutex);
    m_subscriptions[uuid] = std::make_shared<SubscriptionData>(std::move(sub), encoding);
}

std::shared_ptr<DynamicSubscriptions::SubscriptionData> DynamicSubscriptions::getSubscription(const boost::uuids::uuid uuid)
{
    std::unique_lock lock(m_mutex);
    if (auto it = m_subscriptions.find(uuid); it != m_subscriptions.end()) {
        return it->second;
    }

    return nullptr;
}

DynamicSubscriptionHttpStream::DynamicSubscriptionHttpStream(
    const nghttp2::asio_http2::server::request& req,
    const nghttp2::asio_http2::server::response& res,
    std::shared_ptr<rousette::http::EventStream::Signal> signal,
    sysrepo::Session session,
    const std::shared_ptr<DynamicSubscriptions::SubscriptionData>& subscription)
    : EventStream(req, res, *signal)
    , m_session(std::move(session))
    , m_subscriptionData(subscription)
    , m_signal(signal)
    , m_stream(res.io_service(), m_subscriptionData->subscription.fd())
{
}

/** @brief Waits for the next notification and calls cb() */
void DynamicSubscriptionHttpStream::awaitNextNotification()
{
    m_stream.async_read_some(boost::asio::null_buffers(), [this](const boost::system::error_code& err, auto) {
        spdlog::trace("SubscribedNotificationStream::activate async_read_some: {}", err.message());
        if (err == boost::asio::error::operation_aborted) {
            return;
        }
        cb();
    });
}

void DynamicSubscriptionHttpStream::activate()
{
    awaitNextNotification();
    EventStream::activate();
}

void DynamicSubscriptionHttpStream::cb()
{
    // process the incoming notification
    m_subscriptionData->subscription.processEvent([this](const std::optional<libyang::DataNode>& notificationTree, const sysrepo::NotificationTimeStamp& time) {
        // FIXME: if the data format is nullopt, then we should use the content-type which was used for the establish-subscription rpc
        (*m_signal)(rousette::restconf::as_restconf_notification(m_session.getContext(), m_subscriptionData->dataFormat.value_or(libyang::DataFormat::JSON), *notificationTree, time));
    });

    // and wait for the next one
    awaitNextNotification();
}
}
