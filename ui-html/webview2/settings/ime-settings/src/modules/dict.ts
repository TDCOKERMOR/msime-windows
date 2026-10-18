import { createDictionaryPager, DICTIONARY_PAGE_SIZE } from '../utils/dictionary-pagination';
import { readDictionaryFile } from '../utils/dictionary-file';
import { confirmDialog } from '../utils/confirm-dialog';
import { hoistOverlay } from '../utils/overlay-host';
import { onHostMessage } from '../utils/host-messages';
import type { SettingsMessage } from '../../../../shared/messages';
type DictionaryRequest = Extract<SettingsMessage, { type: 'dictionaryRequest' }>['data'];
import { serializeHostMessage } from '../../../../shared/messages';
type DictionaryType = 'quanpin' | 'wubi' | 'english';
// 查询模式：三个词库任意一个，或同时查三个
type QueryTarget = DictionaryType | 'all' | 'gloss';
type DictionaryRow = { code?: string; word: string; weight?: number; display?: string };

let dictionary: QueryTarget = 'quanpin';
let editing: DictionaryRow | null = null;
let requestCounter = 0;
let lastQuery = '';
// 联合查询：一组三个请求，全部回来后合并渲染
let allGroup = '';
const allBuckets = new Map<string, DictionaryRow[]>();
// 联合查询的四个来源：三个词库 + 一份多语释义总表
const ALL_SOURCES = ['quanpin', 'wubi', 'english', 'gloss'] as const;
const SOURCE_LABEL: Record<string, string> = { quanpin: '全拼', wubi: '五笔', english: '英文', gloss: '释义' };
// 每个语种一个独立标签页，各自查询、各自分页。
const GLOSS_TABS: Record<string, string> = {
  ja: '日语', fr: '法语', es: '西班牙语', ru: '俄语', ko: '韩语',
  de: '德语', th: '泰语', vi: '越南语', it: '意大利语'
};
const glossTabLanguage = (): string | null => GLOSS_TABS[dictionary] ? dictionary : null;
const pendingRequests = new Map<string, { action: DictionaryRequest['action']; dictionary: string }>();
let latestQueryId = '';
let pager: ReturnType<typeof createDictionaryPager> | undefined;
let toastTimer: number | null = null;

function post(action: DictionaryRequest['action'], data: Partial<Omit<DictionaryRequest, 'action' | 'requestId'>> = {}): void {
  const requestId = `dict-${++requestCounter}`;
  const targetDictionary = (glossTabLanguage() ? 'english' : dictionary) as QueryTarget;
  pendingRequests.set(requestId, { action, dictionary: targetDictionary });
  if (action === 'query') { latestQueryId = requestId; pager?.loading(); }
  window.chrome?.webview?.postMessage(serializeHostMessage({
    type: 'dictionaryRequest',
    data: { requestId, dictionary: targetDictionary, action, ...data }
  }));
}

function showToast(message: string, ok: boolean, durationMs = 3200): void {
  const toast = document.getElementById('dictToast');
  if (!toast) return;
  const messageElement = document.getElementById('dictToastMessage');
  const iconElement = document.getElementById('dictToastIcon');
  if (messageElement) messageElement.textContent = message;
  if (iconElement) iconElement.textContent = ok ? '' : '!';
  toast.className = `dict-toast visible ${ok ? 'success' : 'error'}`;
  if (toastTimer !== null) window.clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => {
    toast.classList.remove('visible');
    toastTimer = null;
  }, durationMs);
}

