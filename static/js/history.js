// =============================================================================
// 「我的」页 —— SPEC §9 M4 T4.10 [D-39] / T4.12 / T4.16 / T4.17 / T4.23
// 规格来源：§7.1「我的」页三 tab 线框 / §7.3 关键交互 / §7.4 空态与错误态
//           §6.3 GET /api/submissions / DELETE /api/submissions/{id}
//                POST /api/submissions/{id}/retry / POST /api/auth/password
//
// 【三个 tab 共用一次 overview 请求】(AC-24)
//   `overview.js` 的 loadOverview() 是模块级 Promise 缓存，
//   切 tab / tab1 翻页都不重取 —— 否则 DevTools 里会出现多条一模一样的
//   /api/me/overview，而 AC-24 的验收恰恰是"不许有第二条"。
//
// 【重跑按钮按【列表已有的字段】算 canRetry，不再请求详情】
//   服务端 `can_retry = (status == 'failed' && verdict == 'JE')`（submission_api.cpp:294），
//   而列表响应里 status 与 verdict 都在，所以前端能算出**同一个值**。
//   为此多发一次详情请求，是"每次渲染一行就多一次往返"的典型写法。
//   ⚠️ 规则若在服务端变了，前端这条判断会与之漂移 —— 改服务端时要回来改这一行，
//   这条耦合是**故意保留**的：它省掉的是列表页每一次翻页的 N 次请求。
//
// 【删除/重跑按 [D-58]：重跑新增一条记录，不覆盖原记录】
//   成功后重取第一页：总数变了、行序也变了，就地改一行反而更容易让人误以为"它被替换了"。
//
// 【改密的本地校验与登录页同一条约定】
//   不通过就不发请求；旧密码错时**新密码框不清空**（§7.4：便于只改旧密码后重试）。
// =============================================================================

import { get, post, del, logout as apiLogout, currentUser } from './api.js?v=20261015-5';
import { escapeHtml, toast, renderTopbar, relativeTime, formatLocalTime, redirectToLogin } from './common.js?v=20261015-5';
import { renderProgressPanel } from './overview.js?v=20261015-5';

const PAGE_SIZE = 20;                                   // [D-61] 默认 20 条/页
const PASSWORD_MIN = 6;
const PASSWORD_MAX = 64;                                // 与 auth_api.h 的常量一致
const VERDICT_TEXT = {
  AC: '通过', WA: '答案错误', TLE: '超时',
  MLE: '内存超限', RE: '运行错误', CE: '编译错误', JE: '平台错误',
};

export function initHistoryPage(root) {
  const el = {
    topbar: root.querySelector('#topbar'),
    sub: root.querySelector('#mine-sub'),
    tabs: root.querySelectorAll('[data-tab]'),
    panes: {
      records: root.querySelector('#pane-records'),
      progress: root.querySelector('#pane-progress'),
      account: root.querySelector('#pane-account'),
    },
    problem: root.querySelector('#f-problem'),
    verdict: root.querySelector('#f-verdict'),
    subTotal: root.querySelector('#sub-total'),
    rows: root.querySelector('#sub-rows'),
    pager: root.querySelector('#sub-pager'),
    username: root.querySelector('#acc-username'),
    role: root.querySelector('#acc-role'),
    pwForm: root.querySelector('#pw-form'),
    oldPw: root.querySelector('#old-password'),
    newPw: root.querySelector('#new-password'),
    newPw2: root.querySelector('#new-password2'),
    pwSubmit: root.querySelector('#pw-submit'),
    logout: root.querySelector('#logout'),
  };
  const errSlots = {
    oldPassword: root.querySelector('#old-password-error'),
    newPassword: root.querySelector('#new-password-error'),
    newPassword2: root.querySelector('#new-password2-error'),
  };

  renderTopbar(el.topbar, { active: 'me' });

  // 未登录直连本页：整页不可看 ⇒ 跳登录并带 redirect（D-43）
  currentUser().then((user) => {
    if (user === null) { redirectToLogin(); return; }
    el.sub.textContent = `${user.username} 的提交记录、掌握分布与账户设置`;
    if (user.role === 'admin') {
      el.role.innerHTML = '<span class="badge badge--admin">管理员</span>';
    } else {
      // 普通用户不渲染角色徽标（D-41）；这里写"普通用户"是因为它是**只读字段的取值**，
      // 不是顶栏那种"每个用户都无信息量的徽标"
      el.role.textContent = '普通用户';
    }
    el.username.textContent = user.username;
  });

  const state = { page: 1, total: 0, filterProblem: '', filterVerdict: '' };

  bindTabs(root, el);
  bindFilters(el, state);
  bindRows(el, state);
  bindPassword(el, errSlots);
  bindLogout(el);

  loadProblemOptions(el);
  loadSubmissions(el, state);
}

