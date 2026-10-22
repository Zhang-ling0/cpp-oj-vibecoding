// =============================================================================
// 运维台 —— SPEC §9 M4 T4.14，阶段 2 由只读概览扩为【四 tab 管理台】
// 规格来源：§6.3.3 管理端点组 / API.md §7（含阶段 2 新增写端点）
//           §7.3 顶栏运维入口（条件渲染由 common.js 完成）
//
// 【D-37 变更（阶段 2 显式修改）】
//   原 D-37 = admin 页面严格只读，写操作只能改 .env 后重启。
//   本阶段：题目 CRUD / 提交删除·重判 / 用户改角色·停用 在页面上可写。
//   未被推翻的部分（这里必须继续遵守）：
//     · 角色的声明式真源仍是 OJ_ADMIN_USERNAMES，页面改角色下次启动会被同步覆盖；
//     · 不能改自己的角色、不能停用自己（服务端 1011，前端再置一道灰）；
//     · 不物理删题（只有 archived 开关），"不删数据"这条决策没变。
//
// 【四个 tab 共用一次闸门】
//   首屏只发 GET /api/admin/overview；1002 → 跳登录、1003 → 提示后跳回题库。
//   之后每个 tab 懒加载：点开哪个才请求哪个（管理台不是高频页面，首屏不该拉三个列表）。
//
// 【题目编辑器：结构化表单，不是裸 JSON】
//   §4.1 的字段有十余个（含嵌套 cases/params），把整份 JSON 塞进 textarea 等于
//   要求管理员背字段名 + 自己盯语法 —— 一次手滑就是保存失败。现改为分区表单：
//   基本信息 / 题面 / 函数与模板 / 用例 / 标签；signature 默认自动同步，
//   高级区（signature 手改、harness_prelude）按需展开。保存时收集表单 → 组 JSON，
//   仍走 { json } 载荷（后端契约不变）。客户端先做一轮必填/形状校验，
//   把"漏填"挡在发请求之前；服务端 §4.1 全量校验仍是最终闸门。
//   页内块而不是模态框：窄屏上保存按钮始终在视野内。
//
// 【新建题目的模板来自"现有题目"而不是手写骨架】
//   必填字段多，手写的最小骨架一定会漏 —— 复制一份现成题目再改 slug/标题，
//   得到的必然是一个能通过校验的起点（要改的地方反而因此更显眼）。
// =============================================================================

import { get, post, put, del, currentUser } from './api.js?v=20261022-1';
import { escapeHtml, toast, renderTopbar, formatLocalTime } from './common.js?v=20261022-1';

const VERDICT_TEXT = {
  AC: '通过', WA: '答案错误', TLE: '超时',
  MLE: '内存超限', RE: '运行错误', CE: '编译错误', JE: '平台错误',
};
const SUB_PAGE_SIZE = 20;

export function initAdminPage(root) {
  const el = {
    topbar: root.querySelector('#topbar'),
    refresh: root.querySelector('#refresh'),
    denied: root.querySelector('#denied'),
    tabs: root.querySelectorAll('[data-tab]'),
    panes: {
      overview: root.querySelector('#pane-overview'),
      problems: root.querySelector('#pane-problems'),
      submissions: root.querySelector('#pane-submissions'),
      users: root.querySelector('#pane-users'),
    },
    sysStats: root.querySelector('#sys-stats'),
    sysRuntime: root.querySelector('#sys-runtime'),
    sysNote: root.querySelector('#sys-note'),
    problemCount: root.querySelector('#problem-count'),
    problemNew: root.querySelector('#problem-new'),
    problems: root.querySelector('#problems'),
    editor: root.querySelector('#problem-editor'),
    editorTitle: root.querySelector('#peditor-title'),
    editorSlug: root.querySelector('#peditor-slug'),
    editorError: root.querySelector('#peditor-error'),
    editorCancel: root.querySelector('#peditor-cancel'),
    editorSave: root.querySelector('#peditor-save'),
    ped: {
      slug: root.querySelector('#ped-slug'),
      title: root.querySelector('#ped-title'),
      difficulty: root.querySelector('#ped-difficulty'),
      tl: root.querySelector('#ped-tl'),
      ml: root.querySelector('#ped-ml'),
      sort: root.querySelector('#ped-sort'),
      featured: root.querySelector('#ped-featured'),
      statement: root.querySelector('#ped-statement'),
      hint: root.querySelector('#ped-hint'),
      method: root.querySelector('#ped-method'),
      ret: root.querySelector('#ped-return'),
      params: root.querySelector('#ped-params'),
      paramAdd: root.querySelector('#ped-param-add'),
      template: root.querySelector('#ped-template'),
      advToggle: root.querySelector('#ped-adv-toggle'),
      adv: root.querySelector('#ped-adv'),
      signature: root.querySelector('#ped-signature'),
      prelude: root.querySelector('#ped-prelude'),
      cases: root.querySelector('#ped-cases'),
      caseAdd: root.querySelector('#ped-case-add'),
      tagsAlgo: root.querySelector('#ped-tags-algo'),
      tagsSource: root.querySelector('#ped-tags-source'),
    },
    submissionCount: root.querySelector('#submission-count'),
    submissions: root.querySelector('#submissions'),
    subPager: root.querySelector('#submission-pager'),
    userCount: root.querySelector('#user-count'),
    users: root.querySelector('#users'),
  };

  const state = {
    tab: 'overview',
    loaded: { overview: false, problems: false, submissions: false, users: false },
    problems: [],
    users: [],
    subPage: 1,
    subTotal: 0,
    me: null,          // currentUser()：仅用于"不能操作自己"的前端置灰
    saving: false,
    // 编辑器工作副本：{ mode, pathSlug, json, signatureDirty }
    // json 是唯一可变源；表单只是它的投影，保存时再序列化回 { json } 载荷。
    editing: null,
  };

  renderTopbar(el.topbar, { active: 'admin' });
  currentUser().then((u) => { state.me = u; }).catch(() => {});

  el.tabs.forEach((button) => {
    button.addEventListener('click', () => activate(el, state, button.dataset.tab));
  });
  el.refresh.addEventListener('click', () => refreshCurrent(el, state));
  bindProblemEditor(el, state);
  bindSubmissionActions(el, state);
  bindUserActions(el, state);

  loadOverview(el, state);
}

// ---- 闸门与 tab 调度 ------------------------------------------------------------

