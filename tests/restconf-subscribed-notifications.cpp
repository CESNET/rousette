/*
 * Copyright (C) 2025 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <libyang-cpp/Time.hpp>
#include <nghttp2/asio_http2.h>
#include <regex>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/utils/utils.hpp>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/event_watchers.h"
#include "tests/pretty_printers.h"

#define SEND_NOTIFICATION(DATA) notifSession.sendNotification(*ctx.parseOp(DATA, libyang::DataFormat::JSON, libyang::OperationType::NotificationYang).op, sysrepo::Wait::No);
#define REPLAY_COMPLETED trompeloeil::re(R"(^\{"ietf-subscribed-notifications:replay-completed":\{"id":[0-9]+\}\}$)")
constexpr auto uuidV4Regex = "[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-4[a-fA-F0-9]{3}-[89abAB][a-fA-F0-9]{3}-[a-fA-F0-9]{12}";

using namespace std::chrono_literals;
using namespace std::string_literals;

std::string datastoreToString(sysrepo::Datastore ds)
{
    switch (ds) {
    case sysrepo::Datastore::Startup:
        return "ietf-datastores:startup";
    case sysrepo::Datastore::Running:
        return "ietf-datastores:running";
    case sysrepo::Datastore::Candidate:
        return "ietf-datastores:candidate";
    case sysrepo::Datastore::Operational:
        return "ietf-datastores:operational";
    case sysrepo::Datastore::FactoryDefault:
        return "ietf-datastores:factory-default";
    default:
        FAIL("Unhandled sysrepo::Datastore");
        __builtin_unreachable(); // To make GCC 13.2.1 happy
    }
}

struct EstablishSubscriptionResult {
    uint32_t id;
    std::string url;
    std::optional<sysrepo::NotificationTimeStamp> replayStartTimeRevision;
};

struct FilterXPath {
    std::string xpath;
};
using Filter = std::variant<std::monostate, FilterXPath, libyang::XML>;

struct SubscribedNotifications {
    std::string stream;
    Filter filter;
    std::optional<sysrepo::NotificationTimeStamp> replayStartTime;
};

constexpr auto netconfSubscribedNotif = SubscribedNotifications{.stream = "NETCONF", .filter = {}, .replayStartTime = std::nullopt};

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
    const libyang::Context& ctx,
    const libyang::DataFormat rpcEncoding,
    const std::optional<std::pair<std::string, std::string>>& rpcRequestAuthHeader,
    const std::optional<std::string>& encodingLeafValue,
    const std::variant<SubscribedNotifications, YangPushOnChange, YangPushPeriodic>& params)
{
    constexpr auto jsonPrefix = "ietf-subscribed-notifications";
    constexpr auto xmlNamespace = "urn:ietf:params:xml:ns:yang:ietf-subscribed-notifications";

    auto stopTime = libyang::yangTimeFormat(std::chrono::system_clock::now() + 5s, libyang::TimezoneInterpretation::Local);
    std::map<std::string, std::string> requestHeaders;
    ng::header_map expectedHeaders;

    if (rpcRequestAuthHeader) {
        requestHeaders.insert(*rpcRequestAuthHeader);
    }

    // add the forwarded header
    static const auto FORWARD_PROTO = "http";
    static const auto FORWARD_HOST = "["s + SERVER_ADDRESS + "]:" + SERVER_PORT;
    requestHeaders.emplace("forward", "proto="s + FORWARD_PROTO + ";host="s + FORWARD_HOST);

    std::optional<libyang::DataNode> envelope;
    auto rpcTree = ctx.newPath("/ietf-subscribed-notifications:establish-subscription");
    rpcTree.newPath("stop-time", stopTime);

    if (encodingLeafValue) {
        rpcTree.newPath("encoding", *encodingLeafValue);
    }

    if (std::holds_alternative<SubscribedNotifications>(params)) {
        const auto& sn = std::get<SubscribedNotifications>(params);
        rpcTree.newPath("stream", sn.stream);

        if (std::holds_alternative<FilterXPath>(sn.filter)) {
            rpcTree.newPath("stream-xpath-filter", std::get<FilterXPath>(sn.filter).xpath);
        } else if (std::holds_alternative<libyang::XML>(sn.filter)) {
            rpcTree.newPath2("stream-subtree-filter", std::get<libyang::XML>(sn.filter));
        }

        if (sn.replayStartTime) {
            rpcTree.newPath("replay-start-time", libyang::yangTimeFormat(*sn.replayStartTime, libyang::TimezoneInterpretation::Local));
        }
    } else if (std::holds_alternative<YangPushOnChange>(params)) {
        const auto& yp = std::get<YangPushOnChange>(params);

        rpcTree.newPath("ietf-yang-push:datastore", datastoreToString(yp.datastore));
        rpcTree.newPath("ietf-yang-push:on-change", std::nullopt);

        if (std::holds_alternative<FilterXPath>(yp.filter)) {
            rpcTree.newPath("ietf-yang-push:datastore-xpath-filter", std::get<FilterXPath>(yp.filter).xpath);
        } else if (std::holds_alternative<libyang::XML>(yp.filter)) {
            rpcTree.newPath2("ietf-yang-push:datastore-subtree-filter", std::get<libyang::XML>(yp.filter));
        }

        if (yp.syncOnStart) {
            rpcTree.newPath("ietf-yang-push:on-change/sync-on-start", *yp.syncOnStart == sysrepo::SyncOnStart::Yes ? "true" : "false");
        }

        if (yp.dampeningPeriod) {
            const auto dampeningCentiseconds = std::chrono::duration_cast<std::chrono::duration<std::chrono::milliseconds::rep, std::centi>>(*yp.dampeningPeriod);
            rpcTree.newPath("ietf-yang-push:on-change/dampening-period", std::to_string(dampeningCentiseconds.count()));
        }

        for (const auto& changeType : yp.excludedChangeTypes) {
            rpcTree.newPath("ietf-yang-push:on-change/excluded-change[.='" + changeType + "']");
        }
    } else if (std::holds_alternative<YangPushPeriodic>(params)) {
        const auto& yp = std::get<YangPushPeriodic>(params);
        const auto periodCentiseconds = std::chrono::duration_cast<std::chrono::duration<std::chrono::milliseconds::rep, std::centi>>(yp.period);

        rpcTree.newPath("ietf-yang-push:datastore", datastoreToString(yp.datastore));
        rpcTree.newPath("ietf-yang-push:periodic/period", std::to_string(periodCentiseconds.count()));

        if (std::holds_alternative<FilterXPath>(yp.filter)) {
            rpcTree.newPath("ietf-yang-push:datastore-xpath-filter", std::get<FilterXPath>(yp.filter).xpath);
        } else if (std::holds_alternative<libyang::XML>(yp.filter)) {
            rpcTree.newPath2("ietf-yang-push:datastore-subtree-filter", std::get<libyang::XML>(yp.filter));
        }

        if (yp.anchorTime) {
            rpcTree.newPath("ietf-yang-push:periodic/period", libyang::yangTimeFormat(*yp.anchorTime, libyang::TimezoneInterpretation::Local));
        }
    }

    switch (rpcEncoding) {
    case libyang::DataFormat::JSON:
        requestHeaders.insert(CONTENT_TYPE_JSON);
        expectedHeaders = jsonHeaders;
        envelope = ctx.newOpaqueJSON({jsonPrefix, jsonPrefix, "input"}, std::nullopt);
        break;
    case libyang::DataFormat::XML:
        requestHeaders.insert(CONTENT_TYPE_XML);
        expectedHeaders = xmlHeaders;
        envelope = ctx.newOpaqueXML({xmlNamespace, jsonPrefix, "input"}, std::nullopt);
        break;
    default:
        FAIL("Unhandled libyang DataFormat");
        break;
    }

    // reconnect everything
    auto data = rpcTree.child();
    data->unlinkWithSiblings();
    envelope->insertChild(*data);

    auto body = *envelope->printStr(rpcEncoding, libyang::PrintFlags::Siblings);
    auto resp = post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription", requestHeaders, body);
    REQUIRE(resp.equalStatusCodeAndHeaders(Response{200, expectedHeaders, ""}));

    auto reply = ctx.newPath("/ietf-subscribed-notifications:establish-subscription");
    REQUIRE(reply.parseOp(resp.data, rpcEncoding, libyang::OperationType::ReplyRestconf).tree);

    auto idNode = reply.findPath("id", libyang::InputOutputNodes::Output);
    REQUIRE(idNode);

    auto urlNode = reply.findPath("ietf-restconf-subscribed-notifications:uri", libyang::InputOutputNodes::Output);
    REQUIRE(urlNode);

    // We are sending forwarded header with proto=FORWARD_PROTO and host=FORWARD_HOST
    // but the client expects only the URI path
    auto prefix = FORWARD_PROTO + "://"s + FORWARD_HOST;
    auto url = urlNode->asTerm().valueStr();
    REQUIRE(url.starts_with(prefix));
    url = url.erase(0, prefix.length());

    std::optional<sysrepo::NotificationTimeStamp> replayStartTimeRevision;
    if (auto node = reply.findPath("ietf-subscribed-notifications:replay-start-time-revision", libyang::InputOutputNodes::Output)) {
        replayStartTimeRevision = libyang::fromYangTimeFormat<sysrepo::NotificationTimeStamp::clock>(node->asTerm().valueStr());
    }

    return {
        std::get<uint32_t>(idNode->asTerm().value()),
        url,
        replayStartTimeRevision
    };
}

TEST_CASE("RESTCONF subscribed notifications")
{
    trompeloeil::sequence seq1, seq2;
    sysrepo::setLogLevelStderr(sysrepo::LogLevel::Information);
    spdlog::set_level(spdlog::level::trace);

    sysrepo::setGlobalContextOptions(sysrepo::ContextFlags::LibYangPrivParsed | sysrepo::ContextFlags::NoPrinted, sysrepo::GlobalContextEffect::Immediate);
    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};
    setupRealNacm(srSess);

    std::vector<std::unique_ptr<trompeloeil::expectation>> expectations;

    libyang::DataFormat rpcRequestEncoding = libyang::DataFormat::JSON;
    std::optional<std::string> rpcSubscriptionEncoding;
    std::optional<std::pair<std::string, std::string>> rpcRequestAuthHeader;

    SECTION("Stream list")
    {
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-subscribed-notifications:streams/stream=NETCONF", {AUTH_ROOT}) == Response{200, jsonHeaders, R"({
  "ietf-subscribed-notifications:streams": {
    "stream": [
      {
        "name": "NETCONF",
        "description": "Default NETCONF notification stream"
      }
    ]
  }
}
)"});
    }

    SECTION("NACM authorization")
    {
        SECTION("Anonymous access for establish-subscription is disabled")
        {
            /* Remove anonymous user's permission to execute RPCs in ietf-subscribed-notifications.
             * Intentionally a SECTION. We can remove NACM rule for a while in order to test access denied.
             * The rule gets automatically restored for the rest of the tests.
             */
            srSess.switchDatastore(sysrepo::Datastore::Running);
            srSess.deleteItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='16']");
            srSess.applyChanges();

            REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription", {FORWARDED, CONTENT_TYPE_JSON}, R"###({ "ietf-subscribed-notifications:input": { "stream": "NETCONF" } })###")
                    == Response{403, jsonHeaders, R"###({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "access-denied",
        "error-path": "/ietf-subscribed-notifications:establish-subscription",
        "error-message": "Access denied."
      }
    ]
  }
}
)###"});
        }

        SECTION("User DWDM establishes subscription")
        {
            std::pair<std::string, std::string> user = AUTH_DWDM;
            auto [id, uri, replayStartTimeRevision] = establishSubscription(srSess.getContext(), libyang::DataFormat::JSON, user, std::nullopt, netconfSubscribedNotif);
            std::map<std::string, std::string> headers;
            RestconfNotificationWatcher netconfWatcher(srConn.sessionStart().getContext());

            SECTION("Users who cannot GET")
            {
                SECTION("anonymous") { }
                SECTION("norules") { headers.insert(AUTH_NORULES); }
                REQUIRE(get(uri, headers) == Response{404, plaintextHeaders, "Subscription not found."});
            }

            SECTION("Users who can GET")
            {
                SECTION("root") { headers.insert(AUTH_ROOT); }
                SECTION("dwdm") { headers.insert(AUTH_DWDM); }
                REQUIRE(head(uri, headers) == Response{200, eventStreamHeaders, ""});
            }

            SECTION("GET on the same subscription concurently")
            {
                std::map<std::string, std::string> headers;
                std::string response;
                int status;

                SECTION("Allowed users")
                {
                    response = "There is already another GET request on this subscription.";
                    status = 409;

                    SECTION("Same user")
                    {
                        headers.insert(AUTH_DWDM);
                    }
                    SECTION("Different user")
                    {
                        headers.insert(AUTH_ROOT);
                    }
                }
                SECTION("Disallowed user")
                {
                    headers.insert(AUTH_NORULES);
                    response = "Subscription not found.";
                    status = 404;
                }

                PREPARE_LOOP_WITH_EXCEPTIONS
                auto thr = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
                    WAIT_UNTIL_SSE_CLIENT_REQUESTS;
                    REQUIRE(get(uri, headers) == Response{status, plaintextHeaders, response});
                }));

                SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, uri, {AUTH_DWDM});
                RUN_LOOP_WITH_EXCEPTIONS;
            }

            SECTION("GET on the same subscription sequentially")
            {
                boost::asio::io_service io;
                {
                    std::binary_semaphore requestSent(0); // SSEClient needs to notify when the request is sent, but we don't care about it here.
                    SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, uri, {AUTH_DWDM});
                }
                {
                    std::binary_semaphore requestSent(0);
                    SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, uri, {AUTH_DWDM});
                }
            }
        }
    }

    SECTION("Invalid establish-subscription requests")
    {
        // stop-time in the past
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription", {FORWARDED, CONTENT_TYPE_JSON}, R"###({ "ietf-subscribed-notifications:input": { "stream": "NETCONF", "stop-time": "1999-09-09T09:09:09Z" } })###")
                == Response{400, jsonHeaders, R"###({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-attribute",
        "error-message": "Couldn't create notification subscription: SR_ERR_INVAL_ARG\u000A Specified \"stop-time\" is in the past. (SR_ERR_INVAL_ARG)"
      }
    ]
  }
}
)###"});

        // invalid stream
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription", {FORWARDED, CONTENT_TYPE_JSON}, R"###({ "ietf-subscribed-notifications:input": { "stream": "ajsdhauisds" } })###")
                == Response{400, jsonHeaders, R"###({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-attribute",
        "error-message": "Couldn't create notification subscription: SR_ERR_NOT_FOUND\u000A Failed to collect modules to subscribe to, invalid stream and/or XPath filter (Item not found). (SR_ERR_NOT_FOUND)"
      }
    ]
  }
}
)###"});

        // stream-filter-name is unsupported, but leafref validation triggers first
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription", {FORWARDED, CONTENT_TYPE_JSON}, R"###({ "ietf-subscribed-notifications:input": { "stream": "NETCONF", "stream-filter-name": "xyz" } })###")
                == Response{400, jsonHeaders, R"###({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/ietf-subscribed-notifications:establish-subscription/stream-filter-name",
        "error-message": "Invalid leafref value \"xyz\" - no target instance \"/sn:filters/sn:stream-filter/sn:name\" with the same value."
      }
    ]
  }
}
)###"});

        srSess.switchDatastore(sysrepo::Datastore::Operational);
        srSess.setItem("/ietf-subscribed-notifications:filters/stream-filter[name='xyz']/stream-xpath-filter", "/example:eventA");
        srSess.applyChanges();
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription", {FORWARDED, CONTENT_TYPE_JSON}, R"###({ "ietf-subscribed-notifications:input": { "stream": "NETCONF", "stream-filter-name": "xyz" } })###")
                == Response{400, jsonHeaders, R"###({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-attribute",
        "error-message": "Stream filtering with predefined filters is not supported"
      }
    ]
  }
}
)###"});

        // replay-start-time > stop-time
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription",
                     {FORWARDED, CONTENT_TYPE_JSON},
                     R"###({ "ietf-subscribed-notifications:input": { "stream": "NETCONF", "replay-start-time": "2000-11-11T11:22:33Z", "stop-time": "2000-01-01T00:00:00Z" } })###")
                == Response{400, jsonHeaders, R"###({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-attribute",
        "error-message": "Couldn't create notification subscription: SR_ERR_INVAL_ARG\u000A Specified \"stop-time\" is earlier than \"start-time\". (SR_ERR_INVAL_ARG)"
      }
    ]
  }
}
)###"});

        // the forwarded header is missing here
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription",
                     {CONTENT_TYPE_JSON, AUTH_ROOT},
                     R"###({ "ietf-subscribed-notifications:input": { "stream": "NETCONF" } })###")
                == Response{400, jsonHeaders, R"###({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-value",
        "error-message": "Request scheme and host information is required to establish subscription."
      }
    ]
  }
}
)###"});
    }

    SECTION("Subscribed notifications requests")
    {
        RestconfNotificationWatcher netconfWatcher(srConn.sessionStart().getContext());

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
        const std::string notificationForReplayJSON = R"({"example:eventA":{"message":"this-should-be-sent-very-early","progress":0}})";

        std::pair<sysrepo::NotificationTimeStamp, sysrepo::NotificationTimeStamp> replayedNotificationSendInterval; // bounds for replayed notification event time
        bool shouldReviseStartTime = false;

        SubscribedNotifications subNotif;
        subNotif.stream = "NETCONF";

        SECTION("XML stream")
        {
            netconfWatcher.setDataFormat(libyang::DataFormat::XML);
            rpcRequestAuthHeader = AUTH_ROOT;

            SECTION("Stream encoding inferred from request content-type")
            {
                rpcRequestEncoding = libyang::DataFormat::XML;
            }

            SECTION("Explicitly asked for XML stream encoding")
            {
                rpcSubscriptionEncoding = "encode-xml";

                SECTION("Request content-type JSON")
                {
                    rpcRequestEncoding = libyang::DataFormat::JSON;
                }

                SECTION("Request content-type XML")
                {
                    rpcRequestEncoding = libyang::DataFormat::XML;
                }
            }

            EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
            EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
            EXPECT_NOTIFICATION(notificationsJSON[2], seq2);
            EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
            EXPECT_NOTIFICATION(notificationsJSON[4], seq1);
        }

        SECTION("JSON stream")
        {
            netconfWatcher.setDataFormat(libyang::DataFormat::JSON);

            SECTION("NACM: anonymous user cannot read example-notif module")
            {
                rpcRequestAuthHeader = std::nullopt;
                rpcRequestEncoding = libyang::DataFormat::JSON;
                rpcSubscriptionEncoding = "encode-json";

                EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[4], seq1);
            }

            SECTION("XPath filter set")
            {
                rpcRequestAuthHeader = AUTH_ROOT;
                rpcRequestEncoding = libyang::DataFormat::JSON;
                subNotif.filter = FilterXPath{"/example:eventA | /example:eventB"};
                rpcSubscriptionEncoding = "encode-json";
                EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
            }

            SECTION("Subtree filter set")
            {
                rpcRequestAuthHeader = AUTH_ROOT;
                rpcRequestEncoding = libyang::DataFormat::JSON;
                // Constructing the filter as XML is only an implementation detail. The tree is then constructed as JSON in establishSubscription
                subNotif.filter = libyang::XML{"<eventA xmlns='http://example.tld/example' />"};
                rpcSubscriptionEncoding = "encode-json";
                EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
            }

            SECTION("Replays")
            {
                // Announce replay support and send one notification before the client connects
                srConn.setModuleReplaySupport("example", true);

                {
                    auto notifSession = sysrepo::Connection{}.sessionStart();
                    auto ctx = notifSession.getContext();
                    replayedNotificationSendInterval.first = std::chrono::system_clock::now();
                    SEND_NOTIFICATION(notificationForReplayJSON);
                    replayedNotificationSendInterval.second = std::chrono::system_clock::now();
                }

                rpcRequestAuthHeader = AUTH_ROOT;
                rpcRequestEncoding = libyang::DataFormat::JSON;
                rpcSubscriptionEncoding = "encode-json";

                SECTION("replay-start-time-revision is announced to the client")
                {
                    subNotif.replayStartTime = std::chrono::system_clock::now() - 666s /* Reasonable time in the past, earlier than the replayed notification was sent */;

                    EXPECT_NOTIFICATION(notificationForReplayJSON, seq1);
                    EXPECT_NOTIFICATION(REPLAY_COMPLETED, seq1);
                    shouldReviseStartTime = true;
                }

                SECTION("replay-start-time-revision not announced")
                {
                    /* Ask for replay since the time when the first notification was sent. This should not revise the start time
                     * because we are not asking for the start *before* our history. (RFC 8639, 2.4.2.1) */
                    subNotif.replayStartTime = replayedNotificationSendInterval.second;

                    EXPECT_NOTIFICATION(REPLAY_COMPLETED, seq1);
                }

                EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[2], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[4], seq1);
            }

            SECTION("Content-type with set encode leaf")
            {
                rpcRequestAuthHeader = AUTH_ROOT;

                SECTION("Stream encoding inferred from request content-type")
                {
                    rpcRequestEncoding = libyang::DataFormat::JSON;
                }

                SECTION("Explicitly asked for JSON stream encoding")
                {
                    rpcSubscriptionEncoding = "encode-json";
                    SECTION("Request content-type JSON")
                    {
                        rpcRequestEncoding = libyang::DataFormat::JSON;
                    }

                    SECTION("Request content-type XML")
                    {
                        rpcRequestEncoding = libyang::DataFormat::XML;
                    }
                }

                EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[2], seq2);
                EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[4], seq1);
            }
        }

        auto [id, uri, replayStartTimeRevision] = establishSubscription(srSess.getContext(), rpcRequestEncoding, rpcRequestAuthHeader, rpcSubscriptionEncoding, subNotif);
        REQUIRE(std::regex_match(uri, std::regex("/streams/subscribed/"s + uuidV4Regex)));

        if (shouldReviseStartTime) {
            REQUIRE(replayStartTimeRevision);
            REQUIRE(replayedNotificationSendInterval.first <= *replayStartTimeRevision);
            REQUIRE(*replayStartTimeRevision <= replayedNotificationSendInterval.second);
        } else {
            REQUIRE(!replayStartTimeRevision);
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

            WAIT_UNTIL_SSE_CLIENT_REQUESTS;

            SEND_NOTIFICATION(notificationsJSON[0]);
            SEND_NOTIFICATION(notificationsJSON[1]);
            std::this_thread::sleep_for(500ms); // simulate some delays; server might be slow in creating notifications, client should still remain connected
            SEND_NOTIFICATION(notificationsJSON[2]);
            SEND_NOTIFICATION(notificationsJSON[3]);
            std::this_thread::sleep_for(500ms);
            SEND_NOTIFICATION(notificationsJSON[4]);

            // once the main thread has processed all the notifications, stop the ASIO loop
            waitForCompletionAndBitMore(seq1);
            waitForCompletionAndBitMore(seq2);
        }));

        std::map<std::string, std::string> streamHeaders;
        if (rpcRequestAuthHeader) {
            streamHeaders.insert(*rpcRequestAuthHeader);
        }
        SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, uri, streamHeaders);
        RUN_LOOP_WITH_EXCEPTIONS;
    }

    SECTION("delete-subscription")
    {
        std::optional<Response> expectedResponse;
        std::map<std::string, std::string> headers;
        headers.insert(CONTENT_TYPE_JSON);

        SECTION("Subscription created by dwdm")
        {
            rpcRequestAuthHeader = AUTH_DWDM;

            SECTION("dwdm (author) can delete")
            {
                headers.insert(AUTH_DWDM);
                expectedResponse = Response{204, noContentTypeHeaders, ""};
            }

            SECTION("Anonymous cannot delete anything because of NACM")
            {
                expectedResponse = Response{403, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "access-denied",
        "error-path": "/ietf-subscribed-notifications:delete-subscription",
        "error-message": "Access denied."
      }
    ]
  }
}
)"};
            }

            SECTION("norules user")
            {
                headers.insert(AUTH_NORULES);
                expectedResponse = Response{404, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-value",
        "error-path": "/ietf-subscribed-notifications:delete-subscription",
        "error-message": "Subscription not found."
      }
    ]
  }
}
)"};
            }

            SECTION("root user")
            {
                headers.insert(AUTH_ROOT);
                expectedResponse = Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-attribute",
        "error-path": "/ietf-subscribed-notifications:delete-subscription",
        "error-message": "Trying to delete subscription not created by root. Use kill-subscription instead."
      }
    ]
  }
}
)"};
            }

            auto [id, uri, replayStartTimeRevision] = establishSubscription(srSess.getContext(), rpcRequestEncoding, rpcRequestAuthHeader, rpcSubscriptionEncoding, netconfSubscribedNotif);
            auto body = R"({"ietf-subscribed-notifications:input": { "id": )" + std::to_string(id) + "}}";
            REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:delete-subscription", headers, body) == expectedResponse);
        }

        SECTION("Anonymous user cannot delete subscription craeted by anonymous user")
        {
            rpcRequestAuthHeader = std::nullopt;
            auto [id, uri, replayStartTimeRevision] = establishSubscription(srSess.getContext(), rpcRequestEncoding, rpcRequestAuthHeader, rpcSubscriptionEncoding, netconfSubscribedNotif);
            auto body = R"({"ietf-subscribed-notifications:input": { "id": )" + std::to_string(id) + "}}";
            REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:delete-subscription", headers, body) == Response{403, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "access-denied",
        "error-path": "/ietf-subscribed-notifications:delete-subscription",
        "error-message": "Access denied."
      }
    ]
  }
}
)"});
        }
    }

    SECTION("kill-subscription")
    {
        std::optional<Response> expectedResponse;
        std::map<std::string, std::string> headers;
        headers.insert(CONTENT_TYPE_JSON);

        SECTION("User cannot kill")
        {
            SECTION("dwdm (author)")
            {
                headers.insert(AUTH_DWDM);
            }

            SECTION("anonymous")
            {
            }

            expectedResponse = Response{403, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "access-denied",
        "error-path": "/ietf-subscribed-notifications:kill-subscription",
        "error-message": "Access denied."
      }
    ]
  }
}
)"};
        }

        SECTION("root")
        {
            headers.insert(AUTH_ROOT);
            expectedResponse = Response{204, noContentTypeHeaders, ""};
        }

        auto [id, uri, replayStartTimeRevision] = establishSubscription(srSess.getContext(), rpcRequestEncoding, rpcRequestAuthHeader, rpcSubscriptionEncoding, netconfSubscribedNotif);
        auto body = R"({"ietf-subscribed-notifications:input": { "id": )" + std::to_string(id) + "}}";
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:kill-subscription", headers, body) == expectedResponse);
    }

    SECTION("Invalid kill/delete-subscription requests")
    {
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:kill-subscription", {AUTH_ROOT, CONTENT_TYPE_JSON}, R"({"ietf-subscribed-notifications:input": {}})") == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/ietf-subscribed-notifications:kill-subscription",
        "error-message": "Mandatory node \"id\" instance does not exist."
      }
    ]
  }
}
)"});
    }

    SECTION("YANG push on change")
    {
        RestconfYangPushWatcher ypWatcher(srConn.sessionStart().getContext());

        YangPushOnChange yp;
        yp.datastore = sysrepo::Datastore::Running;
        yp.syncOnStart = sysrepo::SyncOnStart::No;

        SECTION("Basic test")
        {
            ypWatcher.setDataFormat(libyang::DataFormat::JSON);
            rpcRequestAuthHeader = AUTH_ROOT;
            rpcRequestEncoding = libyang::DataFormat::JSON;
            rpcSubscriptionEncoding = "encode-json";

            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"create","target":"/example:top-level-leaf","value":{"example:top-level-leaf":"42"}}]}}}})");
            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"replace","target":"/example:top-level-leaf","value":{"example:top-level-leaf":"44"}},{"edit-id":"edit-2","operation":"create","target":"/example:top-level-list[name='key1']","value":{"example:top-level-list":[{"name":"key1"}]}}]}}}})");
            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"create","target":"/example-delete:secret[name='bla']","value":{"example-delete:secret":[{"name":"bla"}]}}]}}}})");
            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"delete","target":"/example:top-level-leaf"}]}}}})");
        }

        SECTION("Only startup DS changes")
        {
            yp.datastore = sysrepo::Datastore::Startup;
            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"create","target":"/example:top-level-leaf","value":{"example:top-level-leaf":"43"}}]}}}})");
        }

        SECTION("NACM works")
        {
            rpcRequestAuthHeader = std::nullopt;
            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"create","target":"/example:top-level-leaf","value":{"example:top-level-leaf":"42"}}]}}}})");
            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"replace","target":"/example:top-level-leaf","value":{"example:top-level-leaf":"44"}},{"edit-id":"edit-2","operation":"create","target":"/example:top-level-list[name='key1']","value":{"example:top-level-list":[{"name":"key1"}]}}]}}}})");
            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"delete","target":"/example:top-level-leaf"}]}}}})");
        }

        SECTION("Filter")
        {
            SECTION("XPath filter")
            {
                yp.filter = FilterXPath{"/example:top-level-list"};
            }

            SECTION("Subtree filter is set")
            {
                yp.filter = libyang::XML{"<top-level-list xmlns='http://example.tld/example' />"};
            }

            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"create","target":"/example:top-level-list[name='key1']","value":{"example:top-level-list":[{"name":"key1"}]}}]}}}})");
        }

        SECTION("Excluded changes")
        {
            yp.excludedChangeTypes = {"delete", "insert", "create", "move"};

            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"replace","target":"/example:top-level-leaf","value":{"example:top-level-leaf":"44"}}]}}}})");
        }

        SECTION("Sync on start")
        {
            // push some data in advance
            srSess.switchDatastore(sysrepo::Datastore::Startup);
            srSess.setItem("/example:tlc/list[name='k1']/choice1", "choice1-startup");
            srSess.applyChanges();

            yp.datastore = sysrepo::Datastore::Startup;

            SECTION("Yes")
            {
                yp.syncOnStart = sysrepo::SyncOnStart::Yes;
                EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:tlc":{"list":[{"name":"k1","choice1":"choice1-startup"}]}}}})");
            }

            SECTION("No")
            {
                yp.syncOnStart = sysrepo::SyncOnStart::No;
            }

            EXPECT_YP_UPDATE(R"({"ietf-yang-push:push-change-update":{"datastore-changes":{"yang-patch":{"edit":[{"edit-id":"edit-1","operation":"create","target":"/example:top-level-leaf","value":{"example:top-level-leaf":"43"}}]}}}})");
        }

        auto uri = establishSubscription(srSess.getContext(), rpcRequestEncoding, rpcRequestAuthHeader, rpcSubscriptionEncoding, yp).url;

        // The thread cooperation is described in the subscribed notification subcase

        PREPARE_LOOP_WITH_EXCEPTIONS;
        std::jthread notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            auto sess = sysrepo::Connection{}.sessionStart();
            auto ctx = sess.getContext();

            WAIT_UNTIL_SSE_CLIENT_REQUESTS;

            sess.switchDatastore(sysrepo::Datastore::Running);
            sess.setItem("/example:top-level-leaf", "42");
            sess.applyChanges();

            sess.switchDatastore(sysrepo::Datastore::Startup);
            sess.setItem("/example:top-level-leaf", "43");
            sess.applyChanges();

            std::this_thread::sleep_for(400ms);

            sess.switchDatastore(sysrepo::Datastore::Running);
            sess.setItem("/example:top-level-leaf", "44");
            sess.setItem("/example:top-level-list[name='key1']", std::nullopt);
            sess.setItem("/example-delete:secret[name='bla']", std::nullopt);
            sess.applyChanges();

            std::this_thread::sleep_for(400ms);

            sess.deleteItem("/example:top-level-leaf");
            sess.applyChanges();

            // once the main thread has processed all the notifications, stop the ASIO loop
            waitForCompletionAndBitMore(seq1);
        }));

        std::map<std::string, std::string> streamHeaders;
        if (rpcRequestAuthHeader) {
            streamHeaders.insert(*rpcRequestAuthHeader);
        }
        SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, ypWatcher, uri, streamHeaders);
        RUN_LOOP_WITH_EXCEPTIONS;
    }

    SECTION("YANG push periodic")
    {
        RestconfYangPushWatcher ypWatcher(srConn.sessionStart().getContext());

        YangPushPeriodic yp;
        yp.period = 50ms;
        yp.datastore = sysrepo::Datastore::Startup; // I'm intentionally avoiding running and operational datastores; they contain a lot of data (for instance, config false stuff in operational and NACM rules in running)

        SECTION("Basic")
        {
            rpcRequestAuthHeader = AUTH_ROOT;

            ypWatcher.setDataFormat(libyang::DataFormat::JSON);
            rpcRequestEncoding = libyang::DataFormat::JSON;
            rpcSubscriptionEncoding = "encode-json";

            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{}}})");
            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"42"}}})");
            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{"example-delete:secret":[{"name":"bla"}]}}})");
        }

        SECTION("NACM works")
        {
            rpcRequestAuthHeader = std::nullopt;

            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{}}})");
            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"42"}}})");
            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{}}})");
        }

        SECTION("Filter")
        {
            SECTION("XPath filter")
            {
                yp.filter = FilterXPath{"/example:top-level-leaf"};
            }

            SECTION("Subtree filter is set")
            {
                yp.filter = libyang::XML{"<top-level-leaf xmlns='http://example.tld/example' />"};
            }

            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{}}})");
            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"42"}}})");
            EXPECT_YP_PERIODIC_UPDATE(R"({"ietf-yang-push:push-update":{"datastore-contents":{}}})");
        }

        auto uri = establishSubscription(srSess.getContext(), rpcRequestEncoding, rpcRequestAuthHeader, rpcSubscriptionEncoding, yp).url;

        // The thread cooperation is described in the subscribed notification subcase

        PREPARE_LOOP_WITH_EXCEPTIONS;
        std::jthread notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            auto sess = sysrepo::Connection{}.sessionStart();
            auto ctx = sess.getContext();

            WAIT_UNTIL_SSE_CLIENT_REQUESTS;

            std::this_thread::sleep_for(400ms);

            sess.switchDatastore(sysrepo::Datastore::Startup);
            sess.setItem("/example:top-level-leaf", "42");
            sess.applyChanges();

            std::this_thread::sleep_for(400ms);

            sess.switchDatastore(sysrepo::Datastore::Startup);
            sess.deleteItem("/example:top-level-leaf");
            sess.setItem("/example-delete:secret[name='bla']", std::nullopt);
            sess.applyChanges();

            // once the main thread has processed all the notifications, stop the ASIO loop
            waitForCompletionAndBitMore(seq1);
            waitForCompletionAndBitMore(seq2);
        }));

        std::map<std::string, std::string> streamHeaders;
        if (rpcRequestAuthHeader) {
            streamHeaders.insert(*rpcRequestAuthHeader);
        }
        SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, ypWatcher, uri, streamHeaders);
        RUN_LOOP_WITH_EXCEPTIONS;
    }
}

