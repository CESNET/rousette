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

DynamicSubscriptions::DynamicSubscriptions(const std::string& streamRootUri)
    : m_restconfStreamUri(streamRootUri)
    , m_uuidGenerator(boost::uuids::random_generator())
{
}

DynamicSubscriptions::~DynamicSubscriptions() = default;

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
            *session.getNacmUser());
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
    subscriptionData->subscription.terminate("ietf-subscribed-notifications:no-such-subscription");
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
    const std::string& user)
    : subscription(std::move(sub))
    , dataFormat(format)
    , uuid(uuid)
    , user(user)
{
    spdlog::debug("{}: created", fmt::streamed(*this));
}

DynamicSubscriptions::SubscriptionData::~SubscriptionData()
{
    try {
        subscription.terminate();
    } catch (const sysrepo::ErrorWithCode& e) { // Maybe it was already terminated (stop-time).
        spdlog::warn("Failed to terminate {}: {}", fmt::streamed(*this), e.what());
    }
}

std::ostream& operator<<(std::ostream& os, const DynamicSubscriptions::SubscriptionData& sub)
{
    return os << "dynamic subscription (id " << sub.subscription.subscriptionId()
              << ", user " << sub.user
              << ", uuid " << boost::uuids::to_string(sub.uuid)
              << ")";
}
}
