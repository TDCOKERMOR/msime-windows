import { serializeHostMessage } from '../../../../shared/messages';
import { applyDropdownValue, applyToggleState, setFuzzyRuleOptionsDisabled, setSmartPunctuationOptionsDisabled, setupDropdownMenu, setupNeuralModelControls, setupToggleButton } from './shared';
import { updateConfig } from './config-sync';
import { updateCandidatePreviewHelpcode } from './appearance';
import { setupCredentialTest, type CredentialTestService } from './credential-test';
import { onHostMessage } from '../utils/host-messages';
import { hoistOverlay } from '../utils/overlay-host';

type InputScheme = 'quanpin' | 'shuangpin' | 'wubi';
type InputMode = 'chinese' | 'japanese';

// 在线翻译只剩一种格式：OpenAI 兼容的 Chat Completions。
type TranslationProvider = 'openai';

// 在线翻译支持的四种协议格式。endpoint 里的 {model} 会被模型名替换。
type WireFormat = 'openai' | 'responses' | 'anthropic' | 'gemini';
const PROTOCOLS: Record<WireFormat, { endpoint: string; model: string; hint: string }> = {
  openai: {
    endpoint: 'https://api.openai.com/v1/chat/completions',
    model: 'gpt-4o-mini',
    hint: 'POST /chat/completions，Authorization: Bearer'
  },
  responses: {
    endpoint: 'https://api.openai.com/v1/responses',
    model: 'gpt-4.1-mini',
    hint: 'POST /responses，输入用 instructions + input'
  },
  anthropic: {
    endpoint: 'https://api.anthropic.com/v1/messages',
    model: 'claude-sonnet-4-5',
    hint: 'POST /v1/messages，x-api-key + anthropic-version'
  },
  gemini: {
    endpoint: 'https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent',
    model: 'gemini-2.5-flash',
    hint: 'POST generateContent，x-goog-api-key；地址里的 {model} 会被替换'
  }
};
let wireFormat: WireFormat = 'openai';
// 翻译提示词三槽，prompt 始终是当前槽的内容。
let promptSlot = 'custom_1';
let promptSlots: Record<string, string> = { custom_1: '', custom_2: '', custom_3: '' };

let applyingInputConfig = false;

const SHUANGPIN_REFRESH_TIMEOUT_MS = 5000;
let shuangpinToastTimer: number | null = null;
let pendingShuangpinRefresh: (() => void) | null = null;

function showShuangpinToast(message: string, ok: boolean, durationMs = 3200): void {
  const toast = document.getElementById('shuangpinToast');
  if (!toast) return;
  document.getElementById('shuangpinToastMessage')!.textContent = message;
  document.getElementById('shuangpinToastIcon')!.textContent = ok ? '' : '!';
  toast.className = `dict-toast visible ${ok ? 'success' : 'error'}`;
  if (shuangpinToastTimer !== null) window.clearTimeout(shuangpinToastTimer);
  shuangpinToastTimer = window.setTimeout(() => {
    toast.classList.remove('visible');
    shuangpinToastTimer = null;
  }, durationMs);
}

// 等宿主推回重新扫描后的快照再提示；超时没回来就报失败。重复点击只保留最后一次。
function refreshCustomShuangpins(): void {
  const webview = window.chrome?.webview;
  if (!webview) return;
  pendingShuangpinRefresh?.();
  const timer = window.setTimeout(() => {
    finish();
    showShuangpinToast('刷新失败，请稍后重试', false);
  }, SHUANGPIN_REFRESH_TIMEOUT_MS);
  const unsubscribe = onHostMessage('configSnapshot', (message) => {
    finish();
    const schemas = (message.data as Record<string, any> | undefined)?.input?.custom_shuangpin_schemas;
    const list: Array<Record<string, unknown>> = Array.isArray(schemas) ? schemas : [];
    const broken = list.filter((schema) => typeof schema?.error === 'string' && schema.error !== '').length;
    if (list.length === 0) {
      showShuangpinToast('刷新成功，文件夹里还没有自定义双拼方案', true);
    } else if (broken > 0) {
      showShuangpinToast(`找到 ${list.length} 个自定义方案，其中 ${broken} 个有错误，悬停在灰色方案上可查看原因`, false, 5000);
    } else {
      showShuangpinToast(`刷新成功，找到 ${list.length} 个自定义方案`, true);
    }
  });
  function finish(): void {
    window.clearTimeout(timer);
    unsubscribe();
    pendingShuangpinRefresh = null;
  }
  pendingShuangpinRefresh = finish;
  webview.postMessage(serializeHostMessage({ type: 'configRequest' }));
}

