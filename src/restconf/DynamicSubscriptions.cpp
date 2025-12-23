/*
 * Copyright (C) 2025 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
*/
#include <boost/uuid/uuid_io.hpp>
#include <fmt/ostream.h>
#include <libyang-cpp/Time.hpp>
#include <nghttp2/asio_http2_server.h>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/utils/exception.hpp>
#include "restconf/DynamicSubscriptions.h"
#include "restconf/Exceptions.h"
#include "restconf/utils/io.h"
#include "restconf/utils/sysrepo.h"
#include "restconf/utils/yang.h"

namespace {


constexpr auto streamFilter = "/ietf-subscribed-notifications:filters/stream-filter";
constexpr auto streamFilterKey = "name";
constexpr auto selectionFilter = "/ietf-subscribed-notifications:filters/ietf-yang-push:selection-filter";
constexpr auto selectionFilterKey = "filter-id";

/** @brief Parses the YANG date-and-time attribute from the RPC input, if present
 *
 * @param rpcInput The RPC input node.
 * @param path The path to the YANG leaf.
 */
std::optional<sysrepo::NotificationTimeStamp> optionalTime(const libyang::DataNode& rpcInput, const std::string& path)
{
    if (auto stopTimeNode = rpcInput.findPath(path)) {
        return libyang::fromYangTimeFormat<sysrepo::NotificationTimeStamp::clock>(stopTimeNode->asTerm().valueStr());
    }

    return std::nullopt;
}

libyang::DataFormat getEncoding(const libyang::DataNode& rpcInput, const libyang::DataFormat requestEncoding)
{
    /* FIXME: So far we allow only encode-json or encode-xml encoding values and not their derived values.
     * We do not know what those derived values might mean and how do they change the meaning of the encoding leaf.
     */
    if (auto encodingNode = rpcInput.findPath("encoding")) {
        const auto encodingStr = encodingNode->asTerm().valueStr();
        if (encodingStr == "ietf-subscribed-notifications:encode-json") {
            return libyang::DataFormat::JSON;
        } else if (encodingStr == "ietf-subscribed-notifications:encode-xml") {
            return libyang::DataFormat::XML;
        } else {
            throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "Unsupported encoding in establish-subscription: '" + encodingStr + "'. Currently we support only 'encode-xml' and 'encode-json' identities.");
        }
    }

    return requestEncoding;
}

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

/** @brief Creates a filter for the subscription.
 *
 * Filters for YANG Push and for subscribed notifications are specified in the same way,
 * only in a different YANG node. The same holds for filter resolution.
 * */
std::optional<std::variant<std::string, libyang::DataNodeAny>> createFilter(
    sysrepo::Session& session,
    const libyang::DataNode& rpcInput,
    const std::string& filterListPath,
    const std::string& filterListKey,
    const std::string& xpathFilterPath,
    const std::string& subtreeFilterPath,
    const std::string& filterNamePath)
{
    if (auto node = rpcInput.findPath(xpathFilterPath)) {
        return node->asTerm().valueStr();
    }

    if (auto node = rpcInput.findPath(subtreeFilterPath)) {
        return node->asAny();
    }

    // resolve filter from ietf-subscribed-notifications:filters
    if (auto node = rpcInput.findPath(filterNamePath)) {
        rousette::restconf::ScopedDatastoreSwitch dsSwitch(session, sysrepo::Datastore::Operational);

        const auto xpath = fmt::format("{}[{}='{}']", filterListPath, filterListKey, node->asTerm().valueStr());
        auto data = session.getData(xpath);
        if (!data) {
            throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "Name '" + node->asTerm().valueStr() + "' does not refer to an existing filter/selection.");
        }

        auto filterNode = data->findPath(xpath);
        if (!filterNode) {
            throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "Name '" + node->asTerm().valueStr() + "' does not refer to an existing filter/selection.");
        }

        if (auto node = filterNode->findPath(xpathFilterPath)) {
            return node->asTerm().valueStr();
        }

        if (auto node = filterNode->findPath(subtreeFilterPath)) {
            return node->asAny();
        }
    }

    return std::nullopt;
}