// 返回 true = 可以继续处理业务；false = 已按 1002/1003/其它错误收口，调用方直接停手。
function gate(el, reply) {
  if (reply.code === 1002) {
    location.href = `/login.html?redirect=${encodeURIComponent('/admin.html')}`;
    return false;
  }
  if (reply.code === 1003) {
    el.denied.hidden = false;
    setTimeout(() => location.replace('/list.html'), 1500);
    return false;
  }
  if (reply.code !== 0) {
    toast(reply.message || '管理端点读取失败，请看 logs/app.log', 'error', 6000);
    return false;
  }
  return true;
}

function activate(el, state, name) {
  state.tab = name;
  el.tabs.forEach((button) => {
    const on = button.dataset.tab === name;
    button.classList.toggle('is-on', on);
    button.setAttribute('aria-selected', on ? 'true' : 'false');
  });
  Object.entries(el.panes).forEach(([key, pane]) => { pane.hidden = key !== name; });
  if (!state.loaded[name]) loadTab(el, state, name);
}

function loadTab(el, state, name) {
  if (name === 'overview') return loadOverview(el, state);
  if (name === 'problems') return loadProblems(el, state);
  if (name === 'submissions') return loadSubmissions(el, state);
  return loadUsers(el, state);
}

function refreshCurrent(el, state) {
  el.refresh.disabled = true;
  el.refresh.textContent = '加载中…';
  const promise = loadTab(el, state, state.tab) || Promise.resolve();
  Promise.resolve(promise).finally(() => {
    el.refresh.disabled = false;
    el.refresh.textContent = '刷新';
  });
}

// ---- tab1 概览 -------------------------------------------------------------------

function loadOverview(el, state) {
  return get('/api/admin/overview').then((reply) => {
    if (reply.code === 1002 || reply.code === 1003) {
      gate(el, reply);
      return;
    }
    if (reply.code !== 0) {
      // 概览是首屏：与其它 tab 的 toast 不同，这里要把原因留在页面上，
      // 否则管理员看到的是四块空白而没有任何一条能复制去搜索的信息。
      el.sysStats.innerHTML = `<div class="notice notice--error">${escapeHtml(reply.message || '读取失败')}</div>`;
      toast(reply.message || '管理端点读取失败，请看 logs/app.log', 'error', 6000);
      return;
    }
    state.loaded.overview = true;
    state.users = reply.data.users || [];
    state.loaded.users = true;      // 概览已带全量用户清单，tab4 不必再拉一次
    renderSystem(el, reply.data.system || {});
    renderUsers(el, state);
  }).catch((error) => {
    toast(error.message || '无法连接到服务器', 'error');
  });
}

function renderSystem(el, sys) {
  el.sysStats.innerHTML =
    tile('用户', sys.userCount, `其中管理员 ${sys.adminCount}`) +
    tile('题目', sys.problemCount, `用例 ${sys.caseCount}`) +
    tile('提交', sys.submissionCount, `AC ${sys.acCount} · 失败 ${sys.failedCount}`) +
    tile('未完成提交', sys.judgingCount, 'pending / judging');

  // judgeAlive 单独用 pill 表达而不是一个数字：0 与 不可用是两件事 ——
  // 队列长度 0 / 并发 0 是"空"，未接线是"没有"，两者对用户的行动含义完全不同。
  const alive = sys.judgeAlive === true;
  el.sysRuntime.innerHTML =
    runtime('队列长度', `${sys.queueLength ?? 0}`) +
    runtime('并发评测', `${sys.activeJudges ?? 0}`) +
    '<div class="runtime__item">' +
      '<span class="runtime__label">判题调度器</span>' +
      (alive ? '<span class="pill pill--ok">运行中</span>'
             : '<span class="pill pill--warn">未接线</span>') +
    '</div>';

  // innerHTML 里强调用 <b> 而不是 Markdown 的 ** —— 混用会在界面上露出两个星号
  el.sysNote.innerHTML = alive
    ? ''
    : '判题调度器尚未接线：提交会被拒并返回「判题服务不可用」；' +
      '队列长度与并发数恒为 0 属于<b>预期状态</b>，不是「没有任务在跑」。';
}

function tile(label, value, hint) {
  return '<div class="stat"><b>' + escapeHtml(String(value ?? 0)) + '</b>' +
         `<span>${escapeHtml(label)}</span>` +
         `<span class="stat__hint">${escapeHtml(hint)}</span></div>`;
}

function runtime(label, value) {
  return `<div class="runtime__item"><span class="runtime__label">${escapeHtml(label)}</span>` +
         `<span class="runtime__value mono">${escapeHtml(value)}</span></div>`;
}

// ---- tab2 题目 -------------------------------------------------------------------

function loadProblems(el, state) {
  return get('/api/admin/problems').then((reply) => {
    if (!gate(el, reply)) return;
    state.loaded.problems = true;
    state.problems = reply.data.problems || [];
    renderProblems(el, state);
  }).catch((error) => toast(error.message || '无法连接到服务器', 'error'));
}

function renderProblems(el, state) {
  const list = state.problems;
  el.problemCount.textContent = `共 ${list.length} 道`;
  if (list.length === 0) {
    el.problems.innerHTML =
      '<tr><td colspan="8" class="cell-center muted">题库为空 —— 用「新建题目」写入第一道题</td></tr>';
    return;
  }
  el.problems.innerHTML = list.map((item) => {
    const archived = item.archived === true;
    return '<tr' + (archived ? ' class="is-archived"' : '') + '>' +
      `<td>${escapeHtml(item.slug)}</td>` +
      `<td><b>${escapeHtml(item.title)}</b></td>` +
      `<td>${escapeHtml(item.difficulty)}</td>` +
      `<td class="num mono">${item.caseCount}</td>` +
      `<td>${item.isFeatured ? '是' : '<span class="muted">—</span>'}</td>` +
      `<td>${archived ? '<span class="pill pill--warn">已下线</span>'
                      : '<span class="pill pill--ok">在架</span>'}</td>` +
      `<td class="mono muted">${escapeHtml(formatLocalTime(item.updatedAt))}</td>` +
      '<td>' +
        `<button type="button" class="btn btn--ghost btn--sm" data-edit="${escapeHtml(item.slug)}">编辑</button>` +
        `<button type="button" class="btn btn--ghost btn--sm" data-archive="${escapeHtml(item.slug)}" data-to="${archived ? '0' : '1'}">` +
          (archived ? '上架' : '下线') + '</button>' +
      '</td>' +
    '</tr>';
  }).join('');
}

