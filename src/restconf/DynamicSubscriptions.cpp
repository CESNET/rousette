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
#include <set>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/Changes.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include <vector>
#include "restconf/DynamicSubscriptions.h"
#include "restconf/Exceptions.h"
#include "restconf/SubscribedNotifications.h"
#include "restconf/utils/sysrepo.h"
#include "restconf/utils/yang.h"

namespace {


/** @brief Converts a duration to the centisecond units used by the ietf-yang-push period/dampening-period leaves. */
std::string yangPushCentiseconds(const std::chrono::milliseconds ms)
{
    return std::to_string(std::chrono::duration_cast<std::chrono::duration<uint64_t, std::centi>>(ms).count());
}

/** @brief Builds the 'ietf-subscribed-notifications:subscription-modified' notification. */
libyang::DataNode subscriptionModifiedNotification(const libyang::Context& ctx, const sysrepo::SubscriptionState& state, const std::optional<rousette::restconf::ReferencedFilter>& configuredFilter)
{
    const auto referencedFilterName = configuredFilter ? std::make_optional(configuredFilter->name) : std::nullopt;

    auto notification = ctx.newPath("/ietf-subscribed-notifications:subscription-modified");
    notification.newPath("id", std::to_string(state.subscriptionId));

    if (state.stopTime) {
        notification.newPath("stop-time", libyang::yangTimeFormat(*state.stopTime, libyang::TimezoneInterpretation::Local));
    }

    if (const auto* sn = std::get_if<sysrepo::SubscribedNotifications>(&state.params)) {
        notification.newPath("stream", sn->stream);
        if (referencedFilterName) {
            notification.newPath("stream-filter-name", *referencedFilterName);
        } else if (state.xpathFilter) {
            notification.newPath("stream-xpath-filter", *state.xpathFilter);
        }
    } else if (const auto* periodic = std::get_if<sysrepo::YangPushPeriodic>(&state.params)) {
        notification.newPath("ietf-yang-push:datastore", rousette::restconf::datastoreToString(periodic->datastore));
        if (referencedFilterName) {
            notification.newPath("ietf-yang-push:selection-filter-ref", *referencedFilterName);
        } else if (state.xpathFilter) {
            notification.newPath("ietf-yang-push:datastore-xpath-filter", *state.xpathFilter);
        }
        notification.newPath("ietf-yang-push:periodic/period", yangPushCentiseconds(periodic->period));
        if (periodic->anchorTime) {
            notification.newPath("ietf-yang-push:periodic/anchor-time", libyang::yangTimeFormat(*periodic->anchorTime, libyang::TimezoneInterpretation::Local));
        }
    } else if (const auto* onChange = std::get_if<sysrepo::YangPushOnChange>(&state.params)) {
        notification.newPath("ietf-yang-push:datastore", rousette::restconf::datastoreToString(onChange->datastore));
        if (referencedFilterName) {
            notification.newPath("ietf-yang-push:selection-filter-ref", *referencedFilterName);
        } else if (state.xpathFilter) {
            notification.newPath("ietf-yang-push:datastore-xpath-filter", *state.xpathFilter);
        }
        notification.newPath("ietf-yang-push:on-change/dampening-period", yangPushCentiseconds(onChange->dampeningPeriod));
    }

    return notification;
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
        sysrepo::subscribedNotificationsStreams,
        "/ietf-subscribed-notifications:streams",
        sysrepo::SubscribeOptions::OperMerge);

    // Watch for changes of the configured filters so that subscriptions referring to them can be updated.
    ScopedDatastoreSwitch dsSwitch(session, sysrepo::Datastore::Running);
    m_filtersChangeSub = session.onModuleChange(
        "ietf-subscribed-notifications",
        [this](sysrepo::Session changeSession, auto, auto, auto, auto, auto) { return onConfiguredFilterChange(changeSession); },
        "/ietf-subscribed-notifications:filters",
        0,
        sysrepo::SubscribeOptions::DoneOnly);
}

DynamicSubscriptions::~DynamicSubscriptions() = default;

void DynamicSubscriptions::stop()
{
    std::lock_guard lock(m_mutex);
    for (const auto& [uuid, subscriptionData] : m_subscriptions) {
        subscriptionData->stop();
    }
}