TEST_CASE("Terminating server under notification load")
{
    trompeloeil::sequence seq1;
    sysrepo::setLogLevelStderr(sysrepo::LogLevel::Information);
    spdlog::set_level(spdlog::level::trace);

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    auto nacmGuard = manageNacm(srSess);
    auto server = std::make_unique<rousette::restconf::Server>(srConn, SERVER_ADDRESS, SERVER_PORT);
    setupRealNacm(srSess);

    RestconfNotificationWatcher netconfWatcher(srConn.sessionStart().getContext());
    constexpr auto notif = R"({"example:eventB":{}})";

    std::optional<std::string> rpcSubscriptionEncoding;
    std::optional<std::pair<std::string, std::string>> rpcRequestAuthHeader;

    std::pair<std::string, std::string> auth = AUTH_ROOT;
    auto [id, uri, replayStartTimeRevision] = establishSubscription(srSess.getContext(), libyang::DataFormat::JSON, auth, std::nullopt, netconfSubscribedNotif);

    PREPARE_LOOP_WITH_EXCEPTIONS;

    std::atomic<bool> serverRunning = true;
    std::atomic<size_t> notificationsReceived = 0;
    constexpr size_t NOTIFICATIONS_BEFORE_TERMINATE = 50;

    auto notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
        auto notifSession = sysrepo::Connection{}.sessionStart();
        auto ctx = notifSession.getContext();

        WAIT_UNTIL_SSE_CLIENT_REQUESTS;

        while (serverRunning) {
            SEND_NOTIFICATION(notif);
        }
    }));

    auto serverShutdownThread = std::jthread([&]() {
        while (notificationsReceived <= NOTIFICATIONS_BEFORE_TERMINATE) {
            // A condition variable would be more elegant, but this is just a test...
            std::this_thread::sleep_for(20ms);
        }
        server.reset();
        serverRunning = false;
    });

    netconfWatcher.setDataFormat(libyang::DataFormat::JSON);
    REQUIRE_CALL(netconfWatcher, data(notif)).IN_SEQUENCE(seq1).TIMES(AT_LEAST(NOTIFICATIONS_BEFORE_TERMINATE)).LR_SIDE_EFFECT(notificationsReceived++);
    REQUIRE_CALL(netconfWatcher, data(R"({ietf-subscribed-notifications:subscription-terminated":{"id":1,"reason":"no-such-subscription"}})")).IN_SEQUENCE(seq1).TIMES(AT_MOST(1));
    SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, uri, {auth});
    RUN_LOOP_WITH_EXCEPTIONS;
}

