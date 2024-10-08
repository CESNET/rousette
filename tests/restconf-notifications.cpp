/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
static const auto SERVER_PORT = "10088";
#include <latch>
#include <libyang-cpp/Time.hpp>
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/utils/utils.hpp>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/pretty_printers.h"

#define EXPECT_NOTIFICATION(DATA, SEQ) expectations.emplace_back(NAMED_REQUIRE_CALL(netconfWatcher, data(DATA)).IN_SEQUENCE(SEQ));
#define SEND_NOTIFICATION(DATA) notifSession.sendNotification(*ctx.parseOp(DATA, libyang::DataFormat::JSON, libyang::OperationType::NotificationYang).op, sysrepo::Wait::No);

using namespace std::chrono_literals;

struct NotificationWatcher {
    libyang::Context ctx;
    libyang::DataFormat dataFormat;

    NotificationWatcher(const libyang::Context& ctx)
        : ctx(ctx)
        , dataFormat(libyang::DataFormat::JSON)
    {
    }

    void setDataFormat(const libyang::DataFormat dataFormat)
    {
        this->dataFormat = dataFormat;
    }

    void operator()(const std::string& msg) const
    {
        spdlog::trace("Client received data: {}", msg);
        auto notifDataNode = ctx.parseOp(msg,
                                         dataFormat,
                                         dataFormat == libyang::DataFormat::JSON ? libyang::OperationType::NotificationRestconf : libyang::OperationType::NotificationNetconf);

        // parsing nested notifications does not return the data tree root node but the notification data node
        auto dataRoot = notifDataNode.op;
        while (dataRoot->parent()) {
            dataRoot = *dataRoot->parent();
        }

        data(*dataRoot->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::Shrink));
    }

    MAKE_CONST_MOCK1(data, void(const std::string&));
};

struct SSEClient {
    std::shared_ptr<ng_client::session> client;
    boost::asio::deadline_timer t;

    SSEClient(
        boost::asio::io_service& io,
        std::latch& requestSent,
        const NotificationWatcher& notification,
        const std::string& uri,
        const std::map<std::string, std::string>& headers,
        const boost::posix_time::seconds silenceTimeout = boost::posix_time::seconds(1)) // test code; the server should respond "soon"
        : client(std::make_shared<ng_client::session>(io, SERVER_ADDRESS, SERVER_PORT))
        , t(io, silenceTimeout)
    {
        ng::header_map reqHeaders;
        for (const auto& [name, value] : headers) {
            reqHeaders.insert({name, {value, false}});
        }

        // shutdown the client after a period of no traffic
        t.async_wait([maybeClient = std::weak_ptr<ng_client::session>{client}](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted) {
                return;
            }
            if (auto client = maybeClient.lock()) {
                client->shutdown();
            }
        });

        client->on_connect([&, uri, reqHeaders, silenceTimeout](auto) {
            boost::system::error_code ec;

            auto req = client->submit(ec, "GET", SERVER_ADDRESS_AND_PORT + uri, "", reqHeaders);
            req->on_response([&, silenceTimeout](const ng_client::response& res) {
                requestSent.count_down();
                res.on_data([&, silenceTimeout](const uint8_t* data, std::size_t len) {
                    // not a production-ready code. In real-life condition the data received in one callback might probably be incomplete
                    for (const auto& event : parseEvents(std::string(reinterpret_cast<const char*>(data), len))) {
                        notification(event);
                    }
                    t.expires_from_now(silenceTimeout);
                });
            });
        });

        client->on_error([&](const boost::system::error_code& ec) {
            throw std::runtime_error{"HTTP client error: " + ec.message()};
        });
    }

    static std::vector<std::string> parseEvents(const std::string& msg)
    {
        static const std::string prefix = "data:";

        std::vector<std::string> res;
        std::istringstream iss(msg);
        std::string line;
        std::string event;

        while (std::getline(iss, line)) {
            if (line.compare(0, prefix.size(), prefix) == 0) {
                event += line.substr(prefix.size());
            } else if (line.empty()) {
                res.emplace_back(std::move(event));
                event.clear();
            } else {
                FAIL("Unprefixed response");
            }
        }
        return res;
    }
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

auto wrap_exceptions_and_asio(std::promise<void>& bg, boost::asio::io_service& io, std::function<void()> func)
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