function bindProblemEditor(el, state) {
  el.problemNew.addEventListener('click', () => {
    // 复制一道现成题目当模板（见文件头：手写最小骨架一定会漏 §4.1 的必填字段）
    const source = state.problems.find((item) => item.archived !== true) || state.problems[0];
    if (source === undefined) {
      openEditor(el, state, 'new', '', minimalDraft());
      return;
    }
    get(`/api/admin/problems/${encodeURIComponent(source.slug)}`).then((reply) => {
      if (!gate(el, reply)) return;
      const json = reply.data.json || {};
      json.slug = 'new-problem';        // 必改：与模板撞名会在保存时吃 1005
      json.title = `新题目（复制自 ${source.slug}，记得改 slug）`;
      openEditor(el, state, 'new', '', json);
    }).catch((error) => toast(error.message || '无法读取模板题目', 'error'));
  });

  el.problems.addEventListener('click', (event) => {
    const editButton = event.target.closest('[data-edit]');
    if (editButton !== null) {
      const slug = editButton.dataset.edit;
      get(`/api/admin/problems/${encodeURIComponent(slug)}`).then((reply) => {
        if (!gate(el, reply)) return;
        openEditor(el, state, 'edit', slug, reply.data.json || {});
      }).catch((error) => toast(error.message || '无法读取题目', 'error'));
      return;
    }
    const archiveButton = event.target.closest('[data-archive]');
    if (archiveButton !== null) {
      const slug = archiveButton.dataset.archive;
      const toArchive = archiveButton.dataset.to === '1';
      const confirmText = toArchive
        ? `下线「${slug}」？它将从列表/详情/提交入口消失（历史提交保留），可随时上架。`
        : `把「${slug}」重新上架？`;
      if (!confirm(confirmText)) return;
      archiveButton.disabled = true;
      post(`/api/admin/problems/${encodeURIComponent(slug)}/archive`, { archived: toArchive })
        .then((reply) => {
          if (!gate(el, reply)) return;
          toast(toArchive ? '已下线归档' : '已重新上架', 'ok');
          return loadProblems(el, state);
        })
        .catch((error) => toast(error.message || '操作失败', 'error'))
        .finally(() => { archiveButton.disabled = false; });
    }
  });

  // 高级区开合：signature / harness_prelude 平时藏起来（多数录题用不到）
  el.ped.advToggle.addEventListener('change', () => {
    el.ped.adv.hidden = !el.ped.advToggle.checked;
  });

  el.ped.paramAdd.addEventListener('click', () => {
    if (!state.editing) return;
    // 先把用例区当前输入收回工作副本 —— 重绘会丢掉 DOM 里未提交的按键
    collectCaseValues(el, state);
    state.editing.json.params.push({ name: '', cpp_type: 'int' });
    // 形参个数变了，每个用例的 args 要跟着补齐/截断（层 ② 第 3 条：一一对应）
    for (const item of state.editing.json.cases) {
      while (item.args.length < state.editing.json.params.length) item.args.push(null);
      item.args.length = state.editing.json.params.length;
    }
    state.editing.signatureDirty = false; // 形参变了，自动签名重新生成
    fillEditorForm(el, state);
  });

  el.ped.cases.addEventListener('click', (event) => {
    if (!state.editing) return;
    const delButton = event.target.closest('[data-del-case]');
    if (delButton !== null) {
      const index = Number(delButton.dataset.delCase);
      collectCaseValues(el, state);
      state.editing.json.cases.splice(index, 1);
      fillEditorForm(el, state);
      return;
    }
    const addAfter = event.target.closest('[data-add-case]');
    if (addAfter !== null) {
      const index = Number(addAfter.dataset.addCase);
      collectCaseValues(el, state);
      const blank = { brief: '', args: state.editing.json.params.map(() => null), expected: null };
      state.editing.json.cases.splice(index + 1, 0, blank);
      fillEditorForm(el, state);
    }
  });

  // 表单 → 工作副本（input/change 事件委托；保存时还会再收集一次兜底）
  el.ped.params.addEventListener('input', (event) => {
    if (!state.editing) return;
    const row = event.target.closest('[data-param-index]');
    if (row === null) return;
    const index = Number(row.dataset.paramIndex);
    const field = event.target.dataset.field;
    if (field === 'name') {
      state.editing.json.params[index].name = event.target.value.trim();
      // 用例行的参数标签要跟上改名；先收回用例输入再重画，避免未失焦的值被冲掉
      collectCaseValues(el, state);
      renderCaseRows(el, state.editing.json);
    } else if (field === 'cpp_type') {
      state.editing.json.params[index].cpp_type = event.target.value;
      state.editing.signatureDirty = false;
      syncSignature(el, state);
      collectCaseValues(el, state);
      renderCaseRows(el, state.editing.json);
    }
  });
  el.ped.params.addEventListener('change', (event) => {
    if (!state.editing) return;
    const row = event.target.closest('[data-param-index]');
    if (row === null) return;
    const index = Number(row.dataset.paramIndex);
    if (event.target.dataset.field === 'cpp_type') {
      state.editing.json.params[index].cpp_type = event.target.value;
      state.editing.signatureDirty = false;
      syncSignature(el, state);
      collectCaseValues(el, state);
      renderCaseRows(el, state.editing.json);
    }
  });
  el.ped.params.addEventListener('click', (event) => {
    if (!state.editing) return;
    const delButton = event.target.closest('[data-del-param]');
    if (delButton === null) return;
    const index = Number(delButton.dataset.delParam);
    if (state.editing.json.params.length <= 1) {
      showEditorError(el, '至少要保留一个形参');
      return;
    }
    collectCaseValues(el, state);
    state.editing.json.params.splice(index, 1);
    for (const item of state.editing.json.cases) {
      item.args.splice(index, 1);
    }
    state.editing.signatureDirty = false;
    fillEditorForm(el, state);
  });

  // 方法名 / 返回类型变化 → 自动重签名（除非已手动改过 signature）
  // 返回类型还会改变 expected 输入形态（int/bool 用原生控件、vector 用 JSON 框），
  // 所以变更后要重画用例区；先收回当前输入避免未失焦的值被冲掉。
  for (const input of [el.ped.method, el.ped.ret]) {
    input.addEventListener('input', () => {
      if (!state.editing) return;
      state.editing.json.method_name = el.ped.method.value.trim();
      state.editing.json.return_type = el.ped.ret.value;
      if (!state.editing.signatureDirty) syncSignature(el, state);
      collectCaseValues(el, state);
      renderCaseRows(el, state.editing.json);
    });
  }
  el.ped.signature.addEventListener('input', () => {
    if (state.editing) state.editing.signatureDirty = true;
  });

  el.editorCancel.addEventListener('click', () => closeEditor(el, state));

  el.editorSave.addEventListener('click', () => {
    if (state.saving || !state.editing) return;
    const json = collectEditorJson(el, state);
    const invalid = validateEditorJson(json);
    if (invalid !== null) {
      showEditorError(el, invalid);
      return;
    }
    showEditorError(el, '');

    const mode = state.editing.mode;
    const pathSlug = state.editing.pathSlug;
    state.saving = true;
    el.editorSave.disabled = true;
    el.editorSave.textContent = '保存中…';

    const request = mode === 'new'
      ? post('/api/admin/problems', { json })
      : put(`/api/admin/problems/${encodeURIComponent(pathSlug)}`, { json });

    request.then((reply) => {
      if (reply.code === 1002 || reply.code === 1003) { gate(el, reply); return; }
      if (reply.code === 1001) {
        // 服务端把 §4.1 的逐条校验结论放在 data.reason 里（format_issues）——
        // 直接展示它，管理员才知道是 cases[2].args 少了一个元素而不是"保存失败"
        showEditorError(el, reply.data.reason || reply.message || '校验失败');
        return;
      }
      if (reply.code === 1005) { showEditorError(el, '同名题目文件已存在，换一个 slug'); return; }
      if (reply.code !== 0) { showEditorError(el, reply.message || '保存失败'); return; }

      toast(mode === 'new' ? '题目已创建并同步入库' : '题目已更新并同步入库', 'ok');
      closeEditor(el, state);
      return loadProblems(el, state);
    }).catch((error) => {
      showEditorError(el, error.message || '无法连接到服务器');
    }).finally(() => {
      state.saving = false;
      el.editorSave.disabled = false;
      el.editorSave.textContent = '保存并同步';
    });
  });
}