// 自定义双拼由 Server 扫描 shuangpin/custom 得到，追加在内置方案之后。
// 校验不通过的方案仍列出来但置灰，悬停提示错误原因，免得用户以为文件没被读到。
export function applyCustomShuangpinSchemas(schemas: unknown, directory: unknown): void {
  const menu = document.getElementById('shuangpinSchemeMenu');
  if (menu && Array.isArray(schemas)) {
    menu.querySelectorAll('.dropdown-item[data-custom]').forEach((item) => item.remove());
    for (const schema of schemas) {
      if (typeof schema?.id !== 'string' || typeof schema?.name !== 'string') continue;
      const item = document.createElement('div');
      item.className = 'dropdown-item';
      item.dataset.value = schema.id;
      item.dataset.custom = 'true';
      item.textContent = schema.name;
      if (typeof schema.error === 'string' && schema.error !== '') {
        item.setAttribute('aria-disabled', 'true');
        item.title = schema.error;
      } else if (typeof schema.name_en === 'string' && schema.name_en !== schema.name) {
        item.title = schema.name_en;
      }
      menu.appendChild(item);
    }
  }
  if (typeof directory === 'string') {
    const element = document.getElementById('customShuangpinDirectory');
    if (element) element.textContent = `文件夹：${directory}`;
  }
}

function updateInputConfig(path: string, value: string): void {
  window.chrome?.webview?.postMessage(serializeHostMessage({
    type: 'configUpdate',
    data: { path, value }
  }));
}

export function applyInputConfig(
  inputMode: string | undefined,
  schema: string | undefined,
  characterSet: string | undefined,
  shuangpinSchema: string | undefined,
  wubiSchema: string | undefined,
  wubiMixedPinyin?: boolean | undefined,
  wubiZMode?: string | undefined,
  wubiFourCodeAutoCommit?: boolean | undefined,
  wubiFifthCodeTopCommit?: boolean | undefined,
  defaultImeMode?: string | undefined,
  imeModeScope?: string | undefined,
  japaneseSchema?: string | undefined
): void {
  applyingInputConfig = true;
  try {
    const mode: InputMode = inputMode === 'japanese' ? 'japanese' : 'chinese';
  const modeRadio = document.querySelector<HTMLInputElement>(`input[name="input-mode"][value="${mode}"]`);
  if (modeRadio) modeRadio.checked = true;
  syncInputModeView(mode);

  if (schema === 'quanpin' || schema === 'shuangpin' || schema === 'wubi') {
    const radio = document.querySelector<HTMLInputElement>(`input[name="input-method"][value="${schema}"]`);
    if (radio) {
      radio.checked = true;
    }
    updateCandidatePreviewHelpcode({ input_schema: schema });
  }

  applyDropdownValue('characterSetBtn', 'characterSetMenu', characterSet);
  applyDropdownValue('shuangpinSchemeBtn', 'shuangpinSchemeMenu', shuangpinSchema);
  applyDropdownValue('wubiSchemeBtn', 'wubiSchemeMenu', wubiSchema);
  if (typeof wubiMixedPinyin === 'boolean') {
    applyToggleState('wubiMixedPinyinToggleBtn', wubiMixedPinyin);
  }
  if (typeof wubiZMode === 'string') {
    applyToggleState('wubiZModeToggleBtn', wubiZMode === 'wildcard');
  }
  if (typeof wubiFourCodeAutoCommit === 'boolean') {
    applyToggleState('wubiFourCodeAutoCommitToggleBtn', wubiFourCodeAutoCommit);
  }
  if (typeof wubiFifthCodeTopCommit === 'boolean') {
    applyToggleState('wubiFifthCodeTopCommitToggleBtn', wubiFifthCodeTopCommit);
  }
  applyDropdownValue('defaultImeModeBtn', 'defaultImeModeMenu', defaultImeMode);
  applyDropdownValue('imeModeScopeBtn', 'imeModeScopeMenu', imeModeScope);
  const japaneseRadio = document.querySelector<HTMLInputElement>(
    `input[name="japanese-input-method"][value="${japaneseSchema === 'romaji' ? japaneseSchema : 'romaji'}"]`
  );
  if (japaneseRadio) japaneseRadio.checked = true;
  } finally {
    applyingInputConfig = false;
  }
}

