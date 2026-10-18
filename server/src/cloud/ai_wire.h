#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// 四种对话协议的公共部分：请求体、鉴权头、响应取文、端点补全。
// 厂商选择退化为协议选择：调用方只关心 wire，不再关心是哪家。
namespace AiWire
{
enum class Wire
{
    Chat,      // OpenAI Chat Completions 及其兼容实现
    Responses, // OpenAI Responses
    Anthropic, // Anthropic Messages
    Gemini,    // Google Gemini generateContent
};

Wire FromId(const std::string &id);
std::string Id(Wire wire);

// 只填了主机名或 /v1 的地址，按协议补齐默认路径。
std::string NormalizeEndpoint(const std::string &endpoint, Wire wire, const std::string &model);

nlohmann::json BuildBody(Wire wire, const std::string &model, const std::string &system,
                         const std::string &user, int max_tokens, double temperature);

// 从响应里取正文；解析失败或带 error 字段时返回空串。
std::string ExtractText(Wire wire, const std::string &response);

// 鉴权相关请求头，name/value 成对返回；没有可加的就返回空。
struct Header
{
    std::string name;
    std::string value;
};
std::vector<Header> AuthHeaders(Wire wire, const std::string &api_key);
} // namespace AiWire
