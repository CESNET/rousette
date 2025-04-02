/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <libyang-cpp/Time.hpp>
#include <nghttp2/asio_http2_server.h>
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

sysrepo::YangPushChange yangPushChange(const std::string& str)
{
    if (str == "create") {
        return sysrepo::YangPushChange::Create;
    } else if (str == "delete") {
        return sysrepo::YangPushChange::Delete;
    } else if (str == "insert") {
        return sysrepo::YangPushChange::Insert;
    } else if (str == "move") {
        return sysrepo::YangPushChange::Move;
    } else if (str == "replace") {
        return sysrepo::YangPushChange::Replace;
    }

    throw std::invalid_argument("Unknown YangPushChange: " + str);
}

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

libyang::DataFormat subscribedNotificatonsEncoding(const libyang::DataNode& rpcInput, const libyang::DataFormat requestEncoding)
{
    if (auto encodingNode = rpcInput.findPath("encoding")) {
        const auto encodingStr = encodingNode->asTerm().valueStr();
        if (encodingStr == "ietf-subscribed-notifications:encode-json") {
            return libyang::DataFormat::JSON;
        } else if (encodingStr == "ietf-subscribed-notifications:encode-xml") {
            return libyang::DataFormat::XML;
        } else {
            throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "Unknown encoding in establish-subscription: '" + encodingStr + "'");
        }
    }

    return requestEncoding;
}

bool isFdClosed(const int fd)
{
    pollfd fds = {
        .fd = fd,
        .events = POLLIN | POLLHUP,
        .revents = 0};

    return poll(&fds, 1, 0) == 1 && fds.revents & POLLHUP;
}

sysrepo::DynamicSubscription subscribeNotificationStream(sysrepo::Session& session, const libyang::DataNode& rpcInput, libyang::DataNode& rpcOutput)
{
    if (!rpcInput.findPath("stream")) {
        throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "Stream is required");
    }

    if (rpcInput.findPath("stream-filter-name")) {
        /* TODO: This requires support for modifying subscriptions first; a change of entry in filters container must change all
         * subscriptions using this stream-filter, see for instance https://datatracker.ietf.org/doc/html/rfc8639.html#section-2.7.2 */
        throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "Stream filtering with predefined filters is not supported");
    }

    std::optional<sysrepo::NotificationTimeStamp> stopTime;
    if (auto stopTimeNode = rpcInput.findPath("stop-time")) {
        stopTime = libyang::fromYangTimeFormat<sysrepo::NotificationTimeStamp::clock>(stopTimeNode->asTerm().valueStr());
    }

    std::optional<std::variant<std::string, libyang::DataNodeAny>> filter;
    if (auto node = rpcInput.findPath("stream-xpath-filter")) {
        filter = node->asTerm().valueStr();
    } else if (auto node = rpcInput.findPath("stream-subtree-filter")) {
        filter = node->asAny();
    }

    std::optional<sysrepo::NotificationTimeStamp> replayStartTime;
    if (auto node = rpcInput.findPath("replay-start-time")) {
        replayStartTime = libyang::fromYangTimeFormat<sysrepo::NotificationTimeStamp::clock>(node->asTerm().valueStr());
    }

    auto sub = session.subscribeNotifications(
        filter,
        rpcInput.findPath("stream")->asTerm().valueStr(),
        stopTime,
        replayStartTime);

    if (auto replayStartTimeRevision = sub.replayStartTime(); replayStartTimeRevision && replayStartTime /* revision should be sent only if time was revised to be different than the requested start time */) {
        rpcOutput.newPath("replay-start-time-revision", libyang::yangTimeFormat(*replayStartTimeRevision, libyang::TimezoneInterpretation::Local), libyang::CreationOptions::Output);
    }

    return sub;
}

