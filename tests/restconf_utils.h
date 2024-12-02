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
    boost::asio::deadline_timer t;

    SSEClient(
        boost::asio::io_service& io,
        const std::string& server_address,
        const std::string& server_port,
        std::latch& requestSent,
        const RestconfNotificationWatcher& notification,
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
