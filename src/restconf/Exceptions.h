/*
 * Copyright (C) 2016-2025 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
*/

#include <string>
#include <optional>

namespace rousette::restconf {

/** @brief RESTCONF-level protocol error response */
struct ErrorResponse : public std::exception {
    int code;
    std::string errorTag;
    std::string errorType;
    std::string errorMessage;
    std::optional<std::string> errorPath;

    ErrorResponse(int code, const std::string errorType, const std::string& errorTag, const std::string& errorMessage, const std::optional<std::string>& errorPath = std::nullopt);
    const char* what() const noexcept override;
};

struct UriSyntaxError : public ErrorResponse {
    UriSyntaxError();
    UriSyntaxError(const unsigned& position, const std::string& expectedToken);
};
}