// ---- 题目编辑器：表单 ↔ JSON 的投影 ------------------------------------------------

// 最小草稿：一道题库还不存在时的起点（字段齐，内容占位）。仅在题库为空时使用。
function minimalDraft() {
  return {
    slug: 'new-problem',
    title: '新题目',
    difficulty: 'easy',
    statement_md: '',
    hint_md: '',
    signature: '',
    method_name: 'solve',
    return_type: 'int',
    harness_prelude: '',
    code_template: 'class Solution {\npublic:\n    int solve(int x) {\n        \n    }\n};\n',
    time_limit_ms: 3000,
    mem_limit_kb: 131072,
    is_featured: false,
    sort_no: 0,
    tags: { algorithm: [], source: [] },
    params: [{ name: 'x', cpp_type: 'int' }],
    cases: [{ brief: '', args: [0], expected: 0 }],
  };
}

// 归一化服务端/复制来的 JSON：补默认值、按 §4.1 白名单砍掉未知顶层键、
// 保证嵌套形状可被表单安全读写。
function normalizeDraft(raw) {
  const source = (raw !== null && typeof raw === 'object' && !Array.isArray(raw)) ? raw : {};
  // §4.1 只认这些顶层键；多余的原样透传会被服务端 kUnknownField 拒掉，
  // 这里主动丢弃 —— 比"保存时突然 1001"友好。
  const json = {};
  for (const key of [
    'slug', 'title', 'difficulty', 'statement_md', 'hint_md', 'signature',
    'method_name', 'return_type', 'harness_prelude', 'code_template',
    'time_limit_ms', 'mem_limit_kb', 'is_featured', 'sort_no', 'tags',
    'params', 'cases',
  ]) {
    if (source[key] !== undefined) json[key] = source[key];
  }
  json.slug = typeof json.slug === 'string' ? json.slug : '';
  json.title = typeof json.title === 'string' ? json.title : '';
  json.difficulty = ['easy', 'medium', 'hard'].includes(json.difficulty) ? json.difficulty : 'medium';
  json.statement_md = typeof json.statement_md === 'string' ? json.statement_md : '';
  json.hint_md = typeof json.hint_md === 'string' ? json.hint_md : '';
  json.signature = typeof json.signature === 'string' ? json.signature : '';
  json.method_name = typeof json.method_name === 'string' ? json.method_name : '';
  json.return_type = typeof json.return_type === 'string' ? json.return_type : 'int';
  json.harness_prelude = typeof json.harness_prelude === 'string' ? json.harness_prelude : '';
  json.code_template = typeof json.code_template === 'string' ? json.code_template : '';
  json.time_limit_ms = Number.isFinite(Number(json.time_limit_ms)) ? Number(json.time_limit_ms) : 3000;
  json.mem_limit_kb = Number.isFinite(Number(json.mem_limit_kb)) ? Number(json.mem_limit_kb) : 131072;
  json.is_featured = json.is_featured === true;
  json.sort_no = Number.isFinite(Number(json.sort_no)) ? Number(json.sort_no) : 0;

  const tags = (json.tags !== null && typeof json.tags === 'object' && !Array.isArray(json.tags))
    ? json.tags : {};
  const toTagList = (value) => (Array.isArray(value)
    ? value.filter((t) => typeof t === 'string').slice(0, 32)
    : []);
  json.tags = { algorithm: toTagList(tags.algorithm), source: toTagList(tags.source) };

  json.params = (Array.isArray(json.params) ? json.params : [])
    .filter((p) => p !== null && typeof p === 'object')
    .map((p) => ({
      name: typeof p.name === 'string' ? p.name : '',
      cpp_type: typeof p.cpp_type === 'string' ? p.cpp_type : 'int',
    }));

  json.cases = (Array.isArray(json.cases) ? json.cases : [])
    .filter((c) => c !== null && typeof c === 'object')
    .map((c) => ({
      brief: typeof c.brief === 'string' ? c.brief : '',
      args: Array.isArray(c.args) ? c.args.slice(0, json.params.length) : [],
      expected: c.expected === undefined ? null : c.expected,
    }));
  // args 长度对齐形参：多砍少补（补 null → 保存前校验会拦"未填"）
  for (const item of json.cases) {
    while (item.args.length < json.params.length) item.args.push(null);
  }
  return json;
}

