/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <libyang-cpp/DataNode.hpp>

namespace sysrepo {
class Session;
}

namespace rousette::restconf {

libyang::DataNode replaceYangLibraryLocations(const std::optional<std::string>& schemeAndHost, const std::string& urlPrefix, libyang::DataNode& node);
bool hasAccessToYangSchema(const sysrepo::Session& session, const std::variant<libyang::Module, libyang::SubmoduleParsed>& module);
}