function syncInputModeView(mode: InputMode): void {
  const japanese = mode === 'japanese';
  document.querySelectorAll<HTMLElement>('.chinese-scheme-settings').forEach((element) => {
    element.hidden = japanese;
  });
  document.querySelectorAll<HTMLElement>('.japanese-scheme-settings').forEach((element) => {
    element.hidden = !japanese;
  });
}

export function applyFrequencyConfig(config: any): void {
  applyDropdownValue('frequencyModeBtn', 'frequencyModeMenu', config?.mode);
  applyDropdownValue('frequencyTriggerCountBtn', 'frequencyTriggerCountMenu', String(config?.trigger_count ?? 1));
  applyDropdownValue('frequencyLinearStepBtn', 'frequencyLinearStepMenu', String(config?.linear_step ?? 1));
}

function syncCandidateTranslationOptions(enabled: boolean): void {
  document.getElementById('candidateTranslationApiOptions')?.classList.toggle('is-disabled', !enabled);
}

function activeTranslationProvider(): TranslationProvider {
  return 'openai';
}

function inputValue(id: string): string {
  return (document.getElementById(id) as HTMLInputElement | null)?.value.trim() ?? '';
}

function applyTranslationPlaceholders(): void {
  const preset = PROTOCOLS[wireFormat];
  const endpoint = document.getElementById('openaiTranslationEndpoint') as HTMLInputElement | null;
  const model = document.getElementById('openaiTranslationModel') as HTMLInputElement | null;
  if (endpoint) endpoint.placeholder = preset.endpoint;
  if (model) model.placeholder = preset.model;
  syncCandidateTranslationWarning();
}

function translationTestConfig(): Record<string, string> {
  return {
    endpoint: inputValue('openaiTranslationEndpoint'),
    apiKey: inputValue('openaiTranslationApiKey'),
    model: inputValue('openaiTranslationModel'),
    prompt: (document.getElementById('openaiTranslationPrompt') as HTMLTextAreaElement | null)?.value.trim() ?? '',
    format: wireFormat
  };
}

function syncCandidateTranslationWarning(): void {
  const warning = document.getElementById('candidateTranslationApiWarning');
  if (!warning) return;
  const endpoint = (document.getElementById('openaiTranslationEndpoint') as HTMLInputElement | null)?.value.trim();
  const model = (document.getElementById('openaiTranslationModel') as HTMLInputElement | null)?.value.trim();
  const validEndpoint = /^https?:\/\/\S+$/i.test(endpoint ?? '');
  warning.textContent = validEndpoint
    ? '请填写模型名称（例如 gpt-4o-mini）'
    : '请填写以 http:// 或 https:// 开头的完整 Chat Completions 接口地址';
  warning.classList.toggle('is-hidden', Boolean(validEndpoint && model));
}

function syncTranslationProviderView(_provider: TranslationProvider): void {
  syncCandidateTranslationWarning();
}

function refreshTranslationProvider(): void {
  const provider = activeTranslationProvider();
  applyDropdownValue('translationProviderBtn', 'translationProviderMenu', provider);
  syncTranslationProviderView(provider);
}

export function applyTencentTmtConfig(config: Record<string, unknown> | undefined): void {
  applyDropdownValue(
    'translationTargetLanguageBtn',
    'translationTargetLanguageMenu',
    typeof config?.target_language === 'string' ? config.target_language : 'en'
  );
  syncCandidateTranslationWarning();
}

