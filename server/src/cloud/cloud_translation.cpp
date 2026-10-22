#include "cloud_translation.h"

#include "config/ime_config.h"
#include "ai_translation.h"
#include "aliyun_mt.h"
#include "custom_translation.h"
#include "niutrans_translation.h"
#include "tencent_tmt.h"
#include "translation_gloss.h"
#include "engine/english/english_dictionary.h"
#include <curl/curl.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace
{
// Wait until the candidate window has been idle for 500ms before a TMT request.
constexpr auto kIdleDelay = std::chrono::milliseconds(500);
constexpr size_t kMaxSourceChars = 40;
// 整句联想的候选是一整句话，不是词条。超过这个长度或带句读就按句子走：
// 单独一批、单独的超时与输出上限；写回词库时用更宽的长度门。
constexpr size_t kSentenceThreshold = 12;
constexpr size_t kMaxSentenceChars = 200;
// 写回词库的长度上限：词条 32，整句放宽到 128（中文一句常见 15–40 字，译成目标语种还会变长）。
constexpr size_t kMaxPersistChars = 32;
constexpr size_t kMaxPersistSentenceChars = 128;
constexpr size_t kMaxCacheEntries = 4096;
// 一轮最多校对多少条：校对是后台补一刀，不能无上限地拉长一个工作循环。
// 与翻译侧一次请求的上限对齐：再多模型容易漏行，反而一条修正都拿不回来。
constexpr size_t kMaxReviewItems = 12;
// 校对线程的积压上限：超过就丢旧的一批，宁可漏校也不排长队。
constexpr size_t kMaxReviewBacklog = 48;

std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_job{0};
std::vector<EnglishIme::TranslationQuery> g_latest_queries;
uint64_t g_latest_generation = 0;
std::chrono::steady_clock::time_point g_last_request;
std::string g_db_path;
CloudTranslation::ApplyCallback g_callback;

std::unordered_map<std::string, std::string> g_cache;

// 提供方对超时、限流、凭据错误和「没有译文」都只回空串，分不清哪次值得马上重试，所以失败项
// 按次数退避：网络恢复后很快能补上，持续失败的词也不会每次刷新候选都被重发、挤占批次预算。
struct FailureBackoff
{
    unsigned failures = 0;
    std::chrono::steady_clock::time_point retry_at;
};
std::unordered_map<std::string, FailureBackoff> g_failures;

std::string TargetLanguage(const TranslationConfig &config)
{
    const auto &language = config.tencent.target_language;
    return language.empty() ? "en" : language;
}

enum class Provider
{
    Tencent,
    Aliyun,
    Custom,
    NiuTrans,
    Ai,
};

// 当前生效的提供方由 [translation].provider 单一决定，服务端与页面不再各写一份优先级。
Provider ActiveProvider(const TranslationConfig &config)
{
    if (config.provider == "ai")
        return Provider::Ai;
    if (config.provider == "niutrans")
        return Provider::NiuTrans;
    if (config.provider == "aliyun")
        return Provider::Aliyun;
    if (config.provider == "custom")
        return Provider::Custom;
    return Provider::Tencent;
}

std::string ProviderScope(const TranslationConfig &config)
{
    switch (ActiveProvider(config))
    {
    case Provider::Ai:
        return "ai:" + config.ai.provider + ":" + config.ai.endpoint + ":" + config.ai.model;
    case Provider::Aliyun:
        // 阿里云没有应用 id，缓存域按 AccessKey ID 区分。
        return "aliyun:" + config.aliyun.secret_id;
    case Provider::NiuTrans:
        return "niutrans:" + config.niutrans.app_id;
    case Provider::Custom:
        // 协议与模型一起进缓存域：同一地址换协议/换模型，旧译文不该继续命中。
        return "custom:" + config.custom.endpoint + ":" +
               (config.custom.format == "deeplx" ? std::string("deeplx") : config.custom.model);
    case Provider::Tencent:
    default:
        return "tencent";
    }
}

std::string Identity(const EnglishIme::TranslationQuery &query, const std::string &target_language,
                     const std::string &provider_scope)
{
    return provider_scope + ":" + target_language + ":" +
           (query.direction == EnglishIme::TranslationDirection::EnglishToChinese ? "e:" : "z:") + query.key;
}

bool HasSentencePunctuation(const std::string &text)
{
    for (const char *mark : {"，", "。", "！", "？", "、", "；", "：", "…", "—", "“", "”", "《", "》"})
    {
        if (text.find(mark) != std::string::npos)
            return true;
    }
    for (const char mark : {',', '.', '!', '?', ';'})
    {
        if (text.find(mark) != std::string::npos)
            return true;
    }
    return false;
}

// 只有中文→目标语种那一路会问这句；英文候选都是单个单词。
bool IsSentenceLike(const std::string &key)
{
    return CloudTranslation::Utf8Length(key) > kSentenceThreshold || HasSentencePunctuation(key);
}

bool TooLong(const std::string &key)
{
    // 原来一律卡 40 字：整句候选正好死在这条线上，一整个长句连请求都发不出去。
    const size_t length = CloudTranslation::Utf8Length(key);
    return length > (IsSentenceLike(key) ? kMaxSentenceChars : kMaxSourceChars);
}

void RememberPositive(const std::string &identity, std::string gloss)
{
    std::lock_guard lock(g_mutex);
    if (g_cache.size() >= kMaxCacheEntries)
        g_cache.clear();
    g_cache[identity] = std::move(gloss);
    g_failures.erase(identity);
}

bool IsBackingOff(const std::string &identity)
{
    const auto found = g_failures.find(identity);
    return found != g_failures.end() && std::chrono::steady_clock::now() < found->second.retry_at;
}

void RememberFailure(const std::string &identity)
{
    std::lock_guard lock(g_mutex);
    if (g_failures.size() >= kMaxCacheEntries && g_failures.find(identity) == g_failures.end())
        g_failures.clear();
    auto &entry = g_failures[identity];
    entry.retry_at = std::chrono::steady_clock::now() + CloudTranslation::FailureRetryDelay(++entry.failures);
}

TencentTmt::Credentials ResolveCredentials(const TencentTmtConfig &config)
{
    TencentTmt::Credentials credentials;
    // provider 由设置页明确选成腾讯云时，加载配置会把 enabled 置为 true；只是从旧布尔推导出
    // 腾讯云的配置仍要求 enabled，和旧版一样，不会因为升级就开始向腾讯云发送候选词。
    if (!config.enabled)
        return credentials;
    credentials.region = config.region.empty() ? "ap-guangzhou" : config.region;
    credentials.secret_id = CloudTranslation::TrimSecret(config.secret_id);
    credentials.secret_key = CloudTranslation::TrimSecret(config.secret_key);
    if (!CloudTranslation::IsUsableSecret(credentials.secret_id) ||
        !CloudTranslation::IsUsableSecret(credentials.secret_key))
        return {};
    return credentials;
}

// 一个任务只需要当前生效提供方的配置，其余提供方保持空。
struct ResolvedProvider
{
    Provider provider = Provider::Tencent;
    TencentTmt::Credentials tencent;
    AliyunMt::Credentials aliyun;
    CustomTranslation::Config custom;
    NiuTransTranslation::Config niutrans;
    AiTranslation::Config ai;
};

std::optional<ResolvedProvider> ResolveActiveProvider(const TranslationConfig &config)
{
    ResolvedProvider resolved;
    resolved.provider = ActiveProvider(config);
    switch (resolved.provider)
    {
    case Provider::Ai:
        // 复用 AI 辅助当前服务商的接口地址、Token 与模型，与 ai_assistant.enabled 解耦：
        // 用户可能只想用翻译、不开 AI 联想。
        resolved.ai = AiTranslation::FromConfiguredTranslation(config);
        if (!AiTranslation::IsUsableConfig(resolved.ai))
            return std::nullopt;
        break;
    case Provider::Aliyun:
        resolved.aliyun = {CloudTranslation::TrimSecret(config.aliyun.secret_id),
                           CloudTranslation::TrimSecret(config.aliyun.secret_key)};
        if (!AliyunMt::IsUsableCredentials(resolved.aliyun))
            return std::nullopt;
        break;
    case Provider::NiuTrans:
        resolved.niutrans = {CloudTranslation::TrimSecret(config.niutrans.app_id),
                             CloudTranslation::TrimSecret(config.niutrans.apikey)};
        if (!NiuTransTranslation::IsUsableConfig(resolved.niutrans))
            return std::nullopt;
        break;
    case Provider::Custom:
        // 四协议、模型、提示词槽和校对开关都在这里落地：只拿 endpoint + api_key 会让
        // 用户选的协议被默默换成 Chat Completions，模型也退回 gpt-4o-mini。
        resolved.custom.endpoint = CloudTranslation::TrimSecret(config.custom.endpoint);
        resolved.custom.api_key = CloudTranslation::TrimSecret(config.custom.api_key);
        resolved.custom.format = config.custom.format;
        resolved.custom.model = config.custom.model.empty() ? std::string("gpt-4o-mini") : config.custom.model;
        resolved.custom.prompt = config.custom.prompt;
        resolved.custom.prompt_id = config.custom.prompt_id;
        resolved.custom.prompt_custom_1 = config.custom.prompt_custom_1;
        resolved.custom.prompt_custom_2 = config.custom.prompt_custom_2;
        resolved.custom.prompt_custom_3 = config.custom.prompt_custom_3;
        resolved.custom.review = config.custom.review;
        if (!CustomTranslation::IsSupportedEndpoint(resolved.custom.endpoint))
            return std::nullopt;
        break;
    case Provider::Tencent:
    default:
        resolved.tencent = ResolveCredentials(config.tencent);
        if (!CloudTranslation::IsUsableSecret(resolved.tencent.secret_id))
            return std::nullopt;
        break;
    }
    return resolved;
}

void PersistGloss(const EnglishIme::TranslationQuery &query, const std::string &gloss,
                  const std::string &target_language)
{
    if (g_db_path.empty())
        return;
    // 查得到就入本地库：词条进词表，整句也进——同一句话再打一次就不必再问云端。
    // 整句给更宽的长度上限，否则一句十几个字的译文会被词条的 32 字闸门挡在门外；
    // 上限仍然保留，不要拿一段话把词表腌掉。
    const bool chinese_to_english = query.direction == EnglishIme::TranslationDirection::ChineseToEnglish;
    const bool sentence = chinese_to_english && IsSentenceLike(query.key);
    if (!CloudTranslation::ShouldPersistGloss(query.key, gloss, sentence ? kMaxPersistSentenceChars
                                                                          : kMaxPersistChars))
        return;
    // 和英语一样：英语落 zh_en_glosses，其余语种落 zh_glosses(chinese, lang, gloss)。
    EnglishDictionary::upsert_gloss(g_db_path, chinese_to_english, query.key, gloss,
                                    chinese_to_english ? target_language : std::string{});
}

// 一轮里已经译出来、等着校对的条目。source 决定校对提示词的方向（en→zh / zh→目标）。
struct PendingReview
{
    EnglishIme::TranslationQuery query;
    std::string gloss;
    std::string source;
    bool sentence = false;
};

// 校对单独开一条线程。若和翻译挤在同一个循环里，用户连着打字时
// 下一句就得排队等校对跑完，反而更慢。
struct ReviewJob
{
    std::vector<PendingReview> items;
    CustomTranslation::Config custom;
    std::string target_language;
    std::string provider_scope;
};

std::mutex g_review_mutex;
std::condition_variable g_review_cv;
ReviewJob g_review_job;
bool g_review_pending = false;
std::thread g_review_worker;

void ApplyTranslatedGroup(const std::vector<EnglishIme::TranslationQuery> &group,
                          const std::vector<std::string> &translated, const std::string &source,
                          const std::string &target_language, const std::string &provider_scope,
                          std::vector<EnglishIme::TranslationResult> &out, bool sentence, bool review_enabled,
                          std::vector<PendingReview> &pending)
{
    for (size_t i = 0; i < group.size(); ++i)
    {
        // 结果数组比请求短，说明提供方的批次预算先用完了，后面的词根本没有发出，不算失败。
        if (i >= translated.size())
            break;
        const std::string identity = Identity(group[i], target_language, provider_scope);
        const std::string gloss = CloudTranslation::FormatGloss(translated[i]);
        if (gloss.empty())
        {
            RememberFailure(identity);
            continue;
        }
        RememberPositive(identity, gloss);
        PersistGloss(group[i], gloss, target_language);
        out.push_back({group[i].key, group[i].direction, gloss});
        // 译完先上屏，再交给校对；校对结果只回写词库与缓存，不阻塞这一帧。
        if (review_enabled && pending.size() < kMaxReviewItems)
            pending.push_back({group[i], gloss, source, sentence});
    }
}

std::vector<std::string> TranslateWithProvider(const ResolvedProvider &resolved, const std::vector<std::string> &texts,
                                               const std::string &source, const std::string &target)
{
    switch (resolved.provider)
    {
    case Provider::Ai:
        return AiTranslation::TextTranslateBatch(resolved.ai, texts, source, target);
    case Provider::Aliyun:
        return AliyunMt::TextTranslateBatch(resolved.aliyun, texts, source, target);
    case Provider::NiuTrans:
        return NiuTransTranslation::TextTranslateBatch(resolved.niutrans, texts, source, target);
    case Provider::Custom:
        return CustomTranslation::TextTranslateBatch(resolved.custom, texts, source, target);
    case Provider::Tencent:
    default:
        return TencentTmt::TextTranslateBatch(resolved.tencent, texts, source, target);
    }
}

void TranslateGroup(const ResolvedProvider &resolved, const std::vector<EnglishIme::TranslationQuery> &group,
                    const std::string &source, const std::string &target, const std::string &target_language,
                    const std::string &provider_scope, std::vector<EnglishIme::TranslationResult> &out,
                    std::vector<PendingReview> &pending, bool sentence = false)
{
    std::vector<std::string> texts;
    texts.reserve(group.size());
    for (const auto &query : group)
        texts.push_back(query.key);
    ResolvedProvider effective = resolved;
    // 整句与词条的超时、输出上限和提示词都不同：只在自定义服务上区分，其余提供方忽略这个标志。
    effective.custom.sentence = sentence;
    const auto translated = TranslateWithProvider(effective, texts, source, target);
    ApplyTranslatedGroup(group, translated, source, target_language, provider_scope, out, sentence,
                         resolved.provider == Provider::Custom && resolved.custom.review, pending);
}

// 译后校对：把本轮新译出的条目交给同一模型复核，只把「确实要改」的修正写回去。
// 只在自定义服务上做（deeplx 没有聊天接口；腾讯/小牛/阿里云没办法跑校对提示词）。
std::vector<EnglishIme::TranslationResult> ReviewPending(const ResolvedProvider &resolved,
                                                         const std::vector<PendingReview> &pending,
                                                         const std::string &target_language,
                                                         const std::string &provider_scope)
{
    std::vector<EnglishIme::TranslationResult> corrections;
    if (resolved.provider != Provider::Custom || pending.empty() || resolved.custom.format == "deeplx")
        return corrections;
    for (const std::string source : {"zh", "en"})
    {
        // 词条与整句分开发：整句的校对要重写一整段，超时和词条不是一个量级。
        for (const bool sentence : {false, true})
        {
            std::vector<const PendingReview *> group;
            for (const auto &item : pending)
            {
                if (item.source == source && item.sentence == sentence)
                    group.push_back(&item);
            }
            if (group.empty())
                continue;
            std::vector<std::string> texts;
            std::vector<std::string> translations;
            texts.reserve(group.size());
            translations.reserve(group.size());
            for (const auto *item : group)
            {
                texts.push_back(item->query.key);
                translations.push_back(item->gloss);
            }
            const std::string target = source == "en" ? std::string("zh") : target_language;
            const auto fixes =
                CustomTranslation::ReviewTranslations(resolved.custom, texts, translations, source, target, sentence);
            for (size_t i = 0; i < group.size() && i < fixes.size(); ++i)
            {
                if (fixes[i].empty())
                    continue;
                const std::string gloss = CloudTranslation::FormatGloss(fixes[i]);
                if (gloss.empty() || gloss == group[i]->gloss)
                    continue;
                // 先过长度门再写缓存：否则内存里是校对版、词库里还是旧版，重开输入法就回退。
                const bool chinese_to_english =
                    group[i]->query.direction == EnglishIme::TranslationDirection::ChineseToEnglish;
                const bool is_sentence = chinese_to_english && IsSentenceLike(group[i]->query.key);
                if (!CloudTranslation::ShouldPersistGloss(group[i]->query.key, gloss,
                                                          is_sentence ? kMaxPersistSentenceChars : kMaxPersistChars))
                    continue;
                // 校对结果回写：内存缓存 + 本地词库，下次同一个词直接给校对过的版本。
                RememberPositive(Identity(group[i]->query, target_language, provider_scope), gloss);
                PersistGloss(group[i]->query, gloss, target_language);
                corrections.push_back({group[i]->query.key, group[i]->query.direction, gloss});
            }
        }
    }
    return corrections;
}

void ReviewLoop()
{
    while (g_running)
    {
        ReviewJob job;
        {
            std::unique_lock lock(g_review_mutex);
            g_review_cv.wait(lock, [] { return !g_running || g_review_pending; });
            if (!g_running)
                break;
            job = std::move(g_review_job);
            g_review_job = ReviewJob{};
            g_review_pending = false;
        }
        if (job.items.empty())
            continue;
        ResolvedProvider resolved;
        resolved.provider = Provider::Custom;
        resolved.custom = job.custom;
        auto corrections = ReviewPending(resolved, job.items, job.target_language, job.provider_scope);
        if (corrections.empty() || !g_running || !g_callback)
            continue;
        uint64_t generation = 0;
        {
            std::lock_guard lock(g_mutex);
            generation = g_latest_generation;
        }
        if (!EnglishIme::IsTranslationCurrent(generation))
            continue;
        g_callback(std::move(corrections), generation);
    }
}

void WorkerLoop()
{
    uint64_t observed_job = g_job.load();
    while (g_running)
    {
        std::unique_lock lock(g_mutex);
        g_cv.wait(lock, [&] { return !g_running || g_job.load() != observed_job; });
        if (!g_running)
            break;
        observed_job = g_job.load();
        auto queries = g_latest_queries;
        auto generation = g_latest_generation;
        auto target = g_last_request + kIdleDelay;
        while (g_running)
        {
            if (g_cv.wait_until(lock, target, [&] { return !g_running || g_job.load() != observed_job; }))
            {
                if (!g_running)
                    return;
                observed_job = g_job.load();
                queries = g_latest_queries;
                generation = g_latest_generation;
                target = g_last_request + kIdleDelay;
                continue;
            }
            break;
        }
        lock.unlock();
        if (!g_running || queries.empty() || !EnglishIme::IsTranslationCurrent(generation))
            continue;

        const auto config = GetConfiguredTranslation();
        const auto resolved = ResolveActiveProvider(*config);
        if (!resolved)
            continue;

        const std::string target_language = TargetLanguage(*config);
        const std::string provider_scope = ProviderScope(*config);
        std::vector<EnglishIme::TranslationQuery> en_zh;
        std::vector<EnglishIme::TranslationQuery> zh_target;
        std::vector<EnglishIme::TranslationQuery> zh_sentence;
        std::vector<EnglishIme::TranslationResult> results;
        {
            std::lock_guard cache_lock(g_mutex);
            for (const auto &query : queries)
            {
                if (TooLong(query.key))
                    continue;
                const std::string identity = Identity(query, target_language, provider_scope);
                const auto cached = g_cache.find(identity);
                if (cached != g_cache.end())
                {
                    // A previous request may have finished after its candidate
                    // generation became stale. Its result is still valid for
                    // this language, so publish it to the current generation
                    // instead of silently skipping it.
                    results.push_back({query.key, query.direction, cached->second});
                    continue;
                }
                if (IsBackingOff(identity))
                    continue;
                if (query.direction == EnglishIme::TranslationDirection::EnglishToChinese &&
                    CloudTranslation::IsCloudTranslatableEnglish(query.key))
                    en_zh.push_back(query);
                else if (query.direction == EnglishIme::TranslationDirection::ChineseToEnglish &&
                         CloudTranslation::IsCloudTranslatableChinese(query.key))
                    (IsSentenceLike(query.key) ? zh_sentence : zh_target).push_back(query);
            }
        }

        // 每批译完就立即上屏一次：词条不该等整句（句子一路要几秒）才一起显出来。
        const auto publish = [&]() -> bool {
            if (results.empty())
                return true;
            if (!g_running || g_job.load() != observed_job || !EnglishIme::IsTranslationCurrent(generation) ||
                !g_callback)
                return false;
            g_callback(std::move(results), generation);
            results.clear();
            return true;
        };
        bool alive = true;
        std::vector<PendingReview> pending;
        if (!en_zh.empty())
        {
            TranslateGroup(*resolved, en_zh, "en", "zh", target_language, provider_scope, results, pending);
            alive = publish();
        }
        if (alive && !zh_target.empty())
        {
            TranslateGroup(*resolved, zh_target, "zh", target_language, target_language, provider_scope, results,
                           pending);
            alive = publish();
        }
        // 整句单独一批：超时和输出上限都跟词条不一样，混在一个批次里会互相拖。
        if (alive && !zh_sentence.empty())
        {
            TranslateGroup(*resolved, zh_sentence, "zh", target_language, target_language, provider_scope, results,
                           pending, /*sentence=*/true);
            alive = publish();
        }
        if (!alive || pending.empty())
            continue;
        // 校对丢给独立线程，工作循环立刻回去等下一次输入，上屏延迟不受影响。
        {
            std::lock_guard review_lock(g_review_mutex);
            if (g_review_job.items.size() + pending.size() > kMaxReviewBacklog)
                g_review_job.items.clear(); // 积压过多说明用户一直在连打，丢掉旧的一批
            g_review_job.items.insert(g_review_job.items.end(), pending.begin(), pending.end());
            g_review_job.custom = resolved->custom;
            g_review_job.target_language = target_language;
            g_review_job.provider_scope = provider_scope;
            g_review_pending = true;
        }
        g_review_cv.notify_one();
    }
}
} // namespace

