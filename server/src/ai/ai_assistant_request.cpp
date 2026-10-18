#include "ai_assistant_request.h"

#include "cloud/ai_wire.h"
#include "voice-input/voice_providers.h"

namespace AiAssistant::detail
{
nlohmann::json BuildRequestBody(const Request &request)
{
    const auto &config = request.config;
    // provider 字段现在存的是协议 id（openai / responses / anthropic / gemini），
    // 老配置里的 deepseek / siliconflow 之类会落到 Chat Completions。
    const auto wire = AiWire::FromId(config.provider);
    const bool thinking = VoiceInput::UsesDeepSeekThinking("deepseek", config.thinking);
    nlohmann::json input = {{"segmented_pinyin", request.pinyin_segments},
                            {"context", request.context},
                            {"candidate_limit", config.candidate_limit}};
    // Reasoning tokens may count against max_tokens; 512 would cut the JSON answer off.
    const int max_tokens = thinking ? 8192 : 512;
    nlohmann::json body = AiWire::BuildBody(wire, config.model, config.prompt, input.dump(), max_tokens, 0.2);
    if (wire == AiWire::Wire::Chat)
    {
        body["response_format"] = {{"type", "json_object"}};
        if (thinking)
        {
            body["thinking"] = {{"type", "enabled"}};
            body["reasoning_effort"] = config.thinking;
        }
        else if (config.provider == "deepseek")
        {
            body["thinking"] = {{"type", "disabled"}};
        }
    }
    return body;
}

long RequestTimeoutMs(const AiAssistantConfig &config)
{
    return VoiceInput::UsesDeepSeekThinking("deepseek", config.thinking) ? 30000L : 8000L;
}
} // namespace AiAssistant::detail
