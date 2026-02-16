/*
 * Copyright (C) 2016-2025 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "restconf/Exceptions.h"

namespace {
std::string constructErrorMessage(const std::string& uriSegment)
{
    return "Syntax error in URI (" + uriSegment + ")";
}

/** @brief Constructs an error message for a syntax error in a URI segment, including the position and expected token if provided.
 *  @param uriSegment The segment of the URI where the error occurred (e.g., "path" or "querystring").
 *  @param position The position in the URI segment where the error was detected.
 *  @param positionInPath An optional offset to add to the position for more accurate error reporting.
 *         Useful for URLs with querystrings in order to report the position in the entire URI rather than just the querystring segment.
 *  @param expectedToken A description of the expected token that was not found at the error position.
 * */
std::string constructErrorMessage(const std::string& uriSegment,
                                  const unsigned& position,
                                  const std::string& expectedToken)
{
    return constructErrorMessage(uriSegment) + " at position " + std::to_string(position) + ": expected " + expectedToken;
}
}

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
    : ErrorResponse(400, "protocol", "invalid-value", constructErrorMessage(uriSegment))
{
}

UriSyntaxError::UriSyntaxError(const std::string& uriSegment,
                               const unsigned& position,
                               const std::string& expectedToken)
    : ErrorResponse(400, "protocol", "invalid-value", constructErrorMessage(uriSegment, position, expectedToken))
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

UriQuerySyntaxError::UriQuerySyntaxError()
    : UriSyntaxError("query")
{
}

UriQuerySyntaxError::UriQuerySyntaxError(const unsigned& position, const std::string& expectedToken)
    : UriSyntaxError("query", position, expectedToken)
{
}
}
