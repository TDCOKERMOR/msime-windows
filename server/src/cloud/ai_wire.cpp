#include "ai_wire.h"

#include <vector>

namespace AiWire
{
Wire FromId(const std::string &id)
{
    if (id == "responses")
        return Wire::Responses;
    if (id == "anthropic")
        return Wire::Anthropic;
    if (id == "gemini")
        return Wire::Gemini;
    return Wire::Chat;
}

std::string Id(Wire wire)
{
    switch (wire)
    {
    case Wire::Responses:
        return "responses";
    case Wire::Anthropic:
        return "anthropic";
    case Wire::Gemini:
        return "gemini";
    case Wire::Chat:
    default:
        return "openai";
    }
}

std::string NormalizeEndpoint(const std::string &raw, Wire wire, const std::string &model)
{
    std::string endpoint = raw;
    const std::string token = "{model}";
    for (auto at = endpoint.find(token); at != std::string::npos; at = endpoint.find(token, at + model.size()))
        endpoint.replace(at, token.size(), model);
    while (!endpoint.empty() && endpoint.back() == '/')
        endpoint.pop_back();
    const auto scheme = endpoint.find("://");
    if (scheme == std::string::npos)
        return endpoint;
    const auto path_at = endpoint.find('/', scheme + 3);
    const std::string path = path_at == std::string::npos ? std::string() : endpoint.substr(path_at);
    if (path.empty())
    {
        switch (wire)
        {
        case Wire::Chat:
            return endpoint + "/v1/chat/completions";
        case Wire::Responses:
            return endpoint + "/v1/responses";
        case Wire::Anthropic:
            return endpoint + "/v1/messages";
        case Wire::Gemini:
            return endpoint + "/v1beta/models/" + model + ":generateContent";
        }
    }
    if (path == "/v1" || path == "/v1beta" || path == "/v1beta1")
    {
        switch (wire)
        {
        case Wire::Chat:
            return endpoint + "/chat/completions";
        case Wire::Responses:
            return endpoint + "/responses";
        case Wire::Anthropic:
            return endpoint + "/messages";
        case Wire::Gemini:
            return endpoint + "/models/" + model + ":generateContent";
        }
    }
    return endpoint;
}

nlohmann::json BuildBody(Wire wire, const std::string &model, const std::string &system,
                         const std::string &user, int max_tokens, double temperature)
{
    using nlohmann::json;
    switch (wire)
    {
    case Wire::Responses:
        return {{"model", model},
                {"instructions", system},
                {"input", user},
                {"temperature", temperature},
                {"max_output_tokens", max_tokens},
                {"stream", false}};
    case Wire::Anthropic:
        return {{"model", model},
                {"max_tokens", max_tokens},
                {"temperature", temperature},
                {"system", system},
                {"messages", json::array({{{"role", "user"}, {"content", user}}})}};
    case Wire::Gemini:
        return {{"systemInstruction", {{"parts", json::array({{{"text", system}}})}}},
                {"contents", json::array({{{"role", "user"}, {"parts", json::array({{{"text", user}}})}}})},
                {"generationConfig", {{"temperature", temperature}, {"maxOutputTokens", max_tokens}}}};
    case Wire::Chat:
    default:
        return {{"model", model},
                {"temperature", temperature},
                {"max_tokens", max_tokens},
                {"stream", false},
                {"messages", json::array({{{"role", "system"}, {"content", system}},
                                          {{"role", "user"}, {"content", user}}})}};
    }
}

namespace
{
std::string JoinPartTexts(const nlohmann::json &parts)
{
    if (!parts.is_array())
        return {};
    std::string text;
    for (const auto &part : parts)
    {
        if (!part.is_object())
            continue;
        const auto value = part.find("text");
        if (value != part.end() && value->is_string())
            text += value->get<std::string>();
    }
    return text;
}
} // namespace

std::string ExtractText(Wire wire, const std::string &response)
{
    try
    {
        const auto root = nlohmann::json::parse(response);
        const auto error = root.find("error");
        if (error != root.end() && !error->is_null())
            return {};
        switch (wire)
        {
        case Wire::Responses:
        {
            const auto output = root.find("output");
            if (output != root.end() && output->is_array())
            {
                for (const auto &item : *output)
                {
                    if (!item.is_object())
                        continue;
                    const auto content = item.find("content");
                    if (content == item.end())
                        continue;
                    const std::string text = JoinPartTexts(*content);
                    if (!text.empty())
                        return text;
                }
            }
            const auto direct = root.find("output_text");
            return direct != root.end() && direct->is_string() ? direct->get<std::string>() : std::string{};
        }
        case Wire::Anthropic:
        {
            const auto content = root.find("content");
            if (content == root.end())
                return {};
            if (content->is_string())
                return content->get<std::string>();
            return JoinPartTexts(*content);
        }
        case Wire::Gemini:
        {
            const auto candidates = root.find("candidates");
            if (candidates == root.end() || !candidates->is_array() || candidates->empty())
                return {};
            const auto content = candidates->front().find("content");
            if (content == candidates->front().end())
                return {};
            const auto parts = content->find("parts");
            return parts != content->end() ? JoinPartTexts(*parts) : std::string{};
        }
        case Wire::Chat:
        default:
        {
            const auto choices = root.find("choices");
            if (choices == root.end() || !choices->is_array() || choices->empty())
                return {};
            const auto legacy = choices->front().find("text");
            if (legacy != choices->front().end() && legacy->is_string())
                return legacy->get<std::string>();
            const auto message = choices->front().find("message");
            if (message == choices->front().end())
                return {};
            const auto content = message->find("content");
            if (content == message->end())
                return {};
            if (content->is_string())
                return content->get<std::string>();
            return JoinPartTexts(*content);
        }
        }
    }
    catch (...)
    {
    }
    return {};
}

std::vector<Header> AuthHeaders(Wire wire, const std::string &api_key)
{
    std::vector<Header> headers;
    if (api_key.empty())
        return headers;
    switch (wire)
    {
    case Wire::Anthropic:
        headers.push_back({"x-api-key", api_key});
        headers.push_back({"anthropic-version", "2023-06-01"});
        break;
    case Wire::Gemini:
        headers.push_back({"x-goog-api-key", api_key});
        break;
    case Wire::Chat:
    case Wire::Responses:
    default:
        headers.push_back({"Authorization", "Bearer " + api_key});
        break;
    }
    return headers;
}
} // namespace AiWire