TEST_CASE("NETCONF notification streams")
{
    trompeloeil::sequence seqMod1, seqMod2;
    sysrepo::setLogLevelStderr(sysrepo::LogLevel::Information);
    spdlog::set_level(spdlog::level::trace);

    std::vector<std::unique_ptr<trompeloeil::expectation>> expectations;

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};
    setupRealNacm(srSess);

    // parent for nested notification
    srSess.switchDatastore(sysrepo::Datastore::Operational);
    srSess.setItem("/example:tlc/list[name='k1']/choice1", "something must me here");
    srSess.applyChanges();

    std::vector<std::string> notificationsJSON{
        R"({"example:eventA":{"message":"blabla","progress":11}})",
        R"({"example:eventB":{}})",
        R"({"example-notif:something-happened":{}})",
        R"({"example:eventA":{"message":"almost finished","progress":99}})",
        R"({"example:tlc":{"list":[{"name":"k1","notif":{"message":"nested"}}]}})",
    };

    NotificationWatcher netconfWatcher(srConn.sessionStart().getContext());

    SECTION("NETCONF streams")
    {
        std::string uri;
        std::map<std::string, std::string> headers;

        SECTION("XML stream")
        {
            netconfWatcher.setDataFormat(libyang::DataFormat::XML);
            headers = {AUTH_ROOT};

            SECTION("No filter")
            {
                uri = "/streams/NETCONF/XML";
                EXPECT_NOTIFICATION(notificationsJSON[0], seqMod1);
                EXPECT_NOTIFICATION(notificationsJSON[1], seqMod1);
                EXPECT_NOTIFICATION(notificationsJSON[2], seqMod2);
                EXPECT_NOTIFICATION(notificationsJSON[3], seqMod1);
                EXPECT_NOTIFICATION(notificationsJSON[4], seqMod1);
            }
            SECTION("Filter")
            {
                uri = "/streams/NETCONF/XML?filter=/example:eventA";
                EXPECT_NOTIFICATION(notificationsJSON[0], seqMod1);
                EXPECT_NOTIFICATION(notificationsJSON[3], seqMod1);
            }
        }

        SECTION("JSON stream")
        {
            uri = "/streams/NETCONF/JSON";

            SECTION("anonymous user cannot read example-notif module")
            {
                EXPECT_NOTIFICATION(notificationsJSON[0], seqMod1);
                EXPECT_NOTIFICATION(notificationsJSON[1], seqMod1);
                EXPECT_NOTIFICATION(notificationsJSON[3], seqMod1);
                EXPECT_NOTIFICATION(notificationsJSON[4], seqMod1);
            }

            SECTION("root user")
            {
                headers = {AUTH_ROOT};
                EXPECT_NOTIFICATION(notificationsJSON[0], seqMod1);
                EXPECT_NOTIFICATION(notificationsJSON[1], seqMod1);
                EXPECT_NOTIFICATION(notificationsJSON[2], seqMod2);
                EXPECT_NOTIFICATION(notificationsJSON[3], seqMod1);
                EXPECT_NOTIFICATION(notificationsJSON[4], seqMod1);
            }
        }

        PREPARE_LOOP_WITH_EXCEPTIONS

        // Here's how these two threads work together.
        //
        // The main test thread (this one):
        // - sets up all the expectations
        // - has an HTTP client which calls/spends the expectations based on the incoming SSE data
        // - blocks while it runs the ASIO event loop
        //
        // The auxiliary thread (the notificationThread):
        // - waits for the HTTP client having issued its long-lived HTTP GET
        // - sends a bunch of notifications to sysrepo
        // - waits for all the expectations getting spent, and then terminates the ASIO event loop cleanly

        std::jthread notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            auto notifSession = sysrepo::Connection{}.sessionStart();
            auto ctx = notifSession.getContext();

            // wait until the client sends its HTTP request
            requestSent.wait();

            SEND_NOTIFICATION(notificationsJSON[0]);
            SEND_NOTIFICATION(notificationsJSON[1]);
            std::this_thread::sleep_for(500ms); // simulate some delays; server might be slow in creating notifications, client should still remain connected
            SEND_NOTIFICATION(notificationsJSON[2]);
            std::this_thread::sleep_for(500ms);
            SEND_NOTIFICATION(notificationsJSON[3]);
            SEND_NOTIFICATION(notificationsJSON[4]);

            // once the main thread has processed all the notifications, stop the ASIO loop
            waitForCompletionAndBitMore(seqMod1);
            waitForCompletionAndBitMore(seqMod2);
        }));

        SSEClient cli(io, requestSent, netconfWatcher, uri, headers);
        RUN_LOOP_WITH_EXCEPTIONS;
    }

    SECTION("Other methods")
    {
        REQUIRE(clientRequest("HEAD", "/streams/NETCONF/XML", "", {AUTH_ROOT}) == Response{200, eventStreamHeaders, ""});
        REQUIRE(clientRequest("OPTIONS", "/streams/NETCONF/XML", "", {AUTH_ROOT}) == Response{200, Response::Headers{ACCESS_CONTROL_ALLOW_ORIGIN, {"allow", "GET, HEAD, OPTIONS"}}, ""});

        const std::multimap<std::string, std::string> headers = {
            {"access-control-allow-origin", "*"},
            {"allow", "GET, HEAD, OPTIONS"},
            {"content-type", "text/plain"},
        };
        REQUIRE(clientRequest("PUT", "/streams/NETCONF/XML", "", {AUTH_ROOT}) == Response{405, headers, "Method not allowed."});
        REQUIRE(clientRequest("POST", "/streams/NETCONF/XML", "", {AUTH_ROOT}) == Response{405, headers, "Method not allowed."});
        REQUIRE(clientRequest("PATCH", "/streams/NETCONF/XML", "", {AUTH_ROOT}) == Response{405, headers, "Method not allowed."});
        REQUIRE(clientRequest("DELETE", "/streams/NETCONF/XML", "", {AUTH_ROOT}) == Response{405, headers, "Method not allowed."});
    }

    SECTION("Invalid URLs")
    {
        REQUIRE(get("/streams/NETCONF/", {}) == Response{404, plaintextHeaders, "Invalid stream"});
        REQUIRE(get("/streams/NETCONF/", {AUTH_ROOT}) == Response{404, plaintextHeaders, "Invalid stream"});
        REQUIRE(get("/streams/NETCONF/bla", {}) == Response{404, plaintextHeaders, "Invalid stream"});
    }

    SECTION("Invalid parameters")
    {
        REQUIRE(get("/streams/NETCONF/XML?filter=.878", {}) == Response{400, plaintextHeaders, "Couldn't create notification subscription: SR_ERR_INVAL_ARG\n XPath \".878\" does not select any notifications. (SR_ERR_INVAL_ARG)"});
        REQUIRE(get("/streams/NETCONF/XML?filter=", {}) == Response{400, plaintextHeaders, "Query parameters syntax error"});

        REQUIRE(get("/streams/NETCONF/XML?start-time=2000-01-01T00:00:00+00:00&stop-time=1990-01-01T00:00:00+00:00", {}) == Response{400, plaintextHeaders, "stop-time must be greater than start-time"});
        REQUIRE(get("/streams/NETCONF/XML?stop-time=1990-01-01T00:00:00+00:00", {}) == Response{400, plaintextHeaders, "stop-time must be used with start-time"});
        REQUIRE(get("/streams/NETCONF/XML?start-time=" + libyang::yangTimeFormat(std::chrono::system_clock::now() + std::chrono::hours(1), libyang::TimezoneInterpretation::Local), {}) == Response{400, plaintextHeaders, "start-time is in the future"});
    }

    SECTION("Replays")
    {
        // no replays so sending a notification does not trigger replay-* leafs
        srSess.sendNotification(*srSess.getContext().parseOp(R"({"example:eventB": {}})", libyang::DataFormat::JSON, libyang::OperationType::NotificationYang).op, sysrepo::Wait::No);
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-restconf-monitoring:restconf-state/streams/stream=NETCONF/replay-support", {AUTH_ROOT, FORWARDED}).statusCode == 404);
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-restconf-monitoring:restconf-state/streams/stream=NETCONF/replay-log-creation-time", {AUTH_ROOT, FORWARDED}).statusCode == 404);

        // announce replay support
        srConn.setModuleReplaySupport("example", true);
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-restconf-monitoring:restconf-state/streams/stream=NETCONF/replay-support", {AUTH_ROOT, FORWARDED}) == Response{200, jsonHeaders, R"({
  "ietf-restconf-monitoring:restconf-state": {
    "streams": {
      "stream": [
        {
          "name": "NETCONF",
          "replay-support": true
        }
      ]
    }
  }
}
)"});

        // sending a notification with replay support on means that the timestamp leaf appears
        srSess.sendNotification(*srSess.getContext().parseOp(R"({"example:eventB": {}})", libyang::DataFormat::JSON, libyang::OperationType::NotificationYang).op, sysrepo::Wait::No);
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-restconf-monitoring:restconf-state/streams/stream=NETCONF/replay-log-creation-time", {AUTH_ROOT, FORWARDED}).statusCode == 200);

        // no more replays
        srConn.setModuleReplaySupport("example", false);
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-restconf-monitoring:restconf-state/streams/stream=NETCONF/replay-support", {AUTH_ROOT, FORWARDED}).statusCode == 404);
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-restconf-monitoring:restconf-state/streams/stream=NETCONF/replay-log-creation-time", {AUTH_ROOT, FORWARDED}).statusCode == 404);
    }

    SECTION("Replay support")
    {
        srConn.setModuleReplaySupport("example", true);
        srConn.setModuleReplaySupport("example-notif", true);

        std::string uri = "/streams/NETCONF/XML";
        netconfWatcher.setDataFormat(libyang::DataFormat::XML);

        PREPARE_LOOP_WITH_EXCEPTIONS
        std::jthread notificationThread;
        std::latch oldNotificationsDone{1};

        SECTION("Start time")
        {
            EXPECT_NOTIFICATION(notificationsJSON[0], seqMod1);
            EXPECT_NOTIFICATION(notificationsJSON[1], seqMod1);
            EXPECT_NOTIFICATION(notificationsJSON[2], seqMod2);
            EXPECT_NOTIFICATION(notificationsJSON[3], seqMod1);
            EXPECT_NOTIFICATION(notificationsJSON[4], seqMod1);

            uri += "?start-time=" + libyang::yangTimeFormat(std::chrono::system_clock::now(), libyang::TimezoneInterpretation::Local);

            SECTION("All notifications before client connects")
            {
                notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
                    auto notifSession = sysrepo::Connection{}.sessionStart();
                    auto ctx = notifSession.getContext();

                    SEND_NOTIFICATION(notificationsJSON[0]);
                    SEND_NOTIFICATION(notificationsJSON[1]);
                    SEND_NOTIFICATION(notificationsJSON[2]);
                    SEND_NOTIFICATION(notificationsJSON[3]);
                    SEND_NOTIFICATION(notificationsJSON[4]);
                    oldNotificationsDone.count_down();
                    requestSent.wait();

                    waitForCompletionAndBitMore(seqMod1);
                    waitForCompletionAndBitMore(seqMod2);
                }));
            }

            SECTION("Some notifications before client connects")
            {
                notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
                    auto notifSession = sysrepo::Connection{}.sessionStart();
                    auto ctx = notifSession.getContext();

                    SEND_NOTIFICATION(notificationsJSON[0]);
                    SEND_NOTIFICATION(notificationsJSON[1]);

                    oldNotificationsDone.count_down();
                    requestSent.wait();

                    SEND_NOTIFICATION(notificationsJSON[2]);
                    SEND_NOTIFICATION(notificationsJSON[3]);
                    SEND_NOTIFICATION(notificationsJSON[4]);

                    waitForCompletionAndBitMore(seqMod1);
                    waitForCompletionAndBitMore(seqMod2);
                }));
            }
        }

        SECTION("Start and stop time")
        {
            auto notifSession = sysrepo::Connection{}.sessionStart();
            auto ctx = notifSession.getContext();

            EXPECT_NOTIFICATION(notificationsJSON[2], seqMod2);

            SEND_NOTIFICATION(notificationsJSON[0]);
            SEND_NOTIFICATION(notificationsJSON[1]);

            auto start = std::chrono::system_clock::now();
            SEND_NOTIFICATION(notificationsJSON[2]);
            auto end = std::chrono::system_clock::now();
            uri += "?start-time=" + libyang::yangTimeFormat(start, libyang::TimezoneInterpretation::Local) + "&stop-time=" + libyang::yangTimeFormat(end, libyang::TimezoneInterpretation::Local);

            notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
                auto notifSession = sysrepo::Connection{}.sessionStart();
                auto ctx = notifSession.getContext();
                SEND_NOTIFICATION(notificationsJSON[3]);
                SEND_NOTIFICATION(notificationsJSON[4]);

                oldNotificationsDone.count_down();
                requestSent.wait();
                waitForCompletionAndBitMore(seqMod1);
                waitForCompletionAndBitMore(seqMod2);
            }));
        }

        oldNotificationsDone.wait();
        SSEClient cli(io, requestSent, netconfWatcher, uri, {AUTH_ROOT});
        RUN_LOOP_WITH_EXCEPTIONS;
    }
}