/** @brief Reads interval from the YANG node and converts it to std::milliseconds.
 *
 *  @tparam SourceRatio Ratio of the interval from the YANG node (e.g. centiseconds, seconds, ...)
 * */
template <class SourceRatio>
std::optional<std::chrono::milliseconds> createInterval(const libyang::DataNode& rpcInput, const std::string& path)
{
    if (auto node = rpcInput.findPath(path)) {
        auto value = std::get<uint32_t>(node->asTerm().value());
        std::chrono::duration<std::chrono::milliseconds::rep, SourceRatio> duration(value);
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    }

    return std::nullopt;
}

sysrepo::DynamicSubscription makeStreamSubscription(sysrepo::Session& session, const libyang::DataNode& rpcInput, libyang::DataNode& rpcOutput)
{
    auto streamNode = rpcInput.findPath("stream");

    if (!streamNode) {
        throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "Stream is required");
    }

    auto stopTime = optionalTime(rpcInput, "stop-time");

    std::optional<sysrepo::NotificationTimeStamp> replayStartTime;
    if (auto node = rpcInput.findPath("replay-start-time")) {
        replayStartTime = libyang::fromYangTimeFormat<sysrepo::NotificationTimeStamp::clock>(node->asTerm().valueStr());
    }

    auto sub = session.subscribeNotifications(
        createFilter(session, rpcInput, streamFilter, streamFilterKey, "stream-xpath-filter", "stream-subtree-filter", "stream-filter-name"),
        streamNode->asTerm().valueStr(),
        stopTime,
        replayStartTime);

    /* Node replay-start-time-revision should be set only if time was revised to be different than the requested start time,
     * i.e. when the "replay-start-time" contains a value that is earlier than what a publisher's retained history.
     * Then the actual publisher's revised start time MUST be set in the returned "replay-start-time-revision" object.
     * (RFC 8639, 2.4.2.1)
     * */
    if (auto replayStartTimeRevision = sub.replayStartTime(); replayStartTimeRevision && replayStartTime) {
        rpcOutput.newPath("replay-start-time-revision", libyang::yangTimeFormat(*replayStartTimeRevision, libyang::TimezoneInterpretation::Local), libyang::CreationOptions::Output);
    }

    return sub;
}

sysrepo::DynamicSubscription makeYangPushOnChangeSubscription(sysrepo::Session& session, const libyang::DataNode& rpcInput, libyang::DataNode&)
{
    sysrepo::Datastore datastore = sysrepo::Datastore::Running;
    if (auto node = rpcInput.findPath("ietf-yang-push:datastore")) {
        datastore = rousette::restconf::datastoreFromString(node->asTerm().valueStr());
    } else {
        throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "Datastore is required for ietf-yang-push:on-change");
    }

    std::optional<sysrepo::NotificationTimeStamp> stopTime;
    if (auto node = rpcInput.findPath("stop-time")) {
        stopTime = libyang::fromYangTimeFormat<sysrepo::NotificationTimeStamp::clock>(node->asTerm().valueStr());
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
    return session.yangPushOnChange(
        createFilter(session, rpcInput, selectionFilter, selectionFilterKey, "ietf-yang-push:datastore-xpath-filter", "ietf-yang-push:datastore-subtree-filter", "ietf-yang-push:selection-filter-ref"),
        createInterval<std::centi>(rpcInput, "ietf-yang-push:on-change/dampening-period"),
        syncOnStart,
        excludedChanges,
        stopTime);
}