void DynamicSubscriptions::establishSubscription(sysrepo::Session& session, const std::optional<std::string>& requestSchemeAndHost, const libyang::DataFormat requestEncoding, const libyang::DataNode& rpcInput, libyang::DataNode& rpcOutput)
{
    if (!requestSchemeAndHost) {
        throw ErrorResponse(400, "application", "invalid-value", "Request scheme and host information is required to establish subscription.");
    }

    // Generate a new UUID associated with the subscription. The UUID will be used as a part of the URI so that the URI is not predictable (RFC 8650, section 5)
    auto uuid = makeUUID();

    auto dataFormat = subscriptionEncoding(rpcInput, requestEncoding);

    try {
        auto sub = makeSubscription(session, rpcInput);

        /* Node replay-start-time-revision should be set only if time was revised to be different than the requested start time,
         * i.e. when the "replay-start-time" contains a value that is earlier than what a publisher's retained history.
         * Then the actual publisher's revised start time MUST be set in the returned "replay-start-time-revision" object.
         * (RFC 8639, 2.4.2.1)
         * */
        if (auto replayStartTimeRevision = sub.replayStartTime(); replayStartTimeRevision && rpcInput.findPath("replay-start-time")) {
            rpcOutput.newPath("replay-start-time-revision", libyang::yangTimeFormat(*replayStartTimeRevision, libyang::TimezoneInterpretation::Local), libyang::CreationOptions::Output);
        }

        // read the id before sub gets moved from; function arguments are indeterminately sequenced
        auto subId = sub.subscriptionId();

        rpcOutput.newPath("id", std::to_string(subId), libyang::CreationOptions::Output);
        rpcOutput.newPath("ietf-restconf-subscribed-notifications:uri", *requestSchemeAndHost + m_restconfStreamUri + "subscribed/" + boost::uuids::to_string(uuid), libyang::CreationOptions::Output);

        std::lock_guard lock(m_mutex);
        m_subscriptions[uuid] = std::make_shared<SubscriptionData>(
            std::move(sub),
            dataFormat,
            uuid,
            *session.getNacmUser(),
            referencedFilter(rpcInput),
            *m_server.io_services().at(0),
            m_inactivityTimeout,
            [this, subId]() { terminateSubscription(subId); });
    } catch (const sysrepo::ErrorWithCode& e) {
        throw ErrorResponse(400, "application", "invalid-attribute", e.what());
    }
}

void DynamicSubscriptions::deleteSubscription(sysrepo::Session& session, [[maybe_unused]] const std::optional<std::string>& requestSchemeAndHost, const libyang::DataFormat, const libyang::DataNode& rpcInput, libyang::DataNode&)
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

void DynamicSubscriptions::modifySubscription(sysrepo::Session& session, [[maybe_unused]] const std::optional<std::string>& requestSchemeAndHost, const libyang::DataFormat, const libyang::DataNode& rpcInput, libyang::DataNode&)
{
    const auto subId = std::get<uint32_t>(rpcInput.findPath("id")->asTerm().value());

    // The RPC is already NACM-checked. Now, retrieve the subscription, if the current user has permission for it
    auto subscriptionData = getSubscriptionForUser(subId, session.getNacmUser());
    if (!subscriptionData) {
        throw ErrorResponse(404, "application", "invalid-value", "Subscription not found.", rpcInput.path());
    }

    // The set of modifiable parameters and the YANG nodes that carry the filter depend on the subscription type.
    const auto filter = subscriptionData->subscription.type() == sysrepo::DynamicSubscriptionType::SubscribedNotifications
        ? createFilter(session, rpcInput, streamFilter, streamFilterKey, "stream-xpath-filter", "stream-subtree-filter", "stream-filter-name")
        : createFilter(session, rpcInput, selectionFilter, selectionFilterKey, "ietf-yang-push:datastore-xpath-filter", "ietf-yang-push:datastore-subtree-filter", "ietf-yang-push:selection-filter-ref");

    std::lock_guard lock(subscriptionData->mutex);
    try {
        /* TODO: This is not atomic. If modifying one parameter fails after another was already modified, the subscription
         * is left in a partially modified state and that probably violates the semantics required by RFC 8639, 2.7. */
        subscriptionData->subscription.modifyFilter(filter);

        switch (subscriptionData->subscription.type()) {
        case sysrepo::DynamicSubscriptionType::SubscribedNotifications:
            break;
        case sysrepo::DynamicSubscriptionType::YangPushPeriodic:
            // The 'period' leaf is mandatory inside the 'periodic' container, so it is present whenever the container is.
            if (auto period = createInterval<std::centi>(rpcInput, "ietf-yang-push:periodic/period")) {
                subscriptionData->subscription.modifyPeriod(*period, optionalTime(rpcInput, "ietf-yang-push:periodic/anchor-time"));
            }
            break;
        case sysrepo::DynamicSubscriptionType::YangPushOnChange:
            if (rpcInput.findPath("ietf-yang-push:on-change")) {
                subscriptionData->subscription.modifyDampeningPeriod(createInterval<std::centi>(rpcInput, "ietf-yang-push:on-change/dampening-period"));
            }
            break;
        }

        subscriptionData->subscription.modifyStopTime(optionalTime(rpcInput, "stop-time"));
        subscriptionData->configuredFilter = referencedFilter(rpcInput);
    } catch (const sysrepo::ErrorWithCode& e) {
        throw ErrorResponse(400, "application", "invalid-attribute", e.what());
    }

    /* Notify the connected receiver (if any) with subscription-modified.
     * FIXME: Only best-effort ordering (RFC 8639, 2.7.2): new-params records come after the marker (produced only after modify_*()),
     * but old-params records still buffered unread in sysrepo's pipe may also land after it.
     * There is (probably) no way of stopping sysrepo producing new events temporarily?
     */
    if (auto sink = subscriptionData->notificationSink.lock()) {
        try {
            auto notification = subscriptionModifiedNotification(session.getContext(), subscriptionData->subscription.subscriptionState(), subscriptionData->configuredFilter);
            (*sink)(as_restconf_notification(
                session.getContext(),
                subscriptionData->dataFormat,
                notification,
                std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now())));
            subscriptionData->subscription.notifSent();
        } catch (const std::exception& e) {
            spdlog::warn("{}: failed to send subscription-modified notification: {}", fmt::streamed(*subscriptionData), e.what());
        }
    }
}