export function applyCustomTranslationConfig(config: Record<string, unknown> | undefined): void {
  const format = typeof config?.format === 'string' ? config.format : 'openai';
  wireFormat = (format === 'responses' || format === 'anthropic' || format === 'gemini') ? format : 'openai';
  applyDropdownValue('translationProviderBtn', 'translationProviderMenu', wireFormat);
  const endpoint = document.getElementById('openaiTranslationEndpoint') as HTMLInputElement | null;
  const apiKey = document.getElementById('openaiTranslationApiKey') as HTMLInputElement | null;
  const model = document.getElementById('openaiTranslationModel') as HTMLInputElement | null;
  const prompt = document.getElementById('openaiTranslationPrompt') as HTMLTextAreaElement | null;
  if (endpoint && typeof config?.endpoint === 'string') endpoint.value = config.endpoint;
  if (apiKey && typeof config?.api_key === 'string') apiKey.value = config.api_key;
  if (model && typeof config?.model === 'string') model.value = config.model;
  promptSlots = {
    custom_1: typeof config?.prompt_custom_1 === 'string' ? config.prompt_custom_1 : '',
    custom_2: typeof config?.prompt_custom_2 === 'string' ? config.prompt_custom_2 : '',
    custom_3: typeof config?.prompt_custom_3 === 'string' ? config.prompt_custom_3 : ''
  };
  // 旧配置只有单槽 prompt，迁到第一槽。
  if (!promptSlots.custom_1 && typeof config?.prompt === 'string') promptSlots.custom_1 = config.prompt;
  promptSlot =
    config?.prompt_id === 'custom_2' || config?.prompt_id === 'custom_3'
      ? (config.prompt_id as string)
      : 'custom_1';
  applyDropdownValue('translationPromptSlotBtn', 'translationPromptSlotMenu', promptSlot);
  if (prompt) prompt.value = promptSlots[promptSlot] ?? '';
  applyTranslationPlaceholders();
  refreshTranslationProvider();
}

export function applyNiuTransConfig(_config: Record<string, unknown> | undefined): void {
  // 小牛翻译已从界面移除，旧配置保留在文件里但不再影响候选词翻译。
}

function setupSecretVisibility(inputId: string, buttonId: string, name: string): void {
  const input = document.getElementById(inputId) as HTMLInputElement | null;
  const button = document.getElementById(buttonId) as HTMLButtonElement | null;
  button?.addEventListener('click', () => {
    if (!input) return;
    const show = input.type === 'password';
    input.type = show ? 'text' : 'password';
    button.setAttribute('aria-pressed', String(show));
    const label = `${show ? '隐藏' : '显示'} ${name}`;
    button.setAttribute('aria-label', label);
    button.title = label;
  });
}

