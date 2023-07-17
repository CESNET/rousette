/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include "trompeloeil_doctest.h"
#include "restconf/Server.h"

namespace std {
std::ostream& operator<<(std::ostream& s, const std::optional<std::string>& x) {
    if (!x) {
        return s << "nullopt{}";
    }
    return s << "optional{" << *x << "}";
}
}

TEST_CASE("subtree path validity") {
    auto data = std::initializer_list<std::pair<std::string, std::optional<std::string>>> {
        {{}, {}},
        {"/restconf/data", {}},
        {"/restconf/data/foo", {}},
        {"/restconf/data/foo:", {}},
        {"/restconf/data/foo:*", "foo:*"},
        {"/restconf/data/333:666", {}},
        {"/restconf/data/x333:y666", "x333:y666"},
        {"/restconf/data/foo:*/bar", {}},
        {"/restconf/data/:bar", {}},
        {"/restconf/data/foo:bar", "foo:bar"},
        {"/restconf/data/foo:bar/baz", "foo:bar/baz"},
        {"/restconf/data/foo:bar/meh:baz", "foo:bar/meh:baz"},
        {"/restconf/data/foo:bar/yay/meh:baz", "foo:bar/yay/meh:baz"},
        {"/restconf/data/foo:bar/:baz", {}},
    };
    for (const auto& x : data) {
        const auto& input = x.first;
        const auto& xpath = x.second;
        CAPTURE(input);
        CAPTURE(xpath);
        REQUIRE(rousette::restconf::as_subtree_path(input) == xpath);
    }
}

TEST_CASE("allowed paths for anonymous read") {

    auto allowed = {
        "czechlight-roadm-device:*",
        "czechlight-roadm-device:spectrum-scan",
        "czechlight-roadm-device:something/complex",
        "czechlight-system:firmware",
    };
    auto rejected = {
        "foo:*",
        "foo:bar",
        "unrelated:wtf/czechlight-roadm-device:something/complex",
        "czechlight-system:authentication",
        "czechlight-system:*",
        "ietf-netconf-server:*",
    };

    for (const auto path : allowed) {
        CAPTURE(path);
        REQUIRE(rousette::restconf::allow_anonymous_read_for(path));
    }

    for (const auto path : rejected) {
        CAPTURE(path);
        REQUIRE(!rousette::restconf::allow_anonymous_read_for(path));
    }
}
