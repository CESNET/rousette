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
        {"/restconf/data/foo:bar/Y=val", "foo:bar/Y=val"},
        {"/restconf/data/foo:bar/Y=val-ue", "foo:bar/Y=val-ue"},
        {"/restconf/data/foo:bar/X=Y=instance-value", {}},
        {"/restconf/data/foo:bar/lst=key1", "foo:bar/lst=key1"},
        {"/restconf/data/foo:bar/lst=key1/leaf", "foo:bar/lst=key1/leaf"},
        {"/restconf/data/foo:bar/lst=key1,", "foo:bar/lst=key1,"},
        {"/restconf/data/foo:bar/lst=key1,,,,", "foo:bar/lst=key1,,,,"},
        {"/restconf/data/foo:bar/lst=key1,,,,=", {}},
        {"/restconf/data/foo:bar/lst=key1,/leaf", "foo:bar/lst=key1,/leaf"},
        {"/restconf/data/foo:bar/lst=key1,key2", "foo:bar/lst=key1,key2"},
        {"/restconf/data/foo:bar/lst=key1,key2/leaf", "foo:bar/lst=key1,key2/leaf"},
        {"/restconf/data/foo:bar/lst=key1,key2/lst2=key1/leaf", "foo:bar/lst=key1,key2/lst2=key1/leaf"},
        {"/restconf/data/foo:bar/lst=", {}},
        {"/restconf/data/foo:bar/prefix:lst=key1/prefix:leaf", "foo:bar/prefix:lst=key1/prefix:leaf"},
        {"/restconf/data/foo:bar/lst==", {}},
        {"/restconf/data/foo:bar/lst==key", {}},
        {"/restconf/data/foo:bar/=key", {}},
        {"/restconf/data/foo:bar/lst=key1,,key3", "foo:bar/lst=key1,,key3"},
        {"/restconf/data/foo:bar/lst=key%2CWithAComma,,key3", "foo:bar/lst=key%2CWithAComma,,key3"},
        {R"(/restconf/data/foo:bar/list1=%2C%27"%3A"%20%2F,,foo)", R"(foo:bar/list1=%2C%27"%3A"%20%2F,,foo)"},
    };
    for (const auto& x : data) {
        const auto& input = x.first;
        const auto& xpath = x.second;
        CAPTURE(input);
        CAPTURE(xpath);
        REQUIRE(rousette::restconf::as_subtree_path(input) == xpath);
    }
}