// ---- tab 路由（hash，可分享 / 刷新保持）--------------------------------------------

function bindTabs(root, el) {
  const show = (name) => {
    for (const tab of el.tabs) {
      const on = tab.dataset.tab === name;
      tab.classList.toggle('is-on', on);
      tab.setAttribute('aria-selected', on ? 'true' : 'false');
      el.panes[tab.dataset.tab].hidden = !on;
    }
    // 切 tab 只改 hash，不重新请求：overview 已缓存，切 tab 不该产生网络请求
    if (location.hash !== `#${name}`) location.hash = name;
    // 掌握分布【首次】切到才渲染：overview.js 的模块级缓存保证整个页面
    // （无论切几次 tab、tab1 翻多少页）最多只发出一条 /api/me/overview（AC-24）。
    // 不在挂载时就渲染，是因为没人打开的 tab 不该替用户产生这次请求。
    if (name === 'progress' && !progressRendered) {
      progressRendered = true;
      renderProgressPanel(el.panes.progress);
    }
  };
  let progressRendered = false;
  for (const tab of el.tabs) tab.addEventListener('click', () => show(tab.dataset.tab));
  window.addEventListener('hashchange', () => show(hashTab()));

  const initial = hashTab();
  show(initial);
  // tab2 / tab3 的数据在挂载时已各取过一次（overview 缓存 / auth/me 缓存），
  // 不因初始 tab 是哪个而重发
}

function hashTab() {
  const name = location.hash.replace('#', '');
  return ['records', 'progress', 'account'].includes(name) ? name : 'records';
}

function bindFilters(el, state) {
  el.problem.addEventListener('change', () => {
    state.filterProblem = el.problem.value;
    state.page = 1;
    loadSubmissions(el, state);
  });
  el.verdict.addEventListener('change', () => {
    state.filterVerdict = el.verdict.value;
    state.page = 1;
    loadSubmissions(el, state);
  });
}

/** 题目下拉：一次取 100 条（服务端 size 上限 100）。50 题量级下一次取全量最省事。 */
function loadProblemOptions(el) {
  get('/api/problems', { size: 100, sort: 'default' }).then((reply) => {
    if (reply.code !== 0) return;
    for (const item of reply.data.list || []) {
      const option = document.createElement('option');
      option.value = item.slug;
      option.textContent = item.title;
      el.problem.appendChild(option);
    }
  });
}

// ---- tab1 提交记录 ---------------------------------------------------------------

function loadSubmissions(el, state) {
  el.rows.innerHTML = '<tr><td colspan="7" class="cell-center muted">加载中…</td></tr>';
  get('/api/submissions', {
    problemSlug: state.filterProblem,
    verdict: state.filterVerdict,
    page: state.page,
    size: PAGE_SIZE,
  }).then((reply) => {
    if (reply.code === 1002) { redirectToLogin(); return; }
    if (reply.code !== 0) {
      el.rows.innerHTML = `<tr><td colspan="7" class="cell-center muted">${escapeHtml(reply.message || '读取失败')}</td></tr>`;
      return;
    }
    const list = reply.data.list || [];
    state.page = reply.data.page || 1;      // 跟着服务端回显，删完最后一条不会卡在空页
    state.total = reply.data.total || 0;

    el.subTotal.textContent = state.total === 0 ? '' : `共 ${state.total} 条`;
    if (list.length === 0) {
      el.rows.innerHTML = state.total === 0 && state.filterProblem === '' && state.filterVerdict === ''
        ? '<tr><td colspan="7"><div class="empty"><p>还没有提交记录</p>' +
          '<a class="btn" href="/list.html">去刷题</a></div></td></tr>'
        : '<tr><td colspan="7"><div class="empty"><p>没有匹配的提交记录</p></div></td></tr>';
      el.pager.innerHTML = '';
      return;
    }
    el.rows.innerHTML = list.map(rowHtml).join('');
    renderPager(el, state);
  }).catch((error) => {
    el.rows.innerHTML = `<tr><td colspan="7" class="cell-center muted">${escapeHtml(error.message || '无法连接到服务器')}</td></tr>`;
  });
}

