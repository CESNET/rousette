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
#include "restconf/utils/yang.h"

namespace {

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

sysrepo::DynamicSubscription makeStreamSubscription(sysrepo::Session& session, const libyang::DataNode& rpcInput)
{
    auto streamNode = rpcInput.findPath("stream");

    if (!streamNode) {
        throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "Stream is required");
    }

    if (rpcInput.findPath("stream-filter-name")) {
        throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "Stream filtering is not supported");
    }

    auto stopTime = optionalTime(rpcInput, "stop-time");

    return session.subscribeNotifications(
        std::nullopt /* TODO xpath filter */,
        streamNode->asTerm().valueStr(),
        stopTime,
        std::nullopt /* TODO replayStart */);
}
}

namespace rousette::restconf {

DynamicSubscriptions::DynamicSubscriptions(const std::string& streamRootUri, const nghttp2::asio_http2::server::http2& server, const std::chrono::seconds inactivityTimeout)
    : m_restconfStreamUri(streamRootUri)
    , m_server(server)
    , m_uuidGenerator(boost::uuids::random_generator())
    , m_inactivityTimeout(inactivityTimeout)
{
}

DynamicSubscriptions::~DynamicSubscriptions() = default;

void DynamicSubscriptions::stop()
{
    std::lock_guard lock(m_mutex);
    for (const auto& [uuid, subscriptionData] : m_subscriptions) {
        subscriptionData->inactivityCancel();
    }
}

void DynamicSubscriptions::establishSubscription(sysrepo::Session& session, const libyang::DataFormat requestEncoding, const libyang::DataNode& rpcInput, libyang::DataNode& rpcOutput)
{
    // Generate a new UUID associated with the subscription. The UUID will be used as a part of the URI so that the URI is not predictable (RFC 8650, section 5)
    auto uuid = makeUUID();

    auto dataFormat = getEncoding(rpcInput, requestEncoding);

    try {
        auto sub = makeStreamSubscription(session, rpcInput);

        rpcOutput.newPath("id", std::to_string(sub.subscriptionId()), libyang::CreationOptions::Output);
        rpcOutput.newPath("ietf-restconf-subscribed-notifications:uri", m_restconfStreamUri + "subscribed/" + boost::uuids::to_string(uuid), libyang::CreationOptions::Output);

        std::lock_guard lock(m_mutex);
        m_subscriptions[uuid] = std::make_shared<SubscriptionData>(
            std::move(sub),
            dataFormat,
            uuid,
            *session.getNacmUser(),
            *m_server.io_services().at(0),
            m_inactivityTimeout,
            [this, subId = sub.subscriptionId()]() { terminateSubscription(subId); });
        m_subscriptions[uuid]->inactivityStart();
    } catch (const sysrepo::ErrorWithCode& e) {
        throw ErrorResponse(400, "application", "invalid-attribute", e.what());
    }
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
}

DynamicSubscriptions::SubscriptionData::~SubscriptionData()
{
    try {
        terminate();
    } catch (const sysrepo::ErrorWithCode& e) { // Maybe it was already terminated (stop-time).
        spdlog::warn("Failed to terminate {}: {}", fmt::streamed(*this), e.what());
    }

    inactivityCancel();
}

void DynamicSubscriptions::SubscriptionData::clientDisconnected()
{
    spdlog::debug("{}: client disconnected", fmt::streamed(*this));

    {
        std::lock_guard lock(mutex);
        if (state == State::Terminating) {
            return;
        }

        state = State::Start;
    }

    inactivityStart();
}

void DynamicSubscriptions::SubscriptionData::clientConnected()
{
    spdlog::debug("{}: client connected", fmt::streamed(*this));

    inactivityCancel();

    std::lock_guard lock(mutex);
    state = State::ReceiverActive;
}

bool DynamicSubscriptions::SubscriptionData::isReadyToAcceptClient() const
{
    std::lock_guard lock(mutex);
    return state == State::Start;
}

void DynamicSubscriptions::SubscriptionData::inactivityStart()
{
    spdlog::trace("{}: starting inactivity timer", fmt::streamed(*this));
    std::lock_guard lock(mutex);
    clientInactiveTimer.expires_after(inactivityTimeout);
    clientInactiveTimer.async_wait([weakThis = weak_from_this()](const boost::system::error_code& err) {
        auto self = weakThis.lock();
        if (!self) {
            return;
        }

        if (err == boost::asio::error::operation_aborted) {
            return;
        }

        spdlog::trace("{}: client inactive, perform inactivity callback", fmt::streamed(*self));
        self->onClientInactiveCallback();
    });
}

void DynamicSubscriptions::SubscriptionData::inactivityCancel()
{
    spdlog::trace("{}: cancelling inactivity timer", fmt::streamed(*this));
    std::lock_guard lock(mutex);
    clientInactiveTimer.cancel();
}

void DynamicSubscriptions::SubscriptionData::terminate(const std::optional<std::string>& reason)
{
    std::lock_guard lock(mutex);

    // already terminating, do nothing
    if (state == State::Terminating) {
        return;
    }

    state = State::Terminating;
    spdlog::debug("{}: terminating subscription ({})", fmt::streamed(*this), reason.value_or("<no reason>"));
    subscription.terminate(reason);
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
          [this]() { m_subscriptionData->terminate("ietf-subscribed-notifications:no-such-subscription"); },
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