function renderRows(rows: DictionaryRow[]): void {
  const body = document.getElementById('dictRows');
  if (!body) return;
  if (!rows.length) {
    body.innerHTML = '<tr><td colspan="5" class="dict-empty">没有找到词条</td></tr>';
    syncTableHeaderWidth();
    return;
  }
  body.replaceChildren(...rows.map((row, index) => {
    const tr = document.createElement('tr');
    const indexCell = document.createElement('td');
    indexCell.className = 'dict-index-column';
    indexCell.textContent = String((pager?.offset ?? 0) + index + 1);
    tr.appendChild(indexCell);
    const values = glossTabLanguage()
      ? [row.word, row.display ?? '']
      : dictionary === 'english'
      ? [row.word, row.display ?? row.word, String(row.weight ?? 0)]
      : [row.code ?? '', row.word, String(row.weight ?? 0)];
    values.forEach((value) => {
      const td = document.createElement('td');
      td.textContent = value;
      td.addEventListener('mouseenter', () => {
        if (td.scrollWidth > td.clientWidth) td.title = value;
        else td.removeAttribute('title');
      });
      tr.appendChild(td);
    });
    if (glossTabLanguage()) return tr;
    const actions = document.createElement('td');
    const edit = document.createElement('button'); edit.className = 'dict-row-action'; edit.textContent = '编辑';
    edit.addEventListener('click', () => openDialog(row));
    const remove = document.createElement('button'); remove.className = 'dict-row-action danger'; remove.textContent = '删除';
    remove.addEventListener('click', async () => {
      if (!await confirmDialog(`确定删除“${row.word}”吗？`)) return;
      if (dictionary === 'english') post('delete', { oldWord: row.word, oldDisplay: row.display ?? row.word, word: row.word, display: row.display ?? row.word });
      else post('delete', { oldCode: row.code, oldWord: row.word, code: row.code, word: row.word, weight: row.weight });
    });
    actions.append(edit, remove); tr.appendChild(actions); return tr;
  }));
  syncTableHeaderWidth();
}

function syncTableHeaderWidth(): void {
  window.requestAnimationFrame(() => {
    const scrollArea = document.querySelector<HTMLElement>('.dict-table-wrap');
    const header = document.getElementById('dictTableHeaderWrap');
    if (!scrollArea || !header) return;
    header.style.paddingRight = `${scrollArea.offsetWidth - scrollArea.clientWidth}px`;
  });
}

function updateMode(): void {
  latestQueryId = '';
  pager?.reset();
  const english = dictionary === 'english';
  const quanpin = dictionary === 'quanpin';
  const all = dictionary === 'all';
  const gloss = glossTabLanguage() !== null;
  const search = document.getElementById('dictSearch') as HTMLInputElement;
  search.value = '';
  search.placeholder = gloss
    ? `输入中文词头，查${GLOSS_TABS[dictionary] ?? ''}释义，例如 一定`
    : all
    ? '输入关键词，同时查全拼 / 五笔 / 英文，并列出全部语种释义'
    : english ? '输入英文前缀，例如 meta' : quanpin
      ? '输入完整全拼，例如 nihao' : '输入五笔编码前缀';
  document.getElementById('dictHint')!.textContent = gloss
    ? '按中文词头前缀查询该语种释义；每页 100 条，本地没有的词会由在线翻译补全并写回'
    : all
    ? '同一个关键词分别查全拼 / 五笔 / 英文，并附上该词头的全部语种释义，合并后按权重排序；联合结果只读'
    : english
      ? '按英文前缀查询；批量导入格式为：单词<Tab>显示内容<Tab>权重（兼容无权重的两列文件）'
      : quanpin
        ? '全拼新增会校验拼音合法性、汉字数量和重复词条；批量导入格式为：词语<Tab>全拼[<Tab>权重]（兼容 Rime userdb.txt / dict.yaml，全拼可用空格或 \' 分音节）'
        : '管理 86 五笔编码、词条及权重；批量导入格式为：词语<Tab>五笔编码[<Tab>权重]（兼容 Rime dict.yaml）';
  const glossExport = document.getElementById('dictGlossExportCurrent') as HTMLButtonElement | null;
  if (glossExport) glossExport.hidden = !gloss;
  const importButton = document.getElementById('dictImportButton') as HTMLButtonElement | null;
  if (importButton) importButton.style.display = all || gloss ? 'none' : '';

  // 纯汉字导入按当前标签页出码：拼音走注音，五笔走 wubi86 单字码组合。
  // 英文词库收不了纯汉字，隐藏以免用户点出必然失败的请求。
  const hansSection = document.querySelector<HTMLElement>('.dict-hans-import-section');
  if (hansSection) hansSection.style.display = english || all || gloss ? 'none' : '';
  const hansTitle = document.getElementById('dictHansImportTitle');
  if (hansTitle) {
    hansTitle.textContent = quanpin
      ? '批量导入纯汉字词组(e.g.人名)到拼音词库'
      : '批量导入纯汉字词组(e.g.人名)到五笔词库';
  }
  const addButton = document.getElementById('dictAddButton') as HTMLButtonElement | null;
  if (addButton) addButton.style.display = all || gloss ? 'none' : '';
  const pagination = document.querySelector<HTMLElement>('.dict-pagination');
  if (pagination) pagination.style.display = all ? 'none' : '';
  document.getElementById('dictTableHeader')!.innerHTML = gloss
    ? '<th class="dict-index-column">No.</th><th>词头</th><th>释义</th>'
    : all
    ? '<th class="dict-index-column">No.</th><th>来源</th><th>编码/单词</th><th>词条</th><th>权重</th>'
    : english
      ? '<th class="dict-index-column">No.</th><th>单词</th><th>显示内容</th><th>权重</th><th>操作</th>'
      : '<th class="dict-index-column">No.</th><th>编码</th><th>词条</th><th>权重</th><th>操作</th>';
  document.getElementById('dictRows')!.innerHTML = '<tr><td colspan="5" class="dict-empty">输入查询条件后查看词条</td></tr>';
  syncTableHeaderWidth();
}