export function setupInput(): void {
  document.querySelectorAll<HTMLInputElement>('input[name="input-mode"]').forEach((radio) => {
    radio.addEventListener('change', () => {
      if (applyingInputConfig) return;
      if (!radio.checked || (radio.value !== 'chinese' && radio.value !== 'japanese')) return;
      const mode = radio.value as InputMode;
      syncInputModeView(mode);
      updateInputConfig('input.mode', mode);
    });
  });

  document.querySelectorAll<HTMLInputElement>('input[name="japanese-input-method"]').forEach((radio) => {
    radio.addEventListener('change', () => {
      if (radio.checked && radio.value === 'romaji' && !applyingInputConfig) {
        updateInputConfig('input.japanese_schema', radio.value);
      }
    });
  });

  setupDropdownMenu('characterSetBtn', 'characterSetMenu', 'changeCharacterSet', true, 'input.character_set');
  setupDropdownMenu('defaultImeModeBtn', 'defaultImeModeMenu', '', true, 'input.default_ime_mode');
  setupDropdownMenu('imeModeScopeBtn', 'imeModeScopeMenu', '', true, 'input.ime_mode_scope');
  document.querySelectorAll<HTMLInputElement>('input[name="input-method"]').forEach((radio) => {
    radio.addEventListener('change', () => {
      if (applyingInputConfig) return;
      if (!radio.checked || (radio.value !== 'quanpin' && radio.value !== 'shuangpin' && radio.value !== 'wubi')) {
        return;
      }
      const schema = radio.value as InputScheme;
      updateInputConfig('input.schema', schema);
      updateCandidatePreviewHelpcode({ input_schema: schema });
    });
  });

  setupDropdownMenu(
    'shuangpinSchemeBtn',
    'shuangpinSchemeMenu',
    'changeShuangpinScheme',
    true,
    'input.shuangpin_schema'
  );
  // 打开自定义双拼所在文件夹，由宿主负责创建并用资源管理器打开
  document.getElementById('openCustomShuangpinDirectory')?.addEventListener('click', () => {
    window.chrome?.webview?.postMessage(serializeHostMessage({ type: 'openShuangpinDirectory' }));
  });
  // 重新请求配置快照：宿主会重新扫描并校验 custom 文件夹，下拉框随快照重建
  document.getElementById('refreshCustomShuangpins')?.addEventListener('click', refreshCustomShuangpins);
  hoistOverlay(document.getElementById('shuangpinToast'));
  document.getElementById('shuangpinToastClose')?.addEventListener('click', () => {
    document.getElementById('shuangpinToast')?.classList.remove('visible');
  });
  setupDropdownMenu('wubiSchemeBtn', 'wubiSchemeMenu', 'changeWubiScheme', true, 'input.wubi_schema');
  setupToggleButton('wubiMixedPinyinToggleBtn', (active) => {
    updateConfig('input.wubi_mixed_pinyin', active);
  });
  // z 键只剩通配两态，配置值仍是字符串 off | wildcard，开关只负责在这两者间选一个写回。
  setupToggleButton('wubiZModeToggleBtn', (active) => {
    updateConfig('input.wubi_z_mode', active ? 'wildcard' : 'off');
  });
  // 两个上屏开关彼此独立，各自只写自己的键：默认都开，关掉一个不影响另一个。
  setupToggleButton('wubiFourCodeAutoCommitToggleBtn', (active) => {
    updateConfig('input.wubi_four_code_auto_commit', active);
  });
  setupToggleButton('wubiFifthCodeTopCommitToggleBtn', (active) => {
    updateConfig('input.wubi_fifth_code_top_commit', active);
  });

  setupPageOptions();
  setupFrequencyOptions();
  setupToggleButton('wordToCharacterToggleBtn', (active) => {
    updateConfig('input.word_to_character', active);
  });
  document.querySelectorAll<HTMLInputElement>('input[name="word-to-character-keys"]').forEach((radio) => {
    radio.addEventListener('change', () => {
      if (radio.checked && !radio.disabled) updateConfig('input.word_to_character_keys', radio.value);
    });
  });
  // punctuation_lock 是单值配置（follow/chinese/english），用单选组表达三者互斥。
  document.querySelectorAll<HTMLInputElement>('input[name="punctuation-lock"]').forEach((radio) => {
    radio.addEventListener('change', () => {
      if (applyingInputConfig) return;
      if (!radio.checked) return;
      if (radio.value !== 'follow' && radio.value !== 'chinese' && radio.value !== 'english') return;
      updateConfig('input.punctuation_lock', radio.value);
    });
  });
  setupToggleButton('autocorrectTranspositionToggleBtn', (active) => {
    updateConfig('quanpin.autocorrect_transposition', active);
  });
  setupToggleButton('autocorrectNeighborToggleBtn', (active) => {
    updateConfig('quanpin.autocorrect_neighbor', active);
  });
  setupToggleButton('autocorrectMarkerToggleBtn', (active) => {
    updateConfig('quanpin.autocorrect_marker', active);
  });
  setupFuzzySection();
  setupSmartPunctuationSection();
  setupToggleButton('pairedPunctuationToggleBtn', (active) => {
    updateConfig('input.paired_punctuation', active);
  });
  setupToggleButton('escapeKeepsSelectedWordToggleBtn', (active) => {
    updateConfig('input.escape_keeps_selected_word', active);
  });
  setupToggleButton('enterLearnsEnglishWordToggleBtn', (active) => {
    updateConfig('input.enter_learns_english_word', active);
  });
  setupToggleButton('candidateTranslationsToggleBtn', (active) => {
    syncCandidateTranslationOptions(active);
    updateConfig('general.candidate_translations', active);
  });
  setupDropdownMenu(
    'translationTargetLanguageBtn',
    'translationTargetLanguageMenu',
    '',
    true,
    'tencent_tmt.target_language'
  );
  setupDropdownMenu('translationProviderBtn', 'translationProviderMenu', '', true);
  document.getElementById('translationProviderMenu')?.addEventListener('click', (event: Event) => {
    const item = (event.target as HTMLElement | null)?.closest<HTMLElement>('.dropdown-item');
    if (!item) return;
    const value = item.dataset.value;
    if (value !== 'openai' && value !== 'responses' && value !== 'anthropic' && value !== 'gemini') return;
    wireFormat = value;
    // 只换占位提示，不覆盖已填的地址和模型。
    applyTranslationPlaceholders();
    syncTranslationProviderView('openai');
    updateConfig('custom_translation.format', wireFormat);
    updateConfig('custom_translation.enabled', true);
  });
  const translationFields: Record<string, string> = {
    openaiTranslationEndpoint: 'custom_translation.endpoint',
    openaiTranslationApiKey: 'custom_translation.api_key',
    openaiTranslationModel: 'custom_translation.model'
  };
  Object.entries(translationFields).forEach(([id, path]) => {
    const input = document.getElementById(id) as HTMLInputElement | null;
    input?.addEventListener('change', () => {
      input.value = input.value.trim();
      updateConfig(path, input.value);
      syncCandidateTranslationWarning();
    });
  });
  setupDropdownMenu('translationPromptSlotBtn', 'translationPromptSlotMenu', '', false);
  document.getElementById('translationPromptSlotMenu')?.addEventListener('click', (event: Event) => {
    const item = (event.target as HTMLElement | null)?.closest<HTMLElement>('.dropdown-item');
    if (!item) return;
    const value = item.dataset.value;
    if (value !== 'custom_1' && value !== 'custom_2' && value !== 'custom_3') return;
    const textarea = document.getElementById('openaiTranslationPrompt') as HTMLTextAreaElement | null;
    // 切槽前先把当前输入存回原槽。
    if (textarea) promptSlots[promptSlot] = textarea.value;
    promptSlot = value;
    applyDropdownValue('translationPromptSlotBtn', 'translationPromptSlotMenu', promptSlot);
    if (textarea) textarea.value = promptSlots[promptSlot] ?? '';
    updateConfig('custom_translation.prompt_id', promptSlot);
  });
  const translationPrompt = document.getElementById('openaiTranslationPrompt') as HTMLTextAreaElement | null;
  translationPrompt?.addEventListener('change', () => {
    promptSlots[promptSlot] = translationPrompt.value;
    updateConfig(`custom_translation.prompt_custom_${promptSlot.slice(-1)}`, translationPrompt.value);
  });
  setupSecretVisibility('openaiTranslationApiKey', 'openaiTranslationApiKeyVisibility', 'API Key');
  setupCredentialTest(
    'candidateTranslationTestButton',
    'candidateTranslationTestStatus',
    () => 'translation.custom' as CredentialTestService,
    translationTestConfig
  );
  setupToggleButton('cloudCandidatesToggleBtn', (active) => {
    updateConfig('general.cloud_candidates', active);
  });
  // 四个整句来源和一个去重补位开关，默认全关。两个神经来源会自行在词格中生成最多 12 条
  // 内部备选；补位打开后，某来源的首选重复时沿该来源自己的排名寻找下一条不同结果。
  setupToggleButton('sentenceWordLatticeToggleBtn', (active) => {
    updateConfig('association.sentence_wordlattice', active);
  });
  setupToggleButton('sentenceGoogleToggleBtn', (active) => {
    updateConfig('association.sentence_google', active);
  });
  setupToggleButton('sentenceNeuralDesktopToggleBtn', (active) => {
    updateConfig('association.sentence_neural_desktop', active);
  });
  // 效果优先模型的下载/导入/删除按钮；状态与开关置灰由 shared.applyNeuralModelStatus 随快照驱动。
  setupNeuralModelControls();
  setupToggleButton('sentenceNeuralKeyboardToggleBtn', (active) => {
    updateConfig('association.sentence_neural_keyboard', active);
  });
  setupToggleButton('sentenceShowNextOnDuplicateToggleBtn', (active) => {
    updateConfig('association.sentence_show_next_on_duplicate', active);
  });
  // 只控制候选窗里整句候选后的来源标签，不影响候选本身。
  setupToggleButton('sentenceSourceBadgeToggleBtn', (active) => {
    updateConfig('association.sentence_source_badge', active);
  });
  // octagram 语法模型总开关：一并接入解码期搭配加成与 n-best 重排，引擎侧不再分两个开关。
  setupToggleButton('sentenceCollocationToggleBtn', (active) => {
    updateConfig('association.sentence_collocation_enabled', active);
  });
  // 模型列表行（激活单选/下载/删除）由 shared.seedCollocationRows 按 catalog 播种并在行构建时
  // 接线；状态文本、控件可用态与轮询由 shared.applyCollocationModelStatus 随配置快照驱动
  // （config-sync 调用）。
}