TEST_CASE("Cleaning up inactive subscriptions")
{
    trompeloeil::sequence seq1, seq2;
    sysrepo::setLogLevelStderr(sysrepo::LogLevel::Information);
    spdlog::set_level(spdlog::level::trace);

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    RestconfNotificationWatcher netconfWatcher(srConn.sessionStart().getContext());

    auto nacmGuard = manageNacm(srSess);
    constexpr auto inactivityTimeout = 2s;
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT, 0ms, 55s, inactivityTimeout};

    auto [id, uri, replayStartTimeRevision] = establishSubscription(srSess.getContext(), libyang::DataFormat::JSON, {AUTH_ROOT}, std::nullopt, netconfSubscribedNotif);

    SECTION("Client connects and disconnects")
    {
        PREPARE_LOOP_WITH_EXCEPTIONS
        auto thr = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            WAIT_UNTIL_SSE_CLIENT_REQUESTS;
            std::this_thread::sleep_for(1s);
        }));

        SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, uri, {AUTH_ROOT});
        RUN_LOOP_WITH_EXCEPTIONS;
    }

    std::this_thread::sleep_for(inactivityTimeout + 1500ms);
    REQUIRE(get(uri, {AUTH_ROOT}) == Response{404, plaintextHeaders, "Subscription not found."});
}
