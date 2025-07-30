#include <boost/uuid/uuid_io.hpp>
#include <fmt/ostream.h>
#include <libyang-cpp/Time.hpp>
#include <nghttp2/asio_http2_server.h>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/utils/exception.hpp>
#include "configure.cmake.h"
#include "restconf/DynamicSubscriptions.h"
#include "restconf/Exceptions.h"

namespace {

/** @brief Parses the YANG date-and-time attribute from the RPC input and returns it as an optional sysrepo::NotificationTimeStamp.
 *
 * @param rpcInput The RPC input node.
 * @param path The path to the YANG leaf.
 * @return An optional sysrepo::NotificationTimeStamp if the attribute is present and valid, otherwise std::nullopt.
 */
template <typename T>
std::optional<sysrepo::NotificationTimeStamp> optionalTime(const libyang::DataNode& rpcInput, const std::string& path)
{
    if (auto stopTimeNode = rpcInput.findPath(path)) {
        return libyang::fromYangTimeFormat<typename T::clock>(stopTimeNode->asTerm().valueStr());
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

sysrepo::DynamicSubscription getStream(sysrepo::Session& session, const libyang::DataNode& rpcInput)
{
    if (!rpcInput.findPath("stream")) {
        throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "Stream is required");
    }

    if (rpcInput.findPath("stream-filter-name")) {
        throw rousette::restconf::ErrorResponse(400, "application", "invalid-attribute", "Stream filtering is not supported");
    }

    auto stopTime = optionalTime<sysrepo::NotificationTimeStamp>(rpcInput, "stop-time");

    return session.subscribeNotifications(
        std::nullopt /* TODO xpath filter */,
        rpcInput.findPath("stream")->asTerm().valueStr(),
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
    auto uuid = m_uuidGenerator();

    auto dataFormat = getEncoding(rpcInput, requestEncoding);

    try {
        std::optional<sysrepo::DynamicSubscription> sub;

        if (rpcInput.findPath("stream")) {
            sub = getStream(session, rpcInput);
        } else {
            throw ErrorResponse(400, "application", "missing-attribute", "stream attribute is required");
        }

        rpcOutput.newPath("id", std::to_string(sub->subscriptionId()), libyang::CreationOptions::Output);
        rpcOutput.newPath("ietf-restconf-subscribed-notifications:uri", m_restconfStreamUri + "subscribed/" + boost::uuids::to_string(uuid), libyang::CreationOptions::Output);

        std::unique_lock lock(m_mutex);
        m_subscriptions[uuid] = std::make_shared<SubscriptionData>(
            std::move(*sub),
            dataFormat,
            uuid,
            *session.getNacmUser(),
            *m_server.io_services().at(0),
            m_inactivityTimeout,
            [this, subId = sub->subscriptionId()]() { terminateSubscription(subId); });
        m_subscriptions[uuid]->inactivityStart();
    } catch (const sysrepo::ErrorWithCode& e) {
        throw ErrorResponse(400, "application", "invalid-attribute", e.what());
    }
}

void DynamicSubscriptions::terminateSubscription(const uint32_t subId)
{
    std::unique_lock lock(m_mutex);

    for (const auto& [uuid, subscriptionData] : m_subscriptions) { // TODO: Store by both id and uuid in order to search fast by both keys
        if (subscriptionData->subscription.subscriptionId() == subId) {
            spdlog::debug("{}: termination requested", fmt::streamed(*subscriptionData));
            subscriptionData->terminate("ietf-subscribed-notifications:no-such-subscription");
            m_subscriptions.erase(uuid);
            return;
        }
    }

    spdlog::warn("Requested termination of subscription with id {}, but subscription not found", subId);
}

/** @brief Returns the subscription data for the given UUID and user.
 *
 * @param uuid The UUID of the subscription.
 * @return A shared pointer to the SubscriptionData object if found and user is the one who established the subscription (or NACM recovery user), otherwise nullptr.
 */
std::shared_ptr<DynamicSubscriptions::SubscriptionData> DynamicSubscriptions::getSubscriptionForUser(const boost::uuids::uuid& uuid, const std::optional<std::string>& user)
{
    std::unique_lock lock(m_mutex);
    if (auto it = m_subscriptions.find(uuid); it != m_subscriptions.end() && (it->second->user == user || user == sysrepo::Session::getNacmRecoveryUser())) {
        return it->second;
    }

    return nullptr;
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
        if (state != State::Terminating) {
            subscription.terminate();
        }
    } catch (const sysrepo::ErrorWithCode& e) { // Maybe it was already terminated (stop-time).
        spdlog::warn("Failed to terminate {}: {}", fmt::streamed(*this), e.what());
    }

    inactivityCancel();
}

void DynamicSubscriptions::SubscriptionData::clientDisconnected()
{
    spdlog::debug("{}: client disconnected", fmt::streamed(*this));

    if (state == State::Terminating) {
        return;
    }

    state = State::Start;
    inactivityStart();
}

void DynamicSubscriptions::SubscriptionData::clientConnected()
{
    spdlog::debug("{}: client connected", fmt::streamed(*this));
    state = State::ReceiverActive;
    inactivityCancel();
}

bool DynamicSubscriptions::SubscriptionData::isReadyToAcceptClient() const
{
    return state == State::Start;
}

void DynamicSubscriptions::SubscriptionData::terminate(const std::optional<std::string>& reason)
{
    spdlog::debug("{}: terminating subscription ({})", fmt::streamed(*this), reason.value_or("<no reason>"));
    std::lock_guard lock(mutex);
    state = State::Terminating;
    subscription.terminate(reason);
}

void DynamicSubscriptions::SubscriptionData::inactivityStart()
{
    spdlog::trace("{}: starting inactivity timer", fmt::streamed(*this));
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
    clientInactiveTimer.cancel();
}

std::ostream& operator<<(std::ostream& os, const DynamicSubscriptions::SubscriptionData& sub)
{
    return os << "dynamic subscription (id " << sub.subscription.subscriptionId()
              << ", user " << sub.user
              << ", uuid " << boost::uuids::to_string(sub.uuid)
              << ")";
}
}
