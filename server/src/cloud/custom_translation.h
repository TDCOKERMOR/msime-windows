#pragma once

#include <string>
#include <vector>

namespace CustomTranslation
{
struct Config
{
    std::string endpoint;
    std::string api_key;
    // openai / responses / anthropic / gemini / deeplx
    std::string format = "openai";
    std::string model = "gpt-4o-mini";
    // 自定义提示词，留空用内置释义提示；支持 {lang} 占位符。
    std::string prompt;
    // 整句模式：输入是一句完整的话，不是词条。提示词、输出长度和超时都按句子放宽，
    // 译文也不再压成「最多两个义项」。
    bool sentence = false;
};

bool IsSupportedEndpoint(const std::string &endpoint);
std::string ParseTranslationResponse(const std::string &response);
// 最近一次失败的原因（HTTP 状态码 + 响应片段），供「测试配置」展示。
std::string LastError();

// openai 格式走 Chat Completions，responses/anthropic/gemini 走各自的原生协议；
// 都是一条文本一个请求，空字符串表示这一条失败。
std::vector<std::string> TextTranslateBatch(const Config &config, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target);
} // namespace CustomTranslation
