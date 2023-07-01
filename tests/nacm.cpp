/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <iostream>
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Enum.hpp>
#include <sysrepo-cpp/utils/utils.hpp>
#include "restconf/Server.h"

using namespace std::chrono_literals;

std::map<std::string, std::string> retrieveDataAsUser(sysrepo::Session sess, const std::string& path, const std::string& user)
{
    auto data = rousette::restconf::getData(sess, path, user);
    if (!data) {
        return {};
    }

    std::map<std::string, std::string> res;
    for (const auto& n : data->childrenDfs()) {
        if (n.isTerm()) {
            res.emplace(n.path(), n.asTerm().valueStr());
            std::cout << n.path() << " -> " << n.asTerm().valueStr() << std::endl;
        }
    }
    return res;
}

TEST_CASE("NACM user")
{
    sysrepo::setLogLevelStderr(sysrepo::LogLevel::Information);
    auto cliConn = sysrepo::Connection{};
    auto cliSess = cliConn.sessionStart(sysrepo::Datastore::Running);
    auto cliSubs = cliSess.initNacm();

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);

    srSess.setItem("/ietf-netconf-acm:nacm/enable-external-groups", "false");
    srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='blah']/user-name[.='a']", "");
    srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='optics']/user-name[.='dwdm']", "");
    srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='optics']/user-name[.='dwdm2']", "");

    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='optics can access l2 and l3']/group[.='optics']", "");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='optics can access l2 and l3']/rule[name='1']/module-name", "nacm-test-a");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='optics can access l2 and l3']/rule[name='1']/action", "permit");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='optics can access l2 and l3']/rule[name='1']/access-operations", "read");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='optics can access l2 and l3']/rule[name='1']/path", "/nacm-test-a:cont/l2");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='optics can access l2 and l3']/rule[name='2']/module-name", "nacm-test-a");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='optics can access l2 and l3']/rule[name='2']/action", "permit");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='optics can access l2 and l3']/rule[name='2']/access-operations", "read");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='optics can access l2 and l3']/rule[name='2']/path", "/nacm-test-a:cont/l3");

    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='Nobody can access cont']/group[.='*']", "");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='Nobody can access cont']/rule[name='1']/module-name", "nacm-test-a");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='Nobody can access cont']/rule[name='1']/action", "deny");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='Nobody can access cont']/rule[name='1']/access-operations", "*");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='Nobody can access cont']/rule[name='1']/path", "/");

    srSess.applyChanges();

    REQUIRE(retrieveDataAsUser(cliSess, "nacm-test-a:cont", "a").empty());
    REQUIRE(retrieveDataAsUser(cliSess, "nacm-test-a:cont/l1", "a").empty());
    REQUIRE(retrieveDataAsUser(cliSess, "nacm-test-a:cont/l1", "dwdm").empty());
    REQUIRE(retrieveDataAsUser(cliSess, "nacm-test-a:cont/l1", "dwdm2").empty());
    REQUIRE(retrieveDataAsUser(cliSess, "nacm-test-a:cont", "dwdm") == std::map<std::string, std::string>{
                {"/nacm-test-a:cont/l2", "2"},
                {"/nacm-test-a:cont/l3", "3"},
            });

    srSess.deleteItem("/ietf-netconf-acm:nacm/rule-list");
    srSess.applyChanges();
}