function rowHtml(item) {
  const verdict = item.verdict
    ? `<span class="verdict verdict--${escapeHtml(item.verdict.toLowerCase())}">${escapeHtml(item.verdict)} ${VERDICT_TEXT[item.verdict] || ''}</span>`
    : `<span class="verdict verdict--none">${item.status === 'failed' ? '平台错误' : '评测中'}</span>`;
  const local = formatLocalTime(item.createdAt);
  return `<tr class="subrow" data-id="${item.id}" tabindex="0">` +
    `<td><span title="${escapeHtml(local)}">${escapeHtml(relativeTime(item.createdAt))}</span></td>` +
    `<td><a href="/problem.html?slug=${encodeURIComponent(item.problemSlug)}">${escapeHtml(item.problemTitle)}</a></td>` +
    `<td>${verdict}</td>` +
    `<td class="num mono">${item.caseTotal != null ? `${item.casePassed}/${item.caseTotal}` : '—'}</td>` +
    `<td class="num mono">${item.totalMs != null ? `${item.totalMs} ms` : '—'}</td>` +
    `<td class="num mono">${item.maxMemKb != null ? `${(item.maxMemKb / 1024).toFixed(1)} MB` : '—'}</td>` +
    '<td class="muted">详情 ▾</td>' +
  '</tr>';
}

function bindRows(el, state) {
  el.rows.addEventListener('click', (event) => {
    // 按钮优先：展开行里点「删除 / 重跑」不该顺带把行收起来
    if (event.target.closest('[data-del]') !== null) return;
    if (event.target.closest('[data-retry]') !== null) return;
    const row = event.target.closest('[data-id]');
    if (row === null) return;
    toggleDetail(el, Number(row.dataset.id));
  });

  // 键盘可达：Enter / Space 与点击等价（行是 tabindex=0 的可聚焦元素）
  el.rows.addEventListener('keydown', (event) => {
    if (event.key !== 'Enter' && event.key !== ' ') return;
    const row = event.target.closest('[data-id]');
    if (row === null) return;
    event.preventDefault();
    toggleDetail(el, Number(row.dataset.id));
  });

  el.pager.addEventListener('click', (event) => {
    const button = event.target.closest('[data-page]');
    if (button === null) return;
    state.page = Number(button.dataset.page);
    loadSubmissions(el, state);
  });
}

/**
 * 展开 / 收起一条提交的详情。同一时刻最多展开一条 —— 历史可能有几百行，
 * 全部展开会让页面变成一片表格，也让人失去"我在看哪一条"的位置感。
 */
function toggleDetail(el, id) {
  const existing = el.rows.querySelector('[data-detail]');
  if (existing !== null) {          // 已展开 → 收起（点哪一行都先收起，符合直觉）
    existing.remove();
    return;
  }

  const row = el.rows.querySelector(`[data-id="${id}"]`);
  if (row === null) return;
  const detail = document.createElement('tr');
  detail.className = 'subdetail';
  detail.dataset.detail = String(id);
  detail.innerHTML = '<td colspan="7" class="cell-center muted">加载中…</td>';
  row.after(detail);

  get(`/api/submissions/${id}`).then((reply) => {
    if (reply.code === 1002) { redirectToLogin(); return; }
    if (reply.code !== 0) {
      detail.innerHTML = `<td colspan="7" class="subdetail__msg">${escapeHtml(reply.message || '读取失败')}</td>`;
      return;
    }
    detail.innerHTML = detailHtml(reply.data);
    bindDetailActions(detail, el, state, reply.data);
  }).catch((error) => {
    detail.innerHTML = `<td colspan="7" class="subdetail__msg">${escapeHtml(error.message || '无法连接到服务器')}</td>`;
  });
}