function openDialog(row: DictionaryRow | null = null): void {
  editing = row;
  const english = dictionary === 'english';
  document.getElementById('dictDialogTitle')!.textContent = row ? '编辑词条' : '新增词条';
  document.getElementById('dictCodeField')!.firstChild!.textContent = english ? '单词' : dictionary === 'quanpin' ? '全拼' : '五笔编码';
  document.getElementById('dictWordField')!.firstChild!.textContent = english ? '显示内容' : '词条';
  (document.getElementById('dictCode') as HTMLInputElement).value = row
    ? (english ? row.word : row.code ?? '')
    : '';
  document.getElementById('dictWeightField')!.style.display = 'grid';
  (document.getElementById('dictWord') as HTMLInputElement).value = english ? row?.display ?? '' : row?.word ?? '';
  (document.getElementById('dictWeight') as HTMLInputElement).value = row?.weight === undefined ? '10' : String(row.weight);
  const modal = document.getElementById('dictModal')!; modal.classList.add('open'); modal.setAttribute('aria-hidden', 'false');
}

function closeDialog(): void {
  const modal = document.getElementById('dictModal')!; modal.classList.remove('open'); modal.setAttribute('aria-hidden', 'true'); editing = null;
}

function query(offset = 0): void {
  if (offset === 0) lastQuery = (document.getElementById('dictSearch') as HTMLInputElement).value.trim();
  if (!lastQuery) { showToast('请输入查询内容', false); return; }
  if (dictionary === 'all') { queryAll(); return; }
  const language = glossTabLanguage();
  if (language) {
    post('query', { word: lastQuery, code: language, offset, limit: DICTIONARY_PAGE_SIZE });
    return;
  }
  post('query', {
    ...(dictionary === 'english' ? { word: lastQuery } : { code: lastQuery }),
    offset,
    limit: DICTIONARY_PAGE_SIZE
  });
}

// 联合查询：同一个关键词分别打给三个词库，全部返回后合并成一张表。
function queryAll(): void {
  allGroup = `dict-all-${++requestCounter}`;
  allBuckets.clear();
  ALL_SOURCES.forEach((source) => {
    const requestId = `${allGroup}-${source}`;
    pendingRequests.set(requestId, { action: 'query', dictionary: 'all' });
    // 释义总表复用英文通道：code=all 时按词头聚合全部语种。
    const channel: DictionaryType = source === 'gloss' ? 'english' : source;
    window.chrome?.webview?.postMessage(serializeHostMessage({
      type: 'dictionaryRequest',
      data: {
        requestId,
        dictionary: channel,
        action: 'query',
        ...(source === 'gloss'
          ? { word: lastQuery, code: 'all' }
          : source === 'english' ? { word: lastQuery } : { code: lastQuery }),
        offset: 0,
        limit: 20
      }
    }));
  });
}