namespace CloudTranslation
{
void Start(const std::string &db_path, ApplyCallback apply_callback)
{
    if (g_running)
        return;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_db_path = db_path;
    g_callback = std::move(apply_callback);
    g_running = true;
    g_worker = std::thread(WorkerLoop);
    g_review_worker = std::thread(ReviewLoop);
}

void Stop()
{
    if (!g_running)
        return;
    g_running = false;
    g_cv.notify_all();
    g_review_cv.notify_all();
    if (g_worker.joinable())
        g_worker.join();
    if (g_review_worker.joinable())
        g_review_worker.join();
    g_callback = {};
}

void RequestMisses(std::vector<EnglishIme::TranslationQuery> queries, uint64_t generation)
{
    {
        std::lock_guard lock(g_mutex);
        g_latest_queries = std::move(queries);
        g_latest_generation = generation;
        g_last_request = std::chrono::steady_clock::now();
        g_job.fetch_add(1);
    }
    g_cv.notify_one();
}

void Clear()
{
    RequestMisses({}, 0);
}

std::string ActiveProviderScope()
{
    return ProviderScope(*GetConfiguredTranslation());
}

std::string LookupCache(const std::string &key, EnglishIme::TranslationDirection direction)
{
    const auto config = GetConfiguredTranslation();
    const std::string identity = Identity({key, direction}, TargetLanguage(*config), ProviderScope(*config));
    std::lock_guard lock(g_mutex);
    const auto found = g_cache.find(identity);
    return found == g_cache.end() ? std::string{} : found->second;
}
} // namespace CloudTranslation