function setupFrequencyOptions(): void {
  setupDropdownMenu('frequencyModeBtn', 'frequencyModeMenu', '', true, 'frequency_adjustment.mode');
  setupDropdownMenu('frequencyTriggerCountBtn', 'frequencyTriggerCountMenu', '', true,
    'frequency_adjustment.trigger_count', Number);
  setupDropdownMenu('frequencyLinearStepBtn', 'frequencyLinearStepMenu', '', true,
    'frequency_adjustment.linear_step', Number);
}

function setupPageOptions(): void {
  document.querySelectorAll<HTMLInputElement>('input[name="page-method"]').forEach((checkbox) => {
    checkbox.addEventListener('change', () => {
      const configPaths: Record<string, string> = {
        minus: 'general.paging_minus_equal',
        comma: 'general.paging_comma_period',
        brackets: 'general.paging_brackets',
        tab: 'general.paging_tab',
        page: 'general.paging_page_up_down',
        arrow: 'general.candidate_arrow_navigation',
        wheel: 'general.paging_mouse_wheel'
      };
      const path = configPaths[checkbox.value];
      if (path) updateConfig(path, checkbox.checked);
    });
  });
}

// 模糊音分区：折叠头 + 总开关 + 11 规则复选。折叠交互仿 appearance.ts 的主题模式：
// aria-expanded 驱动 chevron 与容器显隐，默认收起且不跨会话持久化。
function setupFuzzySection(): void {
  setupToggleButton('fuzzyPinyinToggleBtn', (active) => {
    updateConfig('input.fuzzy_pinyin', active);
    setFuzzyRuleOptionsDisabled(!active);
  });
  const fuzzyExpand = document.getElementById('fuzzyExpand');
  const fuzzyDetails = document.getElementById('fuzzyDetails');
  fuzzyExpand?.addEventListener('click', () => {
    const expanded = fuzzyExpand.getAttribute('aria-expanded') !== 'true';
    fuzzyExpand.setAttribute('aria-expanded', String(expanded));
    fuzzyDetails?.classList.toggle('open', expanded);
  });
  setupFuzzyRuleOptions();
}