function renderMergedRows(): void {
  const body = document.getElementById('dictRows');
  if (!body) return;
  const merged: { source: string; code: string; word: string; weight: number }[] = [];
  allBuckets.forEach((rows, source) => {
    rows.forEach((row) => merged.push({
      source,
      code: source === 'english' || source === 'gloss' ? row.word : (row.code ?? ''),
      word: source === 'english' || source === 'gloss' ? (row.display ?? row.word) : row.word,
      weight: row.weight ?? 0
    }));
  });
  merged.sort((a, b) => b.weight - a.weight);
  if (!merged.length) {
    body.innerHTML = '<tr><td colspan="5" class="dict-empty">三个词库都没有找到词条</td></tr>';
    syncTableHeaderWidth();
    return;
  }
  body.replaceChildren(...merged.map((row, index) => {
    const tr = document.createElement('tr');
    const indexCell = document.createElement('td');
    indexCell.className = 'dict-index-column';
    indexCell.textContent = String(index + 1);
    tr.appendChild(indexCell);
    [SOURCE_LABEL[row.source], row.code, row.word, String(row.weight)].forEach((value) => {
      const td = document.createElement('td');
      td.textContent = value;
      td.addEventListener('mouseenter', () => {
        if (td.scrollWidth > td.clientWidth) td.title = value;
        else td.removeAttribute('title');
      });
      tr.appendChild(td);
    });
    return tr;
  }));
  syncTableHeaderWidth();
}

function downloadExport(content: string, filename: string): void {
  const blob = new Blob([new Uint8Array([0xef, 0xbb, 0xbf]), content], {
    type: 'text/plain;charset=utf-8',
  });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = filename;
  anchor.style.display = 'none';
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 1000);
}

