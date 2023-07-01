/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <spdlog/spdlog.h>
#include "nacm.h"


namespace {
bool isRuleReadOnly(const libyang::DataNode& rule)
{
    auto accessOperations = rule.findXPath("access-operations");
    return !accessOperations.empty() && std::all_of(accessOperations.begin(), accessOperations.end(), [](const auto& e) {
        return e.asTerm().valueStr() == "read";
    });
}

bool isRuleDenyForAll(const libyang::DataNode& rule)
{
    return rule.findPath("action")->asTerm().valueStr() == "deny" && rule.findPath("module-name")->asTerm().valueStr() == "*";
}
}

namespace rousette::restconf {

/**
 * Validates that NACM rules for anonymous user access are set according to this policy:
 *
 * The first rule-list element contains rules for anonymous user access, i.e.:
 *  - The group is set to TODO (this one should contain the anonymous user)
 *  - In rules (except the last one) the access-operation allowed is "read"
 *  - The last rule has module-name="*" and action "deny".
 *
 *  @return boolean indicating whether the rules are configured properly for anonymous user access
 */
bool validAnonymousNacmRules(sysrepo::Connection conn)
{
    auto sess = conn.sessionStart(sysrepo::Datastore::Running);

    auto data = sess.getData("/ietf-netconf-acm:nacm");
    if (!data) {
        return false;
    }

    auto ruleSets = data->findXPath("/ietf-netconf-acm:nacm/rule-list");
    if (ruleSets.size() == 0) {
        return false;
    }


    for (const auto& rs : ruleSets) {
        auto rules = rs.findXPath("rule");
        auto groups = rs.findXPath("group");

        if (!std::any_of(groups.begin(), groups.end(), [](const auto& e){
                    return e.asTerm().valueStr() == "anonymous";
                    })) {
            return false;
        }

        if (rules.empty()) {
            return false;
        }
        if (rules.size() > 0 && !(std::all_of(rules.begin(), rules.end() - 1, isRuleReadOnly) && isRuleDenyForAll(rules.back()))) {
            return false;
        }

        break; // we care about first one only
    }

    return true;
}
}
