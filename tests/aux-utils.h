/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#pragma once
#include "trompeloeil_doctest.h"
#include <latch>
#include <nghttp2/asio_http2_client.h>
#include "UniqueResource.h"
#include "eventWatchers.h"

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

static const auto SERVER_ADDRESS = "::1";
static const auto SERVER_ADDRESS_AND_PORT = std::string("http://[") + SERVER_ADDRESS + "]" + ":" + SERVER_PORT;

#define AUTH_DWDM {"authorization", "Basic ZHdkbTpEV0RN"}
#define AUTH_NORULES {"authorization", "Basic bm9ydWxlczplbXB0eQ=="}
#define AUTH_ROOT {"authorization", "Basic cm9vdDpzZWtyaXQ="}
#define AUTH_WRONG_PASSWORD {"authorization", "Basic ZHdkbTpGQUlM"}

#define FORWARDED {"forward", "proto=http;host=example.net"}

#define CONTENT_TYPE_JSON {"content-type", "application/yang-data+json"}
#define CONTENT_TYPE_XML {"content-type", "application/yang-data+xml"}

#define CONTENT_TYPE_YANG_PATCH_JSON {"content-type", "application/yang-patch+json"}
#define CONTENT_TYPE_YANG_PATCH_XML {"content-type", "application/yang-patch+xml"}

#define YANG_ROOT "/yang"
#define RESTCONF_ROOT "/restconf"
#define RESTCONF_DATA_ROOT RESTCONF_ROOT "/data"
#define RESTCONF_OPER_ROOT RESTCONF_ROOT "/operations"
#define RESTCONF_ROOT_DS(NAME) RESTCONF_ROOT "/ds/ietf-datastores:" NAME

const ng::header_map jsonHeaders{
    {"access-control-allow-origin", {"*", false}},
    {"content-type", {"application/yang-data+json", false}},
};

const ng::header_map xmlHeaders{
    {"access-control-allow-origin", {"*", false}},
    {"content-type", {"application/yang-data+xml", false}},
};

const ng::header_map noContentTypeHeaders{
    {"access-control-allow-origin", {"*", false}},
};

const ng::header_map yangHeaders{
    {"access-control-allow-origin", {"*", false}},
    {"content-type", {"application/yang", false}},
};

const ng::header_map plaintextHeaders{
    {"access-control-allow-origin", {"*", false}},
    {"content-type", {"text/plain", false}},
};

const ng::header_map eventStreamHeaders{
    {"access-control-allow-origin", {"*", false}},
    {"content-type", {"text/event-stream", false}},
};

#define ACCESS_CONTROL_ALLOW_ORIGIN {"access-control-allow-origin", "*"}
#define ACCEPT_PATCH {"accept-patch", "application/yang-data+json, application/yang-data+xml, application/yang-patch+xml, application/yang-patch+json"}

static const boost::posix_time::time_duration CLIENT_TIMEOUT = boost::posix_time::seconds(3);

Response clientRequest(
    const std::string& address,
    const std::string& port,
    const std::string& method,
    const std::string& uri,
    const std::string& data,
    const std::map<std::string, std::string>& headers,
    // this is a test, and the server is expected to reply "soon"
    const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT);

Response get(auto uri, const std::map<std::string, std::string>& headers, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "GET", uri, "", headers, timeout);
}

Response options(auto uri, const std::map<std::string, std::string>& headers, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "OPTIONS", uri, "", headers, timeout);
}

Response head(auto uri, const std::map<std::string, std::string>& headers, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "HEAD", uri, "", headers, timeout);
}

Response put(auto xpath, const std::map<std::string, std::string>& headers, const std::string& data, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "PUT", xpath, data, headers, timeout);
}

Response post(auto xpath, const std::map<std::string, std::string>& headers, const std::string& data, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "POST", xpath, data, headers, timeout);
}

Response patch(auto uri, const std::map<std::string, std::string>& headers, const std::string& data, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "PATCH", uri, data, headers, timeout);
}

Response httpDelete(auto uri, const std::map<std::string, std::string>& headers, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "DELETE", uri, "", headers, timeout);
}

UniqueResource manageNacm(sysrepo::Session session);
void setupRealNacm(sysrepo::Session session);

struct SSEClient {
    std::shared_ptr<ng_client::session> client;
    boost::asio::deadline_timer t;

    SSEClient(
        boost::asio::io_service& io,
        std::latch& requestSent,
        const NotificationWatcher& notification,
        const std::string& uri,
        const std::map<std::string, std::string>& headers,
        const boost::posix_time::seconds silenceTimeout = boost::posix_time::seconds(1)); // test code; the server should respond "soon"

    static std::vector<std::string> parseEvents(const std::string& msg);
};

#define PREPARE_LOOP_WITH_EXCEPTIONS \
    boost::asio::io_service io; \
    std::promise<void> bg; \
    std::latch requestSent(1);

#define RUN_LOOP_WITH_EXCEPTIONS \
    do { \
        io.run(); \
        auto fut = bg.get_future(); \
        REQUIRE(fut.wait_for(666ms /* "plenty of time" for the notificationThread to exit after it has called io.stop() */) == std::future_status::ready); \
        fut.get(); \
    } while (false)

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