sysrepo::DynamicSubscription subscribeYangPush(sysrepo::Session& session, const libyang::DataNode& rpcInput, libyang::DataNode&)
{
    sysrepo::Datastore datastore = sysrepo::Datastore::Running;
    if (auto node = rpcInput.findPath("ietf-yang-push:datastore")) {
        datastore = rousette::restconf::datastoreFromString(node->asTerm().valueStr());
    } else {
        throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "Datastore is required for ietf-yang-push:on-change");
    }

    std::optional<std::variant<std::string, libyang::DataNodeAny>> filter;
    if (auto node = rpcInput.findPath("ietf-yang-push:datastore-xpath-filter")) {
        filter = node->asTerm().valueStr();
    } else if (auto node = rpcInput.findPath("ietf-yang-push:datastore-subtree-filter")) {
        filter = node->asAny();
    }

    std::optional<sysrepo::NotificationTimeStamp> stopTime;
    if (auto node = rpcInput.findPath("stop-time")) {
        stopTime = libyang::fromYangTimeFormat<sysrepo::NotificationTimeStamp::clock>(node->asTerm().valueStr());
    }

    std::optional<std::chrono::milliseconds> dampeningPeriod;
    if (auto node = rpcInput.findPath("ietf-yang-push:on-change/dampening-period")) {
        // dampening period is in centiseconds, but sysrepo expects milliseconds
        const std::chrono::duration<std::chrono::milliseconds::rep, std::centi> centiseconds(std::get<int64_t>(node->asTerm().value()));
        dampeningPeriod = std::chrono::duration_cast<std::chrono::milliseconds>(centiseconds);
    }

    sysrepo::SyncOnStart syncOnStart = sysrepo::SyncOnStart::No;
    if (auto node = rpcInput.findPath("ietf-yang-push:on-change/sync-on-start")) {
        syncOnStart = std::get<bool>(node->asTerm().value()) ? sysrepo::SyncOnStart::Yes : sysrepo::SyncOnStart::No;
    }

    std::set<sysrepo::YangPushChange> excludedChanges;
    for (const auto& node : rpcInput.findXPath("ietf-yang-push:on-change/excluded-change")) {
        excludedChanges.emplace(yangPushChange(node.asTerm().valueStr()));
    }

    rousette::restconf::ScopedDatastoreSwitch dsSwitch(session, datastore);
    return session.yangPushOnChange(filter, dampeningPeriod, syncOnStart, excludedChanges, stopTime);
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
    // Generate a new UUID associated with the subscription. The UUID will be used as a part of the URI so that the URI is not predictable (RFC 8650, section 5)
    auto uuid = m_uuidGenerator();

    auto dataFormat = subscribedNotificatonsEncoding(rpcInput, requestEncoding);

    // TODO: We are not yet following the state machine from RFC8639 2.4.1, we are always in state "receiver active"

    try {
        std::optional<sysrepo::DynamicSubscription> sub;

        if (rpcInput.findPath("stream")) {
            sub = subscribeNotificationStream(session, rpcInput, rpcOutput);
        } else if (rpcInput.findPath("ietf-yang-push:on-change")) {
            sub = subscribeYangPush(session, rpcInput, rpcOutput);
        } else if (rpcInput.findPath("ietf-yang-push:periodic")) {
            throw ErrorResponse(400, "application", "invalid-attribute", "Periodic subscriptions are not yet implemented");
        } else {
            throw ErrorResponse(400, "application", "invalid-attribute", "Could not deduce if YANG push on-change, YANG push periodic or subscribed notification");
        }

        rpcOutput.newPath("id", std::to_string(sub->subscriptionId()), libyang::CreationOptions::Output);
        rpcOutput.newPath("ietf-restconf-subscribed-notifications:uri", "/streams/subscribed/" + boost::uuids::to_string(uuid), libyang::CreationOptions::Output);

        std::unique_lock lock(m_mutex);
        m_subscriptions[uuid] = std::make_shared<SubscriptionData>(std::move(*sub), dataFormat);
    } catch (const sysrepo::ErrorWithCode& e) {
        throw ErrorResponse(400, "application", "invalid-attribute", e.what());
    }
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
    m_stream.async_wait(boost::asio::posix::stream_descriptor::wait_read, [this](const boost::system::error_code& err) {
        // unfortunately wait_read does not return operation_aborted when the file descriptor is closed and poll results in POLLHUP
        if (err == boost::asio::error::operation_aborted || isFdClosed(m_subscriptionData->subscription.fd())) {
            return;
        }

        // process the incoming notification
        m_subscriptionData->subscription.processEvent([&](const std::optional<libyang::DataNode>& notificationTree, const sysrepo::NotificationTimeStamp& time) {
            (*m_signal)(rousette::restconf::as_restconf_notification(m_session.getContext(), m_subscriptionData->dataFormat, *notificationTree, time));
        });

        // and wait for the next one
        awaitNextNotification();
    });
}

void DynamicSubscriptionHttpStream::activate()
{
    awaitNextNotification();
    EventStream::activate();
}
}
