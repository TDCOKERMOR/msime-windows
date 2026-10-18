#include "cloud_translation.h"

#include "config/ime_config.h"
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
#include <thread>
#include <unordered_map>

namespace
{
// Wait until the candidate window has been idle for 500ms before a TMT request.
constexpr auto kIdleDelay = std::chrono::milliseconds(500);
constexpr auto kNegativeTtl = std::chrono::minutes(8);
constexpr size_t kMaxSourceChars = 40;
// 整句联想的候选是一整句话，不是词条。超过这个长度或带句读就按句子走：
// 单独一批、单独的超时与输出上限；写回词库时用更宽的长度门。
constexpr size_t kSentenceThreshold = 12;
constexpr size_t kMaxSentenceChars = 200;
// 写回词库的长度上限：词条 32，整句放宽到 128（中文一句常见 15–40 字，译成目标语种还会变长）。
constexpr size_t kMaxPersistChars = 32;
constexpr size_t kMaxPersistSentenceChars = 128;
constexpr size_t kMaxCacheEntries = 4096;

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
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_negative;

std::string TargetLanguage()
{
    const auto &language = GetConfiguredTencentTmt().target_language;
    return language.empty() ? "en" : language;
}

enum class Provider
{
    Tencent,
    Custom,
    NiuTrans,
};

// 只保留一个在线提供方：自定义的 OpenAI 兼容服务优先，旧的 NiuTrans / 腾讯云
// 仅作为旧配置的回退路径保留。
Provider ActiveProvider()
{
    if (GetConfiguredCustomTranslation().enabled)
        return Provider::Custom;
    if (GetConfiguredNiuTrans().enabled)
        return Provider::NiuTrans;
    return Provider::Tencent;
}

std::string ProviderScope()
{
    switch (ActiveProvider())
    {
    case Provider::NiuTrans:
        return "niutrans:" + GetConfiguredNiuTrans().app_id;
    case Provider::Custom:
        return "custom:" + GetConfiguredCustomTranslation().endpoint + ":" +
               (GetConfiguredCustomTranslation().format == "deeplx" ? std::string("deeplx")
                                                                   : GetConfiguredCustomTranslation().model);
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
    g_negative.erase(identity);
}

bool IsNegative(const std::string &identity)
{
    const auto found = g_negative.find(identity);
    if (found == g_negative.end())
        return false;
    if (found->second <= std::chrono::steady_clock::now())
    {
        g_negative.erase(found);
        return false;
    }
    return true;
}

void RememberNegative(const std::string &identity)
{
    std::lock_guard lock(g_mutex);
    if (g_negative.size() >= kMaxCacheEntries)
        g_negative.clear();
    g_negative[identity] = std::chrono::steady_clock::now() + kNegativeTtl;
}

TencentTmt::Credentials ResolveCredentials()
{
    TencentTmt::Credentials credentials;
    const auto &config = GetConfiguredTencentTmt();
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

CustomTranslation::Config ResolveCustomConfig()
{
    const auto &configured = GetConfiguredCustomTranslation();
    if (!configured.enabled)
        return {};
    CustomTranslation::Config resolved;
    resolved.endpoint = CloudTranslation::TrimSecret(configured.endpoint);
    resolved.api_key = CloudTranslation::TrimSecret(configured.api_key);
    resolved.format = configured.format == "deeplx" ? "deeplx" : "openai";
    resolved.model = CloudTranslation::TrimSecret(configured.model);
    if (resolved.model.empty())
        resolved.model = "gpt-4o-mini";
    resolved.prompt = configured.prompt;
    return resolved;
}

NiuTransTranslation::Config ResolveNiuTransConfig()
{
    const auto &configured = GetConfiguredNiuTrans();
    if (!configured.enabled)
        return {};
    return {CloudTranslation::TrimSecret(configured.app_id), CloudTranslation::TrimSecret(configured.apikey)};
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

void ApplyTranslatedGroup(const std::vector<EnglishIme::TranslationQuery> &group,
                          const std::vector<std::string> &translated, const std::string &target_language,
                          const std::string &provider_scope, std::vector<EnglishIme::TranslationResult> &out)
{
    for (size_t i = 0; i < group.size(); ++i)
    {
        const std::string identity = Identity(group[i], target_language, provider_scope);
        // translated 比 group 短，是批次预算到点了：后面的条目根本没发出去。
        // 把「没发」当成「失败」记进负缓存，它们会在 8 分钟内不再重试，用户看到的就是
        // 「怎么又没翻译」——这正是之前整句看起来彻底死掉的一半原因。
        const bool attempted = i < translated.size();
        const std::string raw = attempted ? translated[i] : std::string{};
        const std::string gloss = CloudTranslation::FormatGloss(raw);
        if (gloss.empty())
        {
            if (attempted)
                RememberNegative(identity);
            continue;
        }
        RememberPositive(identity, gloss);
        PersistGloss(group[i], gloss, target_language);
        out.push_back({group[i].key, group[i].direction, gloss});
    }
}

std::vector<std::string> TranslateWithProvider(Provider provider, const TencentTmt::Credentials &credentials,
                                               const CustomTranslation::Config &custom,
                                               const NiuTransTranslation::Config &niutrans,
                                               const std::vector<std::string> &texts, const std::string &source,
                                               const std::string &target)
{
    switch (provider)
    {
    case Provider::NiuTrans:
        return NiuTransTranslation::TextTranslateBatch(niutrans, texts, source, target);
    case Provider::Custom:
        return CustomTranslation::TextTranslateBatch(custom, texts, source, target);
    case Provider::Tencent:
    default:
        return TencentTmt::TextTranslateBatch(credentials, texts, source, target);
    }
}

void TranslateGroup(Provider provider, const TencentTmt::Credentials &credentials,
                    const CustomTranslation::Config &custom, const NiuTransTranslation::Config &niutrans,
                    const std::vector<EnglishIme::TranslationQuery> &group, const std::string &source,
                    const std::string &target, const std::string &target_language, const std::string &provider_scope,
                    std::vector<EnglishIme::TranslationResult> &out, bool sentence = false)
{
    std::vector<std::string> texts;
    texts.reserve(group.size());
    for (const auto &query : group)
        texts.push_back(query.key);
    CustomTranslation::Config custom_config = custom;
    custom_config.sentence = sentence;
    const auto translated = TranslateWithProvider(provider, credentials, custom_config, niutrans, texts, source, target);
    ApplyTranslatedGroup(group, translated, target_language, provider_scope, out);
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

        const Provider provider = ActiveProvider();
        const auto custom = ResolveCustomConfig();
        const auto niutrans = ResolveNiuTransConfig();
        const auto credentials = provider == Provider::Tencent ? ResolveCredentials() : TencentTmt::Credentials{};
        if ((provider == Provider::Custom && !CustomTranslation::IsSupportedEndpoint(custom.endpoint)) ||
            (provider == Provider::NiuTrans && !NiuTransTranslation::IsUsableConfig(niutrans)) ||
            (provider == Provider::Tencent && !CloudTranslation::IsUsableSecret(credentials.secret_id)))
            continue;

        const std::string target_language = TargetLanguage();
        const std::string provider_scope = ProviderScope();
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
                if (IsNegative(identity))
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
        if (!en_zh.empty())
        {
            TranslateGroup(provider, credentials, custom, niutrans, en_zh, "en", "zh", target_language,
                           provider_scope, results);
            alive = publish();
        }
        if (alive && !zh_target.empty())
        {
            TranslateGroup(provider, credentials, custom, niutrans, zh_target, "zh", target_language,
                           target_language, provider_scope, results);
            alive = publish();
        }
        // 整句单独一批：超时和输出上限都跟词条不一样，混在一个批次里会互相拖。
        if (alive && !zh_sentence.empty())
        {
            TranslateGroup(provider, credentials, custom, niutrans, zh_sentence, "zh", target_language,
                           target_language, provider_scope, results, /*sentence=*/true);
            alive = publish();
        }
        if (!alive)
            continue;
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
}

void Stop()
{
    if (!g_running)
        return;
    g_running = false;
    g_cv.notify_all();
    if (g_worker.joinable())
        g_worker.join();
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

std::string LookupCache(const std::string &key, EnglishIme::TranslationDirection direction)
{
    std::lock_guard lock(g_mutex);
    const auto found = g_cache.find(Identity({key, direction}, TargetLanguage(), ProviderScope()));
    return found == g_cache.end() ? std::string{} : found->second;
}
} // namespace CloudTranslation
