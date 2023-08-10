/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <optional>
#include <string>

namespace libyang {
class Context;
}

namespace rousette::restconf {
std::optional<std::string> asLibyangPath(const libyang::Context& ctx, const std::string& uriPath);

}