function detailHtml(data) {
  const cases = (data.caseResults || []).map((item) =>
    '<tr class="case ' + (item.passed ? 'case--yes' : 'case--no') + '">' +
    `<td class="mono">${item.caseIdx}</td>` +
    `<td class="mono">${escapeHtml(item.argsText ?? '—')}</td>` +
    `<td class="mono">${escapeHtml(item.outputText ?? (item.errorMsg || '—'))}</td>` +
    `<td class="mono">${escapeHtml(item.expectedText ?? '—')}</td>` +
    `<td>${escapeHtml(item.verdict)}</td></tr>`).join('');

  const table = cases === '' ? '' :
    '<table class="tagtable"><thead><tr><th>#</th><th>输入</th><th>你的输出</th><th>期望输出</th><th>结论</th></tr></thead>' +
    `<tbody>${cases}</tbody></table>`;

  const canRetry = data.canRetry === true || (data.status === 'failed' && data.verdict === 'JE');

  return '<td colspan="7">' +
    (data.message ? `<p class="subdetail__msg" style="padding-top:var(--sp-3)">${escapeHtml(data.message)}</p>` : '') +
    '<div class="subdetail__grid">' +
      `<div><h4>代码（${escapeHtml(data.createdAt ? formatLocalTime(data.createdAt) : '')}）</h4>` +
        `<pre>${escapeHtml(data.code || '')}</pre></div>` +
      `<div><h4>逐用例结果${data.caseTotal != null ? `（通过 ${data.casePassed}/${data.caseTotal}）` : ''}</h4>` +
        (table || '<p class="muted">没有逐用例明细</p>') +
        (data.compilerOutput ? `<pre style="margin-top:var(--sp-3)">${escapeHtml(data.compilerOutput)}</pre>` : '') +
      '</div>' +
    '</div>' +
    `<div class="subdetail__actions">` +
      `<button type="button" class="btn btn--ghost btn--sm" data-del="${data.id}">删除这条提交</button>` +
      (canRetry ? `<button type="button" class="btn btn--sm" data-retry="${data.id}">重跑</button>` : '') +
    '</div>' +
  '</td>';
}

function bindDetailActions(detail, el, state, data) {
  const delButton = detail.querySelector('[data-del]');
  if (delButton !== null) {
    delButton.addEventListener('click', () => {
      const cases = (data.caseResults || []).length;
      // 确认框里必须写清会连带删掉什么（D-49）：用户点的是"删除一条"，
      // 实际连带删掉的是 N 行用例结果与 M 行编译诊断
      const extra = cases > 0 ? `，含 ${cases} 个用例结果` : '';
      if (!confirm(`这条提交记录将被永久删除${extra}，不可恢复。确认删除？`)) return;
      delButton.disabled = true;
      del(`/api/submissions/${data.id}`).then((reply) => {
        if (reply.code === 1002) { redirectToLogin(); return; }
        if (reply.code !== 0) { toast(reply.message || '删除失败', 'error'); delButton.disabled = false; return; }
        toast(reply.data.message || '提交记录已删除', 'ok');
        loadSubmissions(el, state);     // 重取：总数与行序都变了（[D-58] 的同一条理由）
      }).catch((error) => { toast(error.message || '网络异常', 'error'); delButton.disabled = false; });
    });
  }

  const retryButton = detail.querySelector('[data-retry]');
  if (retryButton !== null) {
    retryButton.addEventListener('click', () => {
      retryButton.disabled = true;
      post(`/api/submissions/${data.id}/retry`).then((reply) => {
        if (reply.code === 1002) { redirectToLogin(); return; }
        if (reply.code !== 0) {
          // 1009 = judge 未接线；服务端已完整校验过归属与状态，这里只透传话术
          toast(reply.message || '重跑失败', 'error', 6000);
          retryButton.disabled = false;
          return;
        }
        toast('已提交重跑，历史里会多出一条新记录', 'ok');
        loadSubmissions(el, state);
      }).catch((error) => { toast(error.message || '网络异常', 'error'); retryButton.disabled = false; });
    });
  }
}