// 模糊音 11 键同名同前缀：checkbox value 直接是 [input] 段的配置键。
function setupFuzzyRuleOptions(): void {
  document.querySelectorAll<HTMLInputElement>('input[name="fuzzy-rule"]').forEach((checkbox) => {
    checkbox.addEventListener('change', () => {
      if (checkbox.value.startsWith('fuzzy_')) updateConfig(`input.${checkbox.value}`, checkbox.checked);
    });
  });
}

// 智能标点分区：折叠头 + 总开关 + 四个子开关。折叠交互与模糊音一致，默认收起。
function setupSmartPunctuationSection(): void {
  setupToggleButton('smartPunctuationToggleBtn', (active) => {
    updateConfig('input.smart_punctuation', active);
    setSmartPunctuationOptionsDisabled(!active);
  });
  setupToggleButton('smartPunctuationSpaceConvertToggleBtn', (active) => {
    updateConfig('input.smart_punctuation_space_convert', active);
  });
  setupToggleButton('smartPunctuationRepeatToChineseToggleBtn', (active) => {
    updateConfig('input.smart_punctuation_repeat_to_chinese', active);
  });
  setupToggleButton('smartPunctuationDirectDigitToggleBtn', (active) => {
    updateConfig('input.smart_punctuation_direct_digit', active);
  });
  setupToggleButton('smartPunctuationDirectLetterToggleBtn', (active) => {
    updateConfig('input.smart_punctuation_direct_letter', active);
  });
  const expand = document.getElementById('smartPunctuationExpand');
  const details = document.getElementById('smartPunctuationDetails');
  expand?.addEventListener('click', () => {
    const expanded = expand.getAttribute('aria-expanded') !== 'true';
    expand.setAttribute('aria-expanded', String(expanded));
    details?.classList.toggle('open', expanded);
  });
}
