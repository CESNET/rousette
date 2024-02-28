/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <sysrepo-cpp/Session.hpp>
#include <sysrepo-cpp/Subscription.hpp>

namespace rousette::restconf {

class YangSchemaLocations {
private:
    std::string m_urlPathPrefix;
    sysrepo::Session m_session;
    std::optional<sysrepo::Subscription> m_sub;

public:
    YangSchemaLocations(sysrepo::Connection conn, const std::string& urlPathPrefix);
};
}