sysrepo::DynamicSubscription makeYangPushPeriodicSubscription(sysrepo::Session& session, const libyang::DataNode& rpcInput, libyang::DataNode&)
{
    sysrepo::Datastore datastore = sysrepo::Datastore::Running;
    if (auto node = rpcInput.findPath("ietf-yang-push:datastore")) {
        datastore = rousette::restconf::datastoreFromString(node->asTerm().valueStr());
    } else {
        throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "Datastore is required for ietf-yang-push:periodic");
    }

    auto period = createInterval<std::centi>(rpcInput, "ietf-yang-push:periodic/period");
    if (!period) {
        throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "period is required for ietf-yang-push:periodic");
    }

    std::optional<sysrepo::NotificationTimeStamp> stopTime;
    if (auto node = rpcInput.findPath("stop-time")) {
        stopTime = libyang::fromYangTimeFormat<sysrepo::NotificationTimeStamp::clock>(node->asTerm().valueStr());
    }

    std::optional<sysrepo::NotificationTimeStamp> anchorTime;
    if (auto node = rpcInput.findPath("ietf-yang-push:periodic/anchor-time")) {
        anchorTime = libyang::fromYangTimeFormat<sysrepo::NotificationTimeStamp::clock>(node->asTerm().valueStr());
    }

    rousette::restconf::ScopedDatastoreSwitch dsSwitch(session, datastore);
    return session.yangPushPeriodic(
        createFilter(session, rpcInput, selectionFilter, selectionFilterKey, "ietf-yang-push:datastore-xpath-filter", "ietf-yang-push:datastore-subtree-filter", "ietf-yang-push:selection-filter-ref"),
        *period,
        anchorTime,
        stopTime);
}

/** @brief Creates and fills ietf-subscribed-notifications:streams. To be called in oper callback. */
void notificationStreamListSubscribed(sysrepo::Session& session, std::optional<libyang::DataNode>& parent)
{
    static const std::string prefix = "/ietf-subscribed-notifications:streams";
    const auto replayInfo = rousette::restconf::sysrepoReplayInfo(session);
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
}