function openEditor(el, state, mode, slug, rawJson) {
  const json = normalizeDraft(rawJson);
  state.editing = {
    mode,
    pathSlug: slug,
    json,
    // 已有题目且 signature 非空：视为"题库里定稿的签名"，只有动了方法/形参才重生成。
    // 新建/空 signature：打开即生成一份，免得管理员对着空框发呆。
    signatureDirty: json.signature !== '',
  };
  el.editor.dataset.mode = mode;
  el.editor.dataset.slug = slug;
  el.editorTitle.textContent = mode === 'new' ? '新建题目' : `编辑题目 ${slug}`;
  el.editorSlug.textContent = mode === 'new' ? '' : `problems/${slug}.json`;
  el.ped.slug.disabled = mode === 'edit'; // 编辑时锁 slug：路径不一致会被服务端 1001 拒
  if (json.signature === '') syncSignature(el, state);
  fillEditorForm(el, state);
  showEditorError(el, '');
  el.editor.hidden = false;
  el.editor.scrollIntoView({ behavior: 'smooth', block: 'start' });
}

function closeEditor(el, state) {
  el.editor.hidden = true;
  state.editing = null;
  el.ped.params.innerHTML = '';
  el.ped.cases.innerHTML = '';
}

function showEditorError(el, text) {
  el.editorError.textContent = text;
}

// 由 signature 模板自动重生成。手动改过 signature 后不再调用（signatureDirty=true）。
function syncSignature(el, state) {
  if (!state.editing) return;
  const j = state.editing.json;
  const params = j.params
    .map((p) => `${p.cpp_type}${p.name ? ` ${p.name}` : ''}`)
    .join(', ');
  j.signature = `class Solution {\npublic:\n    ${j.return_type || 'int'} ${j.method_name || 'solve'}(${params});\n};\n`;
  el.ped.signature.value = j.signature;
}

// 工作副本 → 表单控件（打开编辑器、增删形参/用例后整页刷新时调用）。
function fillEditorForm(el, state) {
  if (!state.editing) return;
  const j = state.editing.json;

  el.ped.slug.value = j.slug;
  el.ped.title.value = j.title;
  el.ped.difficulty.value = j.difficulty;
  el.ped.tl.value = String(j.time_limit_ms);
  el.ped.ml.value = String(j.mem_limit_kb);
  el.ped.sort.value = String(j.sort_no);
  el.ped.featured.checked = j.is_featured;
  el.ped.statement.value = j.statement_md;
  el.ped.hint.value = j.hint_md;
  el.ped.method.value = j.method_name;
  el.ped.ret.value = j.return_type;
  el.ped.template.value = j.code_template;
  el.ped.signature.value = j.signature;
  el.ped.prelude.value = j.harness_prelude;
  el.ped.tagsAlgo.value = j.tags.algorithm.join(', ');
  el.ped.tagsSource.value = j.tags.source.join(', ');

  renderParamRows(el, j);
  renderCaseRows(el, j);
}

// 返回类型 → 用例值编辑器的输入形态。
// int/bool 用原生控件（键盘友好）；string 用 text；vector 用 JSON 数组文本框。
function scalarKindOf(cppType) {
  const t = String(cppType || '').replace(/\s+/g, '');
  if (t === 'int' || t === 'bool' || t === 'std::string' || t === 'string') return t;
  if (t.startsWith('std::vector') || t.startsWith('vector')) return 'vector';
  return 'json';
}

function renderParamRows(el, j) {
  if (j.params.length === 0) {
    el.ped.params.innerHTML = '<p class="muted">还没有形参，点下方添加。</p>';
    return;
  }
  const typeOptions = ['int', 'std::string', 'std::vector<int>&'];
  el.ped.params.innerHTML = j.params.map((p, i) => {
    const known = typeOptions.includes(p.cpp_type);
    const options = typeOptions.map((t) =>
      `<option value="${escapeHtml(t)}"${p.cpp_type === t ? ' selected' : ''}>${escapeHtml(t)}</option>`
    ).join('') + (known ? '' :
      `<option value="${escapeHtml(p.cpp_type)}" selected>${escapeHtml(p.cpp_type)}（自定义）</option>`);
    return '<div class="peditor__param" data-param-index="' + i + '">' +
      `<input class="field__input" data-field="name" type="text" value="${escapeHtml(p.name)}"` +
      ` placeholder="参数名" spellcheck="false" autocomplete="off">` +
      `<select class="field__input" data-field="cpp_type">${options}</select>` +
      `<button type="button" class="btn btn--ghost btn--sm" data-del-param="${i}"` +
      ` title="删除该形参">✕</button>` +
    '</div>';
  }).join('');
}

