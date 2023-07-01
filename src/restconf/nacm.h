/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <sysrepo-cpp/Connection.hpp>

namespace rousette::restconf {

bool validAnonymousNacmRules(sysrepo::Connection conn, const std::string& anonGroup);
}