export function setupDictionary(): void {
  hoistOverlay(document.getElementById('dictToast'));
  hoistOverlay(document.getElementById('dictModal'));
  const table = document.querySelector<HTMLElement>('.dict-table-wrap');
  if (table) pager = createDictionaryPager(table, offset => query(offset));
  document.querySelectorAll<HTMLButtonElement>('.dict-tab').forEach((tab) => tab.addEventListener('click', () => {
    document.querySelector('.dict-tab.active')?.classList.remove('active'); tab.classList.add('active');
    dictionary = tab.dataset.dictionary as QueryTarget; updateMode();
  }));
  document.getElementById('dictSearchButton')?.addEventListener('click', () => query());
  document.getElementById('dictSearch')?.addEventListener('keydown', (event) => { if ((event as KeyboardEvent).key === 'Enter') query(); });
  document.getElementById('dictAddButton')?.addEventListener('click', () => openDialog());
  document.getElementById('dictImportButton')?.addEventListener('click', () => {
    (document.getElementById('dictImportFile') as HTMLInputElement | null)?.click();
  });
  document.getElementById('dictImportFile')?.addEventListener('change', async (event) => {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    input.value = '';
    if (!file) return;
    try {
      const content = await readDictionaryFile(file);
      if (!content.trim()) { showToast('文件内容为空', false); return; }
      post('import', { content });
    } catch {
      showToast('读取文件失败', false);
    }
  });
  document.getElementById('dictImportHansButton')?.addEventListener('click', () => {
    (document.getElementById('dictImportHansFile') as HTMLInputElement | null)?.click();
  });
  document.getElementById('dictImportHansFile')?.addEventListener('change', async (event) => {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    input.value = '';
    if (!file) return;
    try {
      const content = await readDictionaryFile(file);
      if (!content.trim()) { showToast('文件内容为空', false); return; }
      post('importHans', { content });
    } catch {
      showToast('读取文件失败', false);
    }
  });
  document.getElementById('dictGlossExportCurrent')?.addEventListener('click', () => {
    const language = glossTabLanguage();
    if (language) post('export', { dictionary: 'gloss', code: language });
  });
  document.querySelectorAll<HTMLButtonElement>('[data-export-gloss]').forEach((button) => {
    button.addEventListener('click', () => {
      const language = button.dataset.exportGloss;
      if (language) post('export', { dictionary: 'gloss', code: language });
    });
  });
  document.querySelectorAll<HTMLButtonElement>('[data-export-dictionary]').forEach((button) => {
    button.addEventListener('click', () => {
      const exportDictionary = button.dataset.exportDictionary as DictionaryType | undefined;
      if (!exportDictionary) return;
      post('export', { dictionary: exportDictionary });
    });
  });
  document.getElementById('dictCancelButton')?.addEventListener('click', closeDialog);
  document.getElementById('dictToastClose')?.addEventListener('click', () => {
    document.getElementById('dictToast')?.classList.remove('visible');
    if (toastTimer !== null) {
      window.clearTimeout(toastTimer);
      toastTimer = null;
    }
  });
  window.addEventListener('resize', syncTableHeaderWidth);
  document.addEventListener('keydown', (event: KeyboardEvent) => {
    if (event.key === 'Escape' && document.getElementById('dictModal')?.classList.contains('open')) {
      event.preventDefault();
      closeDialog();
      (document.activeElement as HTMLElement | null)?.blur();
    }
  });
  document.getElementById('dictSaveButton')?.addEventListener('click', () => {
    const code = (document.getElementById('dictCode') as HTMLInputElement).value.trim();
    const word = (document.getElementById('dictWord') as HTMLInputElement).value.trim();
    const weight = Number((document.getElementById('dictWeight') as HTMLInputElement).value);
    if (dictionary === 'english') post(editing ? 'update' : 'create', {
      word: code, display: word, weight, oldWord: editing?.word, oldDisplay: editing?.display ?? editing?.word,
    });
    else post(editing ? 'update' : 'create', { code, word, weight, oldCode: editing?.code, oldWord: editing?.word });
  });
  onHostMessage('dictionaryResponse', payload => {
    if (!payload.requestId.startsWith('dict-')) return;
    const context = pendingRequests.get(payload.requestId);
    if (!context) return;
    pendingRequests.delete(payload.requestId);
    // 联合查询：三个来源分别回来，全部到齐再合并成一张表。
    if (context.dictionary === 'all') {
      if (!allGroup || !payload.requestId.startsWith(allGroup)) return;
      const source = payload.requestId.slice(allGroup.length + 1) as DictionaryType;
      allBuckets.set(source, payload.ok ? (payload.rows as DictionaryRow[]) : []);
      if (allBuckets.size >= ALL_SOURCES.length) renderMergedRows();
      return;
    }
    if (context.action === 'query' && payload.requestId !== latestQueryId) return;
    if (context.action === 'query' && context.dictionary !== dictionary) return;
    const lastAction = context.action;
    if (lastAction === 'query') {
      if (payload.ok) pager?.update(payload.offset ?? 0, payload.rows.length, payload.hasMore === true);
      else pager?.failed();
    }

    const isImport = lastAction === 'import' || lastAction === 'importHans';
    const isExport = lastAction === 'export';
    if (isExport && payload.ok && typeof payload.content === 'string' && typeof payload.filename === 'string') {
      downloadExport(payload.content, payload.filename);
    }
    showToast(
      payload.message ?? (payload.ok ? '操作成功' : '操作失败'),
      Boolean(payload.ok),
      isImport ? 5600 : 3200,
    );
    if (lastAction === 'query' && payload.ok) renderRows(payload.rows);
    if (payload.ok && lastAction !== 'query' && !isExport && context.dictionary === dictionary) { closeDialog(); if (lastQuery) query(pager?.offset ?? 0); }
  });
  updateMode();
}
