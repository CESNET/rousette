/*
 * Copyright (C) 2016-2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
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

    ErrorResponse(int code, const std::string errorType, const std::string& errorTag, const std::string& errorMessage, const std::optional<std::string>& errorPath = std::nullopt)
        : code(code)
        , errorTag(errorTag)
        , errorType(errorType)
        , errorMessage(errorMessage)
        , errorPath(errorPath)
    {
    }

    const char* what() const noexcept override
    {
        return errorMessage.c_str();
    }
};
}
