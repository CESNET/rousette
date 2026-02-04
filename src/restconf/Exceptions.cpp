/*
 * Copyright (C) 2016-2025 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
*/

#include "restconf/Exceptions.h"

namespace rousette::restconf {
ErrorResponse::ErrorResponse(int code, const std::string errorType, const std::string& errorTag, const std::string& errorMessage, const std::optional<std::string>& errorPath)
    : code(code)
    , errorTag(errorTag)
    , errorType(errorType)
    , errorMessage(errorMessage)
    , errorPath(errorPath)
{
}

const char* ErrorResponse::what() const noexcept
{
    return errorMessage.c_str();
}

UriSyntaxError::UriSyntaxError(const std::string& uriSegment)
    : ErrorResponse(400, "protocol", "invalid-value", "Syntax error in URI " + uriSegment)
{
}

UriSyntaxError::UriSyntaxError(const std::string& uriSegment,
                               const unsigned& position,
                               const std::string& expectedToken)
    : ErrorResponse(400,
                    "protocol",
                    "invalid-value",
                    "Syntax error in URI " + uriSegment + " at position " + std::to_string(position) + ": expected " + expectedToken)
{
}

UriPathSyntaxError::UriPathSyntaxError()
    : UriSyntaxError("path")
{
}

UriPathSyntaxError::UriPathSyntaxError(const unsigned& position, const std::string& expectedToken)
    : UriSyntaxError("path", position, expectedToken)
{
}

UriQueryStringSyntaxError::UriQueryStringSyntaxError()
    : UriSyntaxError("querystring")
{
}

UriQueryStringSyntaxError::UriQueryStringSyntaxError(const unsigned& position, const std::string& expectedToken)
    : UriSyntaxError("querystring", position, expectedToken)
{
}
}
