/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <spdlog/spdlog.h>
#include "Nacm.h"
#include "NacmIdentities.h"

namespace {
bool isRuleReadOnly(const libyang::DataNode& rule)
{
    auto accessOperations = rule.findXPath("access-operations");
    return !accessOperations.empty() && std::all_of(accessOperations.begin(), accessOperations.end(), [](const auto& e) {
        return e.asTerm().valueStr() == "read";
    });
}

bool isRuleWildcardDeny(const libyang::DataNode& rule)
{
    return rule.findPath("action")->asTerm().valueStr() == "deny" && rule.findPath("module-name")->asTerm().valueStr() == "*" && rule.findPath("access-operations")->asTerm().valueStr() == "*";
}

/**
 * Validates that NACM rules for anonymous user access are set according to this policy:
 *
 * The first rule-list element contains rules for anonymous user access, i.e.:
 *  - The group is set to @p anonGroup (this one should contain the anonymous user)
 *  - In rules (except the last one) the access-operation allowed is "read"
 *  - The last rule has module-name="*" and action "deny".
 *
 *  @return boolean indicating whether the rules are configured properly for anonymous user access
 */
bool validAnonymousNacmRules(sysrepo::Session session, const std::string& anonGroup)
{
    auto data = session.getData("/ietf-netconf-acm:nacm");
    if (!data) {
        spdlog::debug("NACM config validation: no data");
        return false;
    }

    auto ruleLists = data->findXPath("/ietf-netconf-acm:nacm/rule-list");
    if (ruleLists.empty()) {
        spdlog::debug("NACM config validation: no rule-list entries");
        return false;
    }

    auto firstRuleSet = ruleLists.front();
    auto rules = firstRuleSet.findXPath("rule");
    auto groups = firstRuleSet.findXPath("group");

    if (!std::any_of(groups.begin(), groups.end(), [&](const auto& e) {
            return e.asTerm().valueStr() == anonGroup;
        })) {
        spdlog::debug("NACM config validation: First rule list doesn't contain anonymous access user's group");
        return false;
    }

    if (rules.empty()) {
        spdlog::debug("NACM config validation: First rule list doesn't contain any rules");
        return false;
    }

    if (!std::all_of(rules.begin(), rules.end() - 1, isRuleReadOnly)) {
        spdlog::debug("NACM config validation: First n-1 rules in the anonymous rule-list must be configured for read-access only");
        return false;
    }

    if (!isRuleWildcardDeny(rules.back())) {
        spdlog::debug("NACM config validation: Last rule in the anonymous rule-list must be configured to deny all access to all modules");
        return false;
    }

    return true;
}

}

namespace rousette::auth {

Nacm::Nacm(sysrepo::Connection conn)
    : m_srSession(conn.sessionStart(sysrepo::Datastore::Running))
    , m_srSub(m_srSession.initNacm())
    , m_anonymousEnabled{false}
{
    m_srSub.onModuleChange(
        "ietf-netconf-acm", [&](auto session, auto, auto, auto, auto, auto) {
            m_anonymousEnabled = validAnonymousNacmRules(session, ANONYMOUS_USER_GROUP);
            spdlog::info("NACM config validation: Anonymous user access {}", m_anonymousEnabled ? "enabled" : "disabled");
            return sysrepo::ErrorCode::Ok;
        },
        std::nullopt,
        0,
        sysrepo::SubscribeOptions::Enabled | sysrepo::SubscribeOptions::DoneOnly | sysrepo::SubscribeOptions::Passive);
}

/** @brief Tries to set @p user as NACM user in @p session. In case the user is the anonymous user we also check that anonymous access is enabled */
bool Nacm::authorize(sysrepo::Session session, const std::string& user) const
{
    if (user == ANONYMOUS_USER && !m_anonymousEnabled) {
        spdlog::trace("Anonymous access not configured");
        return false;
    }

    session.setNacmUser(user);
    spdlog::trace("Authenticated as user {}", user);
    return true;
}
}