function renderPager(el, state) {
  const pages = Math.max(1, Math.ceil(state.total / PAGE_SIZE));
  if (pages <= 1) { el.pager.innerHTML = ''; return; }
  const btn = (n, label, disabled) =>
    `<button type="button" class="pager__btn${n === state.page ? ' is-on' : ''}" data-page="${n}"` +
    `${n === state.page ? ' aria-current="page"' : ''}${disabled ? ' disabled' : ''}>${label ?? n}</button>`;
  el.pager.innerHTML =
    btn(state.page - 1, '上一页', state.page <= 1) +
    btn(state.page + 1, '下一页', state.page >= pages) +
    `<span class="pager__info">第 ${state.page} / ${pages} 页</span>`;
}

// ---- tab3 账户设置：改密 ----------------------------------------------------------

function bindPassword(el, errSlots) {
  const clearErrors = () => {
    for (const slot of Object.values(errSlots)) slot.textContent = '';
    el.oldPw.classList.remove('is-invalid');
    el.newPw.classList.remove('is-invalid');
    el.newPw2.classList.remove('is-invalid');
  };
  const showError = (field, text) => {
    const input = { oldPassword: el.oldPw, newPassword: el.newPw, newPassword2: el.newPw2 }[field];
    if (input !== undefined) input.classList.add('is-invalid');
    if (errSlots[field] !== undefined) errSlots[field].textContent = text;
  };

  el.pwForm.addEventListener('submit', (event) => {
    event.preventDefault();
    clearErrors();

    const oldPassword = el.oldPw.value;
    const newPassword = el.newPw.value;

    // 本地校验不通过就不发请求（与登录页同一条约定）
    if (oldPassword === '') { showError('oldPassword', '请输入当前密码'); el.oldPw.focus(); return; }
    if (newPassword.length < PASSWORD_MIN || newPassword.length > PASSWORD_MAX) {
      showError('newPassword', `新密码需 ${PASSWORD_MIN}–${PASSWORD_MAX} 位`); el.newPw.focus(); return;
    }
    if (newPassword !== el.newPw2.value) {
      showError('newPassword2', '两次输入的新密码不一致'); el.newPw2.focus(); return;
    }

    el.pwSubmit.disabled = true;
    el.pwSubmit.textContent = '提交中…';
    post('/api/auth/password', { oldPassword, newPassword }).then((reply) => {
      if (reply.code === 1002) { redirectToLogin(); return; }
      if (reply.code === 1001) {
        // 服务端按 data.field 指明是哪个框错了（D-40）
        const field = reply.data.field || 'oldPassword';
        showError(field, field === 'oldPassword' ? '旧密码不正确' : '新密码不符合要求');
        // 旧密码错时【不清空新密码】：用户多数只想改旧密码后重试
        if (field === 'oldPassword') el.oldPw.value = '';
        return;
      }
      if (reply.code !== 0) { toast(reply.message || '修改失败', 'error'); return; }

      toast(reply.data.message || '密码已修改', 'ok');
      el.pwForm.reset();
      // 角色真源是 .env，可能已被运维改过 ⇒ 成功后静默重拉一次登录态（§6.3.5 要点 5）
      currentUser().then(() => renderTopbar(el.topbar, { active: 'me' }));
    }).catch((error) => {
      toast(error.message || '网络异常', 'error');
    }).finally(() => {
      el.pwSubmit.disabled = false;
      el.pwSubmit.textContent = '确认修改';
    });
  });
}

function bindLogout(el) {
  el.logout.addEventListener('click', async () => {
    el.logout.disabled = true;
    try {
      await apiLogout();     // 内部会清掉 api.js 的 currentUser 缓存，否则同页顶栏仍显示已登录
      location.href = '/login.html';
    } catch (error) {
      toast(error.message || '登出失败', 'error');
      el.logout.disabled = false;
    }
  });
}