// 用例行：brief + 每形参一个值输入 + expected + 上移/删除。
// 值的序列化按形参类型走：int→number、bool→true/false、string→带引号文本、
// vector→JSON 数组文本。收集时再 parse，避免"看着是数组存进去是字符串"。
function renderCaseRows(el, j) {
  if (j.cases.length === 0) {
    el.ped.cases.innerHTML = '<p class="muted">还没有用例，点下方添加。至少要有一个。</p>';
    return;
  }
  el.ped.cases.innerHTML = j.cases.map((item, ci) => {
    const argInputs = j.params.map((p, pi) => {
      const kind = scalarKindOf(p.cpp_type);
      const label = escapeHtml(p.name || `arg${pi}`);
      const typeHint = escapeHtml(p.cpp_type);
      const value = serializeArgValue(item.args[pi], kind);
      const ph = placeholderForKind(kind);
      if (kind === 'bool') {
        const options = ['true', 'false'].map((v) =>
          `<option value="${v}"${value === v ? ' selected' : ''}>${v}</option>`
        ).join('');
        return `<label class="peditor__arg"><span>${label}<i>${typeHint}</i></span>` +
          `<select class="field__input" data-case="${ci}" data-arg="${pi}">${options}</select></label>`;
      }
      const inputClass = kind === 'vector' || kind === 'json'
        ? 'field__input peditor__arg-json' : 'field__input';
      return `<label class="peditor__arg"><span>${label}<i>${typeHint}</i></span>` +
        `<input class="${inputClass}" data-case="${ci}" data-arg="${pi}" type="text"` +
        ` value="${escapeHtml(value)}" placeholder="${escapeHtml(ph)}" spellcheck="false"` +
        ` autocomplete="off"></label>`;
    }).join('');

    const expKind = scalarKindOf(j.return_type);
    const expValue = serializeArgValue(item.expected, expKind);
    const expInput = expKind === 'bool'
      ? `<select class="field__input" data-case="${ci}" data-exp="1">` +
        ['true', 'false'].map((v) =>
          `<option value="${v}"${expValue === v ? ' selected' : ''}>${v}</option>`
        ).join('') + '</select>'
      : `<input class="field__input${expKind === 'vector' || expKind === 'json' ? ' peditor__arg-json' : ''}"` +
        ` data-case="${ci}" data-exp="1" type="text" value="${escapeHtml(expValue)}"` +
        ` placeholder="${escapeHtml(placeholderForKind(expKind))}" spellcheck="false" autocomplete="off">`;

    return '<div class="peditor__case">' +
      '<div class="peditor__case-head">' +
        `<b>用例 ${ci + 1}</b>` +
        `<button type="button" class="btn btn--ghost btn--sm" data-add-case="${ci}">在此后插入</button>` +
        `<button type="button" class="btn btn--ghost btn--sm" data-del-case="${ci}">删除</button>` +
      '</div>' +
      '<div class="peditor__case-body">' +
        `<label class="peditor__arg peditor__arg--brief"><span>说明</span>` +
        `<input class="field__input" data-case="${ci}" data-brief="1" type="text"` +
        ` value="${escapeHtml(item.brief)}" placeholder="基础用例" autocomplete="off"></label>` +
        argInputs +
        `<label class="peditor__arg peditor__arg--expected"><span>expected*<i>${escapeHtml(j.return_type)}</i></span>` +
        expInput + '</label>' +
      '</div>' +
    '</div>';
  }).join('');
}

function placeholderForKind(kind) {
  if (kind === 'int') return '0';
  if (kind === 'bool') return 'true';
  if (kind === 'std::string') return 'hello';
  if (kind === 'vector') return '[1, 2, 3]';
  return 'JSON 值';
}

// 工作副本里的 JSON 值 → 表单控件的字符串形态。
function serializeArgValue(value, kind) {
  if (value === null || value === undefined) return '';
  if (kind === 'bool') return value ? 'true' : 'false';
  if (kind === 'int') return String(value);
  if (kind === 'std::string') return typeof value === 'string' ? value : String(value);
  // vector / json：JSON 文本（[1,2,3] / "abc" / 42 都能表达）
  try { return JSON.stringify(value); } catch (_) { return ''; }
}

// 表单控件 → 工作副本（input 委托之外，保存前再兜底收集一次）。
function collectEditorJson(el, state) {
  if (!state.editing) return null;
  const j = state.editing.json;

  j.slug = el.ped.slug.value.trim();
  j.title = el.ped.title.value.trim();
  j.difficulty = el.ped.difficulty.value;
  j.time_limit_ms = Number(el.ped.tl.value);
  j.mem_limit_kb = Number(el.ped.ml.value);
  j.sort_no = Number(el.ped.sort.value);
  j.is_featured = el.ped.featured.checked;
  j.statement_md = el.ped.statement.value;
  j.hint_md = el.ped.hint.value;
  j.method_name = el.ped.method.value.trim();
  j.return_type = el.ped.ret.value;
  j.code_template = el.ped.template.value;
  j.signature = el.ped.signature.value;
  j.harness_prelude = el.ped.prelude.value;
  j.tags = {
    algorithm: splitTags(el.ped.tagsAlgo.value),
    source: splitTags(el.ped.tagsSource.value),
  };

  // 形参：从 DOM 读（用户可能刚敲完还没失焦）
  const paramRows = el.ped.params.querySelectorAll('[data-param-index]');
  if (paramRows.length > 0) {
    j.params = Array.from(paramRows).map((row) => ({
      name: row.querySelector('[data-field="name"]').value.trim(),
      cpp_type: row.querySelector('[data-field="cpp_type"]').value,
    }));
  }

  collectCaseValues(el, state);
  return j;
}

// 仅收集用例区（重绘 params/cases 前也要调，否则未失焦的输入会被冲掉）。
function collectCaseValues(el, state) {
  if (!state.editing) return;
  const j = state.editing.json;
  const draftCases = j.cases.map((item) => ({
    brief: item.brief, args: [...item.args], expected: item.expected,
  }));
  el.ped.cases.querySelectorAll('[data-case]').forEach((input) => {
    const ci = Number(input.dataset.case);
    if (!Number.isInteger(ci) || draftCases[ci] === undefined) return;
    const raw = input.value;
    if (input.dataset.brief === '1') {
      draftCases[ci].brief = raw;
      return;
    }
    if (input.dataset.exp === '1') {
      draftCases[ci].expected = parseArgByInput(input, raw, j.return_type);
      return;
    }
    const pi = Number(input.dataset.arg);
    if (!Number.isInteger(pi) || draftCases[ci].args[pi] === undefined) return;
    const cppType = j.params[pi] ? j.params[pi].cpp_type : 'int';
    draftCases[ci].args[pi] = parseArgByInput(input, raw, cppType);
  });
  j.cases = draftCases;
}

function parseArgByInput(input, raw, cppType) {
  const kind = scalarKindOf(cppType);
  const text = raw.trim();
  if (text === '') return null;
  if (kind === 'bool') return text === 'true';
  if (kind === 'int') {
    const n = Number(text);
    return Number.isInteger(n) ? n : null;
  }
  if (kind === 'std::string') return raw; // 字符串允许前导/尾随空格，原样保留
  try { return JSON.parse(text); } catch (_) { return null; }
}

function splitTags(text) {
  return text.split(/[,，]/).map((t) => t.trim()).filter((t) => t !== '').slice(0, 32);
}

