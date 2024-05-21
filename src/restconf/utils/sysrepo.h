/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <string>

namespace sysrepo {
class Session;
}

namespace rousette::restconf {

bool dataExists(sysrepo::Session session, const std::string& path);
}
