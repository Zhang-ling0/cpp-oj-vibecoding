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
// 【题目编辑器为什么是页内块而不是模态框】
//   题目 JSON 动辄两三百行，模态框里的 textarea 在手机上无法滚动定位；
//   而"表格 + 下方编辑区"在窄屏上仍然是一列，保存按钮也始终在视野内。
//
// 【新建题目的模板来自"现有题目"而不是手写骨架】
//   §4.1 的必填字段有十余个（含嵌套 cases/params），手写的最小骨架一定会漏 ——
//   漏了的后果是管理员第一次保存就吃一屏1001。复制一份现成题目再改 slug/标题，
//   得到的必然是一个能通过校验的起点（要改的地方反而因此更显眼）。
// =============================================================================

import { get, post, put, del, currentUser } from './api.js?v=20261021-1';
import { escapeHtml, toast, renderTopbar, formatLocalTime } from './common.js?v=20261021-1';

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
    editorJson: root.querySelector('#peditor-json'),
    editorError: root.querySelector('#peditor-error'),
    editorCancel: root.querySelector('#peditor-cancel'),
    editorSave: root.querySelector('#peditor-save'),
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
      openEditor(el, 'new', '', { slug: 'new-problem', title: '新题目' });
      return;
    }
    get(`/api/admin/problems/${encodeURIComponent(source.slug)}`).then((reply) => {
      if (!gate(el, reply)) return;
      const json = reply.data.json || {};
      json.slug = 'new-problem';        // 必改：与模板撞名会在保存时吃 1005
      json.title = `新题目（复制自 ${source.slug}，记得改 slug）`;
      openEditor(el, 'new', '', json);
    }).catch((error) => toast(error.message || '无法读取模板题目', 'error'));
  });

  el.problems.addEventListener('click', (event) => {
    const editButton = event.target.closest('[data-edit]');
    if (editButton !== null) {
      const slug = editButton.dataset.edit;
      get(`/api/admin/problems/${encodeURIComponent(slug)}`).then((reply) => {
        if (!gate(el, reply)) return;
        openEditor(el, 'edit', slug, reply.data.json || {});
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

  el.editorCancel.addEventListener('click', () => closeEditor(el));

  el.editorSave.addEventListener('click', () => {
    if (state.saving) return;
    const mode = el.editor.dataset.mode;
    const pathSlug = el.editor.dataset.slug;

    let json;
    try {
      json = JSON.parse(el.editorJson.value);
    } catch (error) {
      showEditorError(el, `JSON 语法错误：${error.message}`);
      return;
    }
    if (json === null || typeof json !== 'object' || Array.isArray(json)) {
      showEditorError(el, '顶层必须是 JSON 对象 {…}');
      return;
    }
    if (typeof json.slug !== 'string' || json.slug === '') {
      showEditorError(el, '缺少 slug 字段（文件名由它决定，只用小写字母/数字/连字符）');
      return;
    }
    showEditorError(el, '');

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
      closeEditor(el);
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

function openEditor(el, mode, slug, json) {
  el.editor.dataset.mode = mode;
  el.editor.dataset.slug = slug;
  el.editorTitle.textContent = mode === 'new' ? '新建题目' : `编辑题目 ${slug}`;
  el.editorSlug.textContent = mode === 'new' ? '' : `problems/${slug}.json`;
  el.editorJson.value = JSON.stringify(json, null, 2);
  showEditorError(el, '');
  el.editor.hidden = false;
  el.editorJson.focus();
  el.editor.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
}

function closeEditor(el) {
  el.editor.hidden = true;
  el.editorJson.value = '';
}

function showEditorError(el, text) {
  el.editorError.textContent = text;
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