// 客户端预检：把"漏填/形状明显不对"挡在发请求之前。
// 返回 null = 通过；否则返回给管理员看的一句话。
// 注意这只是 UX 层，不是安全层 —— 服务端 §4.1 全量校验仍会再跑一遍。
function validateEditorJson(json) {
  if (json === null || typeof json !== 'object') return '表单状态异常，请关闭编辑器重试';
  if (json.slug === '') return '请填写 slug（小写字母/数字/连字符）';
  if (!/^[a-z0-9]+(-[a-z0-9]+)*$/.test(json.slug)) {
    return 'slug 只能用小写字母、数字和连字符（不能以连字符开头/结尾）';
  }
  if (json.title === '') return '请填写标题';
  if (json.statement_md.trim() === '') return '请填写题目描述';
  if (json.method_name === '') return '请填写方法名';
  if (json.code_template.trim() === '') return '请填写代码模板';
  if (json.params.length === 0) return '至少要有一个形参';
  for (let i = 0; i < json.params.length; i++) {
    if (json.params[i].name === '') return `形参 ${i + 1} 缺少参数名`;
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(json.params[i].name)) {
      return `形参 ${i + 1} 的名字不是合法标识符：${json.params[i].name}`;
    }
  }
  for (let i = 0; i < json.params.length; i++) {
    const dup = json.params.findIndex((p, k) => k !== i && p.name === json.params[i].name);
    if (dup !== -1) return `形参名重复：${json.params[i].name}`;
  }
  if (json.cases.length === 0) return '至少要有一个测试用例';
  for (let i = 0; i < json.cases.length; i++) {
    const item = json.cases[i];
    if (item.args.length !== json.params.length) {
      return `用例 ${i + 1} 的参数个数与形参不一致`;
    }
    for (let k = 0; k < item.args.length; k++) {
      if (item.args[k] === null || item.args[k] === undefined) {
        return `用例 ${i + 1} 的第 ${k + 1} 个参数未填写或不是合法值`;
      }
    }
    if (item.expected === null || item.expected === undefined) {
      return `用例 ${i + 1} 缺少 expected`;
    }
  }
  return null;
}

// ---- tab3 提交 -------------------------------------------------------------------

function loadSubmissions(el, state) {
  const query = `?page=${state.subPage}&size=${SUB_PAGE_SIZE}`;
  return get('/api/admin/submissions' + query).then((reply) => {
    if (!gate(el, reply)) return;
    state.loaded.submissions = true;
    state.subTotal = reply.data.total || 0;
    state.subPage = reply.data.page || 1;
    renderSubmissions(el, state, reply.data.list || []);
  }).catch((error) => toast(error.message || '无法连接到服务器', 'error'));
}

function renderSubmissions(el, state, list) {
  el.submissionCount.textContent = state.subTotal === 0 ? '' : `共 ${state.subTotal} 条`;
  if (list.length === 0) {
    el.submissions.innerHTML =
      '<tr><td colspan="8" class="cell-center muted">还没有提交记录</td></tr>';
    el.subPager.innerHTML = '';
    return;
  }
  el.submissions.innerHTML = list.map((item) => {
    const verdict = item.verdict
      ? `<span class="verdict verdict--${escapeHtml(item.verdict.toLowerCase())}">` +
        `${escapeHtml(item.verdict)} ${escapeHtml(VERDICT_TEXT[item.verdict] || '')}</span>`
      : '<span class="muted">—</span>';
    const busy = item.status === 'pending' || item.status === 'judging';
    return '<tr>' +
      `<td class="num">${item.id}</td>` +
      `<td>${escapeHtml(item.username)}</td>` +
      `<td>${escapeHtml(item.problemTitle)} <span class="muted mono">${escapeHtml(item.problemSlug)}</span></td>` +
      `<td class="mono">${escapeHtml(item.language)}</td>` +
      `<td>${escapeHtml(item.status)}</td>` +
      `<td>${verdict}</td>` +
      `<td class="mono muted">${escapeHtml(formatLocalTime(item.createdAt))}</td>` +
      '<td>' +
        `<button type="button" class="btn btn--ghost btn--sm" data-rejudge="${item.id}"${busy ? ' disabled title="评测进行中，结束后才能重判"' : ''}>重判</button>` +
        `<button type="button" class="btn btn--ghost btn--sm" data-del="${item.id}"${busy ? ' disabled title="评测进行中，结束后才能删除"' : ''}>删除</button>` +
      '</td>' +
    '</tr>';
  }).join('');
  renderPager(el, state);
}

function renderPager(el, state) {
  const pages = Math.max(1, Math.ceil(state.subTotal / SUB_PAGE_SIZE));
  if (pages <= 1) { el.subPager.innerHTML = ''; return; }

  // 页码折叠：只显示 1 / 末页 / 当前页 ±1，其余压成一个 "…"。
  // 全量渲染 200 个按钮没有意义（没人会点第 137 页），而不折叠又会把按钮行撑出表格宽度。
  const nums = [];
  for (let n = 1; n <= pages; n++) {
    if (n === 1 || n === pages || Math.abs(n - state.subPage) <= 1) nums.push(n);
    else if (nums[nums.length - 1] !== null) nums.push(null);
  }
  const numHtml = nums.map((n) => (n === null
    ? '<span class="pager__gap">…</span>'
    : `<button type="button" class="pager__btn${n === state.subPage ? ' is-on' : ''}" data-page="${n}"` +
      `${n === state.subPage ? ' aria-current="page"' : ''}>${n}</button>`)).join('');
  const edge = (n, text, disabled) =>
    `<button type="button" class="pager__btn" data-page="${n}"${disabled ? ' disabled' : ''}>${text}</button>`;

  el.subPager.innerHTML =
    edge(state.subPage - 1, '上一页', state.subPage <= 1) + numHtml +
    edge(state.subPage + 1, '下一页', state.subPage >= pages) +
    `<span class="pager__info">第 ${state.subPage} / ${pages} 页</span>`;

  el.subPager.querySelectorAll('[data-page]').forEach((button) => {
    button.addEventListener('click', () => {
      const next = Number(button.dataset.page);
      if (!Number.isInteger(next) || next === state.subPage || next < 1 || next > pages) return;
      state.subPage = next;
      loadSubmissions(el, state);
    });
  });
}