namespace rousette::restconf {

DynamicSubscriptions::DynamicSubscriptions(sysrepo::Session& session, const std::string& streamRootUri, const nghttp2::asio_http2::server::http2& server, const std::chrono::seconds inactivityTimeout)
    : m_restconfStreamUri(streamRootUri)
    , m_server(server)
    , m_uuidGenerator(boost::uuids::random_generator())
    , m_inactivityTimeout(inactivityTimeout)
{
    m_notificationStreamListSub = session.onOperGet(
        "ietf-subscribed-notifications",
        [](auto session, auto, auto, auto, auto, auto, auto& parent) {
            notificationStreamListSubscribed(session, parent);
            return sysrepo::ErrorCode::Ok;
        },
        "/ietf-subscribed-notifications:streams");
}

DynamicSubscriptions::~DynamicSubscriptions() = default;

void DynamicSubscriptions::stop()
{
    std::lock_guard lock(m_mutex);
    for (const auto& [uuid, subscriptionData] : m_subscriptions) {
        subscriptionData->stop();
    }
}

void DynamicSubscriptions::establishSubscription(sysrepo::Session& session, const libyang::DataFormat requestEncoding, const libyang::DataNode& rpcInput, libyang::DataNode& rpcOutput)
{
    // Generate a new UUID associated with the subscription. The UUID will be used as a part of the URI so that the URI is not predictable (RFC 8650, section 5)
    auto uuid = makeUUID();

    auto dataFormat = getEncoding(rpcInput, requestEncoding);

    try {
        std::optional<sysrepo::DynamicSubscription> sub;

        if (rpcInput.findPath("stream")) {
            sub = makeStreamSubscription(session, rpcInput, rpcOutput);
        } else if (rpcInput.findPath("ietf-yang-push:on-change")) {
            sub = makeYangPushOnChangeSubscription(session, rpcInput, rpcOutput);
        } else if (rpcInput.findPath("ietf-yang-push:periodic")) {
            sub = makeYangPushPeriodicSubscription(session, rpcInput, rpcOutput);
        } else {
            throw ErrorResponse(400, "application", "invalid-attribute", "Could not deduce if YANG push on-change, YANG push periodic or subscribed notification");
        }

        rpcOutput.newPath("id", std::to_string(sub->subscriptionId()), libyang::CreationOptions::Output);
        rpcOutput.newPath("ietf-restconf-subscribed-notifications:uri", m_restconfStreamUri + "subscribed/" + boost::uuids::to_string(uuid), libyang::CreationOptions::Output);

        std::lock_guard lock(m_mutex);
        m_subscriptions[uuid] = std::make_shared<SubscriptionData>(
            std::move(*sub),
            dataFormat,
            uuid,
            *session.getNacmUser(),
            *m_server.io_services().at(0),
            m_inactivityTimeout,
            [this, subId = sub->subscriptionId()]() { terminateSubscription(subId); });
    } catch (const sysrepo::ErrorWithCode& e) {
        throw ErrorResponse(400, "application", "invalid-attribute", e.what());
    }
}

void DynamicSubscriptions::deleteSubscription(sysrepo::Session& session, const libyang::DataFormat, const libyang::DataNode& rpcInput, libyang::DataNode&)
{
    const auto isKill = rpcInput.findPath("/ietf-subscribed-notifications:kill-subscription") != std::nullopt;
    const auto subId = std::get<uint32_t>(rpcInput.findPath("id")->asTerm().value());

    // The RPC is already NACM-checked. Now, retrieve the subscription, if the current user has permission for it
    auto subscriptionData = getSubscriptionForUser(subId, session.getNacmUser());
    if (!subscriptionData) {
        throw ErrorResponse(404, "application", "invalid-value", "Subscription not found.", rpcInput.path());
    }

    /* I *think* the RFC 8639 says that root can use delete-subscription only for subscriptions created by root.
     * This checks if the current user is root and the subscription was created by a different user. If so, reject the request.
     */
    if (!isKill && session.getNacmUser() == session.getNacmRecoveryUser() && subscriptionData->user != session.getNacmRecoveryUser()) {
        //FIXME: pass additional error info (rc:yang-data delete-subscription-error-info from RFC 8639)
        throw ErrorResponse(400, "application", "invalid-attribute", "Trying to delete subscription not created by root. Use kill-subscription instead.", rpcInput.path());
    }

    spdlog::debug("Terminating subscription id {}", subId);
    subscriptionData->subscription.terminate("ietf-subscribed-notifications:no-such-subscription");

    std::unique_lock lock(m_mutex);
    m_subscriptions.erase(subscriptionData->uuid);
}

void DynamicSubscriptions::terminateSubscription(const uint32_t subId)
{
    std::lock_guard lock(m_mutex);

    auto it = std::find_if(m_subscriptions.begin(), m_subscriptions.end(), [subId](const auto& entry) {
        return entry.second->subscription.subscriptionId() == subId;
    });

    if (it == m_subscriptions.end()) {
        spdlog::warn("Requested termination of subscription with id {}, but subscription not found", subId);
        return;
    }

    const auto& [uuid, subscriptionData] = *it;
    spdlog::debug("{}: termination requested", fmt::streamed(*subscriptionData));
    subscriptionData->terminate("ietf-subscribed-notifications:no-such-subscription");
    m_subscriptions.erase(uuid);
}

/** @brief Returns the subscription data for the given UUID and user.
 *
 * @param uuid The UUID of the subscription.
 * @return A shared pointer to the SubscriptionData object if found and user is the one who established the subscription (or NACM recovery user), otherwise nullptr.
 */
std::shared_ptr<DynamicSubscriptions::SubscriptionData> DynamicSubscriptions::getSubscriptionForUser(const boost::uuids::uuid& uuid, const std::optional<std::string>& user)
{
    std::lock_guard lock(m_mutex);
    if (auto it = m_subscriptions.find(uuid); it != m_subscriptions.end() && (it->second->user == user || user == sysrepo::Session::getNacmRecoveryUser())) {
        return it->second;
    }

    return nullptr;
}

/** @brief Returns the subscription data for the given subscription id and user.
 *
 * @param id The ID of the subscription.
 * @return A shared pointer to the SubscriptionData object if found and user is the one who established the subscription (or NACM recovery user), otherwise nullptr.
 */
std::shared_ptr<DynamicSubscriptions::SubscriptionData> DynamicSubscriptions::getSubscriptionForUser(const uint32_t subId, const std::optional<std::string>& user)
{
    std::unique_lock lock(m_mutex);

    // FIXME: This is linear search. Maybe use something like boost::multi_index?
    if (auto it = std::find_if(m_subscriptions.begin(), m_subscriptions.end(), [subId](const auto& entry) { return entry.second->subscription.subscriptionId() == subId; });
        it != m_subscriptions.end() && (it->second->user == user || user == sysrepo::Session::getNacmRecoveryUser())) {
        return it->second;
    }

    return nullptr;
}

boost::uuids::uuid DynamicSubscriptions::makeUUID()
{
    // uuid generator instance accesses must be synchronized (https://www.boost.org/doc/libs/1_88_0/libs/uuid/doc/html/uuid.html#design_notes)
    std::lock_guard lock(m_mutex);
    return m_uuidGenerator();
}

DynamicSubscriptions::SubscriptionData::SubscriptionData(
    sysrepo::DynamicSubscription sub,
    libyang::DataFormat format,
    boost::uuids::uuid uuid,
    const std::string& user,
    boost::asio::io_context& io,
    std::chrono::seconds inactivityTimeout,
    std::function<void()> onClientInactiveCallback)
    : subscription(std::move(sub))
    , dataFormat(format)
    , uuid(uuid)
    , user(user)
    , state(State::Start)
    , inactivityTimeout(inactivityTimeout)
    , clientInactiveTimer(io)
    , onClientInactiveCallback(std::move(onClientInactiveCallback))
{
    spdlog::debug("{}: created", fmt::streamed(*this));

    std::lock_guard lock(mutex);
    inactivityStart();
}

DynamicSubscriptions::SubscriptionData::~SubscriptionData()
{
    std::lock_guard lock(mutex);
    inactivityCancel();
    terminate();
}

void DynamicSubscriptions::SubscriptionData::clientDisconnected()
{
    spdlog::debug("{}: client disconnected", fmt::streamed(*this));

    std::lock_guard lock(mutex);
    if (state == State::Terminating) {
        return;
    }

    state = State::Start;
    inactivityStart();
}

void DynamicSubscriptions::SubscriptionData::clientConnected()
{
    spdlog::debug("{}: client connected", fmt::streamed(*this));
    std::lock_guard lock(mutex);
    inactivityCancel();
    state = State::ReceiverActive;
}

bool DynamicSubscriptions::SubscriptionData::isReadyToAcceptClient() const
{
    std::lock_guard lock(mutex);
    return state == State::Start;
}

/** @pre The mutex must be locked by the caller. */
void DynamicSubscriptions::SubscriptionData::inactivityStart()
{
    if (state == State::Terminating) {
        return;
    }

    spdlog::trace("{}: starting inactivity timer", fmt::streamed(*this));

    clientInactiveTimer.expires_after(inactivityTimeout);
    clientInactiveTimer.async_wait([weakThis = weak_from_this()](const boost::system::error_code& err) {
        auto self = weakThis.lock();
        if (!self || err == boost::asio::error::operation_aborted) {
            return;
        }

        spdlog::trace("{}: client inactive, perform inactivity callback", fmt::streamed(*self));
        self->onClientInactiveCallback();
    });
}

/** @pre The mutex must be locked by the caller. */
void DynamicSubscriptions::SubscriptionData::inactivityCancel()
{
    spdlog::trace("{}: cancelling inactivity timer", fmt::streamed(*this));
    clientInactiveTimer.cancel();
}

/** @pre The mutex must be locked by the caller. */
void DynamicSubscriptions::SubscriptionData::terminate(const std::optional<std::string>& reason)
{
    // already terminating, do nothing
    if (state == State::Terminating) {
        return;
    }

    state = State::Terminating;
    spdlog::debug("{}: terminating subscription ({})", fmt::streamed(*this), reason.value_or("<no reason>"));
    try {
        subscription.terminate(reason);
    } catch (const sysrepo::ErrorWithCode& e) { // Maybe it was already terminated (stop-time).
        spdlog::warn("Failed to terminate {}: {}", fmt::streamed(*this), e.what());
    }
}

void DynamicSubscriptions::SubscriptionData::stop()
{
    std::lock_guard lock(mutex);
    inactivityCancel();
    // We are already terminating and will destroy via destructor, calls to terminate() will do nothing
    state = SubscriptionData::State::Terminating;
}

std::ostream& operator<<(std::ostream& os, const DynamicSubscriptions::SubscriptionData& sub)
{
    return os << "dynamic subscription (id " << sub.subscription.subscriptionId()
              << ", user " << sub.user
              << ", uuid " << boost::uuids::to_string(sub.uuid)
              << ")";
}

DynamicSubscriptionHttpStream::DynamicSubscriptionHttpStream(
    const nghttp2::asio_http2::server::request& req,
    const nghttp2::asio_http2::server::response& res,
    rousette::http::EventStream::Termination& termination,
    std::shared_ptr<rousette::http::EventStream::EventSignal> signal,
    const std::chrono::seconds keepAlivePingInterval,
    const std::shared_ptr<DynamicSubscriptions::SubscriptionData>& subscriptionData)
    : EventStream(
          req,
          res,
          termination,
          *signal,
          keepAlivePingInterval,
          std::nullopt /* no initial event */,
          [this]() {
              std::lock_guard lock(m_subscriptionData->mutex);
              m_subscriptionData->terminate("ietf-subscribed-notifications:no-such-subscription");
          },
          [this]() { m_subscriptionData->clientDisconnected(); })
    , m_subscriptionData(subscriptionData)
    , m_signal(signal)
    , m_stream(res.io_service(), m_subscriptionData->subscription.fd())
{
}

DynamicSubscriptionHttpStream::~DynamicSubscriptionHttpStream()
{
    // The stream does not own the file descriptor, sysrepo does. It will be closed when the subscription terminates.
    m_stream.release();
}

/** @brief Waits for the next notifications and process them */
void DynamicSubscriptionHttpStream::awaitNextNotification()
{
    constexpr auto MAX_EVENTS = 50;

    m_stream.async_wait(boost::asio::posix::stream_descriptor::wait_read, [this](const boost::system::error_code& err) {
        // Unfortunately wait_read does not return operation_aborted when the file descriptor is closed and poll results in POLLHUP
        if (err == boost::asio::error::operation_aborted || utils::pipeIsClosedAndNoData(m_subscriptionData->subscription.fd())) {
            return;
        }

        size_t eventsProcessed = 0;
        /* Process all the available notifications, but at most N
         * In case sysrepo is providing the events fast enough, this loop would still run inside the event loop
         * and the event responsible for sending the data to the client would not get to be processed.
         * TODO: Is this enough? What if this async_wait keeps getting called and nothing gets sent?
         */
        while (++eventsProcessed < MAX_EVENTS && utils::pipeHasData(m_subscriptionData->subscription.fd())) {
            std::lock_guard lock(m_subscriptionData->mutex); // sysrepo-cpp's processEvent and terminate is not thread safe
            m_subscriptionData->subscription.processEvent([&](const std::optional<libyang::DataNode>& notificationTree, const sysrepo::NotificationTimeStamp& time) {
                (*m_signal)(rousette::restconf::as_restconf_notification(
                    m_subscriptionData->subscription.getSession().getContext(),
                    m_subscriptionData->dataFormat,
                    *notificationTree,
                    time));
            });
        }

        // and wait for more
        awaitNextNotification();
    });
}

void DynamicSubscriptionHttpStream::activate()
{
    m_subscriptionData->clientConnected();
    EventStream::activate();
    awaitNextNotification();
}

std::shared_ptr<DynamicSubscriptionHttpStream> DynamicSubscriptionHttpStream::create(
    const nghttp2::asio_http2::server::request& req,
    const nghttp2::asio_http2::server::response& res,
    rousette::http::EventStream::Termination& termination,
    const std::chrono::seconds keepAlivePingInterval,
    const std::shared_ptr<DynamicSubscriptions::SubscriptionData>& subscriptionData)
{
    auto signal = std::make_shared<rousette::http::EventStream::EventSignal>();
    auto stream = std::shared_ptr<DynamicSubscriptionHttpStream>(new DynamicSubscriptionHttpStream(req, res, termination, signal, keepAlivePingInterval, subscriptionData));
    stream->activate();
    return stream;
}
}
