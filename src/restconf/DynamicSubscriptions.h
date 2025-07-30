#pragma once

#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/uuid/random_generator.hpp>
#include <map>
#include <memory>
#include <sysrepo-cpp/Subscription.hpp>

namespace libyang {
enum class DataFormat;
}

namespace nghttp2::asio_http2::server {
class http2;
}

namespace rousette::restconf {

/** Dynamic subscriptions manager.
 *
 * Stores all dynamic subscriptions and provides a way to retrieve them by the UUID.
 * */
class DynamicSubscriptions {
public:
    struct SubscriptionData : public std::enable_shared_from_this<SubscriptionData> {
        std::mutex mutex; ///< Guarding access to sysrepo API
        sysrepo::DynamicSubscription subscription;
        libyang::DataFormat dataFormat; ///< Encoding of the notification stream
        boost::uuids::uuid uuid; ///< UUID is part of the GET URI, it identifies subscriptions for clients
        std::string user; ///< User who initiated the establish-subscription RPC

        enum class State {
            Start, ///< Subscription is ready to be consumed by a client
            ReceiverActive, ///< Subscription is being consumed by a client
            Terminating,
        } state;

        std::chrono::seconds inactivityTimeout; ///< Time after which the subscription is considered inactive and can be removed if no client is connected
        boost::asio::system_timer clientInactiveTimer; ///< Timer used for auto-destruction of subscriptions that are unused
        std::function<void()> onClientInactiveCallback;

        SubscriptionData(
            sysrepo::DynamicSubscription sub,
            libyang::DataFormat format,
            boost::uuids::uuid uuid,
            const std::string& user,
            boost::asio::io_context& io,
            std::chrono::seconds inactivityTimeout,
            std::function<void()> onClientInactiveCallback);
        ~SubscriptionData();
        void clientDisconnected();
        void clientConnected();
        void terminate(const std::optional<std::string>& reason);
        bool isReadyToAcceptClient() const;

        void inactivityStart();
        void inactivityCancel();
    };

    DynamicSubscriptions(const std::string& streamRootUri, const nghttp2::asio_http2::server::http2& server, const std::chrono::seconds inactivityTimeout);
    ~DynamicSubscriptions();
    void stop();
    std::shared_ptr<SubscriptionData> getSubscriptionForUser(const boost::uuids::uuid& uuid, const std::optional<std::string>& user);
    void establishSubscription(sysrepo::Session& session, const libyang::DataFormat requestEncoding, const libyang::DataNode& rpcInput, libyang::DataNode& rpcOutput);

private:
    std::mutex m_mutex;
    std::string m_restconfStreamUri;
    const nghttp2::asio_http2::server::http2& m_server;
    std::map<boost::uuids::uuid, std::shared_ptr<SubscriptionData>> m_subscriptions;
    boost::uuids::random_generator m_uuidGenerator;
    std::chrono::seconds m_inactivityTimeout;

    void terminateSubscription(const uint32_t subId);
};
}
