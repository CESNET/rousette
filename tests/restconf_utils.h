/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#pragma once
#include "trompeloeil_doctest.h"
#include <nghttp2/asio_http2_client.h>
#include <semaphore>
#include "event_watchers.h"
#include "UniqueResource.h"

namespace sysrepo {
class Session;
}

namespace ng = nghttp2::asio_http2;
namespace ng_client = ng::client;

struct Response {
    int statusCode;
    ng::header_map headers;
    std::string data;

    using Headers = std::multimap<std::string, std::string>;

    Response(int statusCode, const Headers& headers, const std::string& data);
    Response(int statusCode, const ng::header_map& headers, const std::string& data);
    bool equalStatusCodeAndHeaders(const Response& o) const;
    bool operator==(const Response& o) const;
    static ng::header_map transformHeaders(const Headers& headers);
};

namespace doctest {

template <>
struct StringMaker<ng::header_map> {
    static String convert(const ng::header_map& m)
    {
        std::ostringstream oss;
        oss << "{\n";
        for (const auto& [k, v] : m) {
            oss << "\t"
                << "{\"" << k << "\", "
                << "{\"" << v.value << "\", " << std::boolalpha << v.sensitive << "}},\n";
        }
        oss << "}";
        return oss.str().c_str();
    }
};

template <>
struct StringMaker<Response> {
    static String convert(const Response& o)
    {
        std::ostringstream oss;

        oss << "{"
            << std::to_string(o.statusCode) << ", "
            << StringMaker<decltype(o.headers)>::convert(o.headers) << ",\n"
            << "\"" << o.data << "\",\n"
            << "}";

        return oss.str().c_str();
    }
};
}

// this is a test, and the server is expected to reply "soon"
static const boost::posix_time::time_duration CLIENT_TIMEOUT = boost::posix_time::seconds(3);

Response clientRequest(
    const std::string& server_address,
    const std::string& server_port,
    const std::string& method,
    const std::string& uri,
    const std::string& data,
    const std::map<std::string, std::string>& headers,
    const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT);

UniqueResource manageNacm(sysrepo::Session session);
void setupRealNacm(sysrepo::Session session);

struct SSEClient {
    std::shared_ptr<ng_client::session> client;
    boost::asio::steady_timer t;
    std::string dataBuffer;

    enum class ReportIgnoredLines {
        No,
        Yes,
    };

    SSEClient(
        boost::asio::io_service& io,
        const std::string& server_address,
        const std::string& server_port,
        std::binary_semaphore& requestSent,
        const RestconfNotificationWatcher& eventWatcher,
        const std::string& uri,
        const std::map<std::string, std::string>& headers,
        const std::chrono::seconds silenceTimeout = std::chrono::seconds{1}, // test code; the server should respond "soon"
        const ReportIgnoredLines reportIgnoredLines = ReportIgnoredLines::No);

    void parseEvents(const RestconfNotificationWatcher& eventWatcher, const ReportIgnoredLines reportIgnoredLines);
};

#define PREPARE_LOOP_WITH_EXCEPTIONS \
    boost::asio::io_service io; \
    std::promise<void> bg; \
    std::binary_semaphore requestSent(0);

#define RUN_LOOP_WITH_EXCEPTIONS \
    do { \
        io.run(); \
        auto fut = bg.get_future(); \
        REQUIRE(fut.wait_for(666ms /* "plenty of time" for the notificationThread to exit after it has called io.stop() */) == std::future_status::ready); \
        fut.get(); \
    } while (false)

#define WAIT_UNTIL_SSE_CLIENT_REQUESTS requestSent.try_acquire_for(std::chrono::seconds(3))

inline auto wrap_exceptions_and_asio(std::promise<void>& bg, boost::asio::io_service& io, std::function<void()> func)
{
    return [&bg, &io, func]()
    {
        try {
            func();
        } catch (...) {
            bg.set_exception(std::current_exception());
            return;
        }
        bg.set_value();
        io.stop();
    };
}

struct EstablishSubscriptionResult {
    uint32_t id;
    std::string url;
    std::optional<sysrepo::NotificationTimeStamp> replayStartTimeRevision;
};

struct FilterXPath {
    std::string xpath;
};
struct FilterName {
    std::string name;
};
using Filter = std::variant<std::monostate, FilterXPath, libyang::DataNode, FilterName>;

struct SubscribedNotifications {
    std::string stream;
    Filter filter;
    std::optional<sysrepo::NotificationTimeStamp> replayStartTime;
};

struct YangPushBase {
    sysrepo::Datastore datastore;
    Filter filter;
};

struct YangPushOnChange : public YangPushBase {
    std::optional<std::chrono::milliseconds> dampeningPeriod;
    std::optional<sysrepo::SyncOnStart> syncOnStart;
    std::vector<std::string> excludedChangeTypes;
};

struct YangPushPeriodic : public YangPushBase {
    std::chrono::milliseconds period;
    std::optional<sysrepo::NotificationTimeStamp> anchorTime;
};

/** @brief Calls establish-subscription rpc, returns the url of the stream associated with the created subscription */
EstablishSubscriptionResult establishSubscription(
    const std::string& serverAddress,
    const std::string& serverPort,
    const libyang::Context& ctx,
    const libyang::DataFormat rpcEncoding,
    const std::optional<std::pair<std::string, std::string>>& rpcRequestAuthHeader,
    const std::optional<std::string>& encodingLeafValue,
    const std::variant<SubscribedNotifications, YangPushOnChange, YangPushPeriodic>& params);

void createNamedSubtreeFilter(sysrepo::Session& session, const std::string& path, const std::string& xmlContent);

#define CREATE_SUBTREE_STREAM_FILTER(SESS, NAME, XML) \
    createNamedSubtreeFilter(SESS, "/ietf-subscribed-notifications:filters/stream-filter[name='" NAME "']/stream-subtree-filter", XML)
#define CREATE_SUBTREE_SELECTION_FILTER(SESS, NAME, XML) \
    createNamedSubtreeFilter(SESS, "/ietf-subscribed-notifications:filters/ietf-yang-push:selection-filter[filter-id='" NAME "']/datastore-subtree-filter", XML)