sysrepo::ErrorCode DynamicSubscriptions::onConfiguredFilterChange(sysrepo::Session session)
{
    // Collect the configured filters (their instance xpaths) that changed, and which of them were removed entirely.
    std::set<std::string> changedFilters;
    std::set<std::string> deletedFilters;
    for (const auto& change : session.getChanges()) {
        auto entry = configuredFilterEntry(change.node);
        if (!entry) {
            continue;
        }

        const auto xpath = entry->path();
        changedFilters.insert(xpath);
        if (change.operation == sysrepo::ChangeOperation::Deleted && change.node.schema().nodeType() == libyang::NodeType::List) {
            // the whole list entry was removed, not just one of its children
            deletedFilters.insert(xpath);
        }
    }

    std::lock_guard lock(m_mutex);
    std::vector<boost::uuids::uuid> toErase;

    for (const auto& [uuid, subscriptionData] : m_subscriptions) {
        std::lock_guard subLock(subscriptionData->mutex);
        if (!subscriptionData->configuredFilter) {
            continue;
        }

        const auto filterXPath = subscriptionData->configuredFilter->configuredXPath();
        if (!changedFilters.contains(filterXPath)) {
            continue;
        }

        if (deletedFilters.contains(filterXPath)) {
            // RFC 8639, 2.7.3: the referenced filter no longer exists, so the subscription has to be terminated.
            spdlog::debug("{}: referenced filter was removed, terminating", fmt::streamed(*subscriptionData));
            subscriptionData->terminate("ietf-subscribed-notifications:filter-unavailable");
            toErase.emplace_back(uuid);
            continue;
        }

        /* Re-resolve the filter from the (now updated) configuration and apply it.
         * RFC 8639, 2.7.2: a change of a referenced filter must be reflected in all subscriptions using it.
         */
        try {
            subscriptionData->subscription.modifyFilter(resolveConfiguredFilter(session, filterXPath));
            spdlog::debug("{}: filter updated after configuration change", fmt::streamed(*subscriptionData));
        } catch (const sysrepo::ErrorWithCode& e) {
            spdlog::warn("{}: failed to update filter after configuration change: {}", fmt::streamed(*subscriptionData), e.what());
        }
    }

    for (const auto& uuid : toErase) {
        m_subscriptions.erase(uuid);
    }

    return sysrepo::ErrorCode::Ok;
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
    const std::optional<ReferencedFilter>& configuredFilter,
    boost::asio::io_context& io,
    std::chrono::seconds inactivityTimeout,
    std::function<void()> onClientInactiveCallback)
    : subscription(std::move(sub))
    , dataFormat(format)
    , uuid(uuid)
    , user(user)
    , configuredFilter(configuredFilter)
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
    notificationSink.reset();
    inactivityStart();
}

void DynamicSubscriptions::SubscriptionData::clientConnected(const std::shared_ptr<http::EventStream::EventSignal>& sink)
{
    spdlog::debug("{}: client connected", fmt::streamed(*this));
    std::lock_guard lock(mutex);
    notificationSink = sink;
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
    , m_io(res.io_service())
{
}

void DynamicSubscriptionHttpStream::activate()
{
    m_subscriptionData->clientConnected(m_signal);
    EventStream::activate();
    m_broadcaster = std::make_unique<SubscriptionBroadcaster>(m_io, m_subscriptionData->subscription, m_subscriptionData->dataFormat, m_signal, m_subscriptionData->mutex);
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