function bindSubmissionActions(el, state) {
  el.submissions.addEventListener('click', (event) => {
    const delButton = event.target.closest('[data-del]');
    if (delButton !== null) {
      const id = Number(delButton.dataset.del);
      if (!confirm(`删除提交 #${id}？会连带删除它的逐用例结果与编译诊断，不可恢复。`)) return;
      delButton.disabled = true;
      del(`/api/admin/submissions/${id}`).then((reply) => {
        if (!gate(el, reply)) return;
        toast(reply.data.message || '提交记录已删除', 'ok');
        return loadSubmissions(el, state);
      }).catch((error) => toast(error.message || '删除失败', 'error'))
        .finally(() => { delButton.disabled = false; });
      return;
    }
    const rejudgeButton = event.target.closest('[data-rejudge]');
    if (rejudgeButton !== null) {
      const id = Number(rejudgeButton.dataset.rejudge);
      rejudgeButton.disabled = true;
      post(`/api/admin/submissions/${id}/rejudge`).then((reply) => {
        // 1009 = judge 未接线（服务端【不改库】就拒绝），1011 = 正在评测中
        if (reply.code === 1002 || reply.code === 1003) { gate(el, reply); return; }
        if (reply.code !== 0) { toast(reply.message || '重判失败', 'error', 6000); return; }
        toast(`#${id} 已重新入队`, 'ok');
        return loadSubmissions(el, state);
      }).catch((error) => toast(error.message || '重判失败', 'error'))
        .finally(() => { rejudgeButton.disabled = false; });
    }
  });
}

// ---- tab4 用户 -------------------------------------------------------------------

function loadUsers(el, state) {
  // 用户清单与概览同源（同一个 GET 返回 users[]）：概览已加载过就直接渲染，
  // 不为"新鲜度"再发一次相同的请求 —— 概览/用户两份数据才会出现口径打架。
  if (state.loaded.overview) {
    renderUsers(el, state);
    return Promise.resolve();
  }
  return loadOverview(el, state);
}

function renderUsers(el, state) {
  const users = state.users;
  el.userCount.textContent = `共 ${users.length} 人`;
  if (users.length === 0) {
    el.users.innerHTML = '<tr><td colspan="13" class="cell-center muted">还没有用户</td></tr>';
    return;
  }
  el.users.innerHTML = users.map((user) => rowHtml(user, state)).join('');
}

function rowHtml(user, state) {
  const isAdmin = user.role === 'admin';
  const isMe = state.me !== null && state.me !== undefined && state.me.username === user.username;
  const rate = user.submissionCount > 0
    ? `${Math.round((user.acCount / user.submissionCount) * 100)}%`
    : '—';
  // 最近一条还在排队时 lastVerdict 为 null：不回退到上一条 AC（API.md §7.1 明确）
  const verdict = user.lastVerdict
    ? `<span class="verdict verdict--${escapeHtml(user.lastVerdict.toLowerCase())}">` +
      `${escapeHtml(user.lastVerdict)} ${escapeHtml(VERDICT_TEXT[user.lastVerdict] || '')}</span>`
    : '<span class="muted">—</span>';
  const disabled = user.disabled === true;

  return '<tr' + (disabled ? ' class="is-disabled"' : '') + '>' +
    `<td><b>${escapeHtml(user.username)}</b>${isMe ? ' <span class="muted">（你）</span>' : ''}</td>` +
    `<td>${isAdmin ? '<span class="badge badge--admin">管理员</span>' : '<span class="muted">普通用户</span>'}</td>` +
    `<td>${disabled ? '<span class="pill pill--warn">已停用</span>' : '<span class="pill pill--ok">正常</span>'}</td>` +
    `<td class="mono muted">${escapeHtml(formatLocalTime(user.createdAt))}</td>` +
    `<td class="num mono">${user.submissionCount}</td>` +
    `<td class="num mono">${user.acCount}</td>` +
    `<td class="num mono">${escapeHtml(rate)}</td>` +
    `<td class="num mono">${user.solved}</td>` +
    `<td class="num mono">${user.attempted}</td>` +
    `<td class="num mono">${user.favorites}</td>` +
    `<td class="mono muted">${user.lastSubmitAt ? escapeHtml(formatLocalTime(user.lastSubmitAt)) : '—'}</td>` +
    `<td>${verdict}</td>` +
    '<td>' +
      `<button type="button" class="btn btn--ghost btn--sm" data-role="${escapeHtml(user.username)}"` +
        ` data-to="${isAdmin ? 'user' : 'admin'}"${isMe ? ' disabled title="不能修改自己的角色"' : ''}>` +
        (isAdmin ? '降为普通' : '升为管理员') + '</button>' +
      `<button type="button" class="btn btn--ghost btn--sm" data-disable="${escapeHtml(user.username)}"` +
        ` data-to="${disabled ? '0' : '1'}"${isMe && !disabled ? ' disabled title="不能停用自己"' : ''}>` +
        (disabled ? '恢复' : '停用') + '</button>' +
    '</td>' +
  '</tr>';
}

function bindUserActions(el, state) {
  el.users.addEventListener('click', (event) => {
    const roleButton = event.target.closest('[data-role]');
    if (roleButton !== null) {
      const username = roleButton.dataset.role;
      const role = roleButton.dataset.to;
      const confirmText = role === 'admin'
        ? `把 ${username} 升为管理员？（.env 的 OJ_ADMIN_USERNAMES 在下次启动时会覆盖这次改动）`
        : `把 ${username} 降为普通用户？（若它在 OJ_ADMIN_USERNAMES 名单里，下次启动会被重新提权）`;
      if (!confirm(confirmText)) return;
      roleButton.disabled = true;
      post(`/api/admin/users/${encodeURIComponent(username)}/role`, { role })
        .then((reply) => {
          if (!gate(el, reply)) return;
          toast(`${username} → ${role === 'admin' ? '管理员' : '普通用户'}`, 'ok');
          return loadOverview(el, state);
        })
        .catch((error) => toast(error.message || '改角色失败', 'error'))
        .finally(() => { roleButton.disabled = false; });
      return;
    }

    const disableButton = event.target.closest('[data-disable]');
    if (disableButton !== null) {
      const username = disableButton.dataset.disable;
      const toDisable = disableButton.dataset.to === '1';
      const confirmText = toDisable
        ? `停用 ${username}？它的全部登录会话会被立即清空，且无法再次登录。`
        : `恢复 ${username} 的登录权限？`;
      if (!confirm(confirmText)) return;
      disableButton.disabled = true;
      post(`/api/admin/users/${encodeURIComponent(username)}/disable`, { disabled: toDisable })
        .then((reply) => {
          if (!gate(el, reply)) return;
          toast(toDisable ? '已停用（会话已清空）' : '已恢复登录权限', 'ok');
          return loadOverview(el, state);
        })
        .catch((error) => toast(error.message || '操作失败', 'error'))
        .finally(() => { disableButton.disabled = false; });
    }
  });
}
