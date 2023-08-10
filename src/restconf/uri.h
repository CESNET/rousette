/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#pragma once
#include <optional>
#include <stdexcept>
#include <string>

namespace libyang {
class Context;
}

namespace rousette::restconf {
std::optional<std::string> asLibyangPath(const libyang::Context& ctx, const std::string& uriPath);

class InvalidURIException : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

}
