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

TEST_CASE("subtree paths") {
    std::string input;
    std::optional<std::string> xpath;
    SECTION("empty") {
    }
    SECTION("nothing") {
        input = "/restconf/data";
    }
    SECTION("namespace only") {
        input = "/restconf/data/foo";
    }
    SECTION("missing root item") {
        input = "/restconf/data/foo:";
    }
    SECTION("top-level wildcard") {
        input = "/restconf/data/foo:*";
        xpath = "foo:*";
    }
    SECTION("wildcard cannot be continued") {
        input = "/restconf/data/foo:*/bar";
    }
    SECTION("empty top-level namespace") {
        input = "/restconf/data/:bar";
    }
    SECTION("foo:bar") {
        input = "/restconf/data/foo:bar";
        xpath = "foo:bar";
    }
    SECTION("foo:bar/baz") {
        input = "/restconf/data/foo:bar/baz";
        xpath = "foo:bar/baz";
    }
    SECTION("foo:bar/meh:baz") {
        input = "/restconf/data/foo:bar/meh:baz";
        xpath = "foo:bar/meh:baz";
    }
    SECTION("foo:bar/yay/meh:baz") {
        input = "/restconf/data/foo:bar/yay/meh:baz";
        xpath = "foo:bar/yay/meh:baz";
    }
    SECTION("empty namespace") {
        input = "/restconf/data/foo:bar/:baz";
    }
    REQUIRE(rousette::restconf::as_subtree_path(input) == xpath);
}
