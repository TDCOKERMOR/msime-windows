#include "translate/translate_provider.h"

#include "cloud/ai_translation.h"
#include "cloud/aliyun_mt.h"
#include "cloud/custom_translation.h"
#include "cloud/niutrans_translation.h"
#include "cloud/tencent_tmt.h"

namespace Translate
{
Outcome TranslateWithProvider(const RuntimeConfig &config, const Batch &batch)
{
    Outcome outcome;
    switch (config.vendor)
    {
    case Vendor::Ai:
        outcome.translations = AiTranslation::TextTranslateBatch(config.ai, batch.texts, batch.source, batch.target);
        break;
    case Vendor::Aliyun:
        outcome.translations = AliyunMt::TextTranslateBatch(config.aliyun, batch.texts, batch.source, batch.target);
        break;
    case Vendor::NiuTrans:
        outcome.translations =
            NiuTransTranslation::TextTranslateBatch(config.niutrans, batch.texts, batch.source, batch.target);
        break;
    case Vendor::Custom:
    {
        // 整句标志只对自定义服务有意义：它决定超时、输出上限和提示词。
        CustomTranslation::Config custom = config.custom;
        custom.sentence = batch.sentence;
        outcome.translations = CustomTranslation::TextTranslateBatch(custom, batch.texts, batch.source, batch.target);
        break;
    }
    case Vendor::Tencent:
    default:
        outcome.translations =
            TencentTmt::TextTranslateBatch(config.tencent, batch.texts, batch.source, batch.target);
        break;
    }
    return outcome;
}
} // namespace Translate
