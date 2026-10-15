// =============================================================================
// 运维概览页 —— SPEC §9 M4 T4.14 `[D-37]`
// 规格来源：§7.1「仅 admin：系统运行状态卡片 + 用户清单表。无任何写操作」
//           §6.3.3 / API.md §7.1 `GET /api/admin/overview`
//
// 【本页只有一种网络行为：GET /api/admin/overview】
//   没有任何写端点被调用 —— 这不是"还没做"，是 D-37 刻意划的边界。
//   下面的代码里找不到 POST/PUT/DELETE，不是省略，是不存在。
//
// 【为什么 1003 要"先显示再跳走"，而不是直接跳】
//   T4.14 要求普通用户直连时"提示无权限并跳回 list.html"。若直接跳，
//   用户只看到自己被弹走，**不知道是"没权限"还是"页面坏了"**。
//   先渲染一句原因、再自动跳，是把 1 秒的不适换成一条能定位的信息。
//   期间保留「立即返回」链接 —— 自动跳转不该剥夺用户自己决定的权利。
//
// 【judgeAlive = false 要显式说成"未接线"而不是渲染成 0】
//   队列长度 0 / 并发 0 是"空",不是"没有"。用户看到 0 会以为队列是空的（正常态），
//   而真实原因是判题调度器尚未接线（§9 T2.7）—— 两者对用户的行动含义完全不同。
//
// 【AC 率 0 提交时渲染「—」而不是 0%】(与 T4.16 通过率同一条约定)
//   0/0 不是 0% 成功，是"没数据"。把没数据渲染成 0% 会让榜单出现假的最低分。
//
// 【不静默回退 lastVerdict】(API.md §7.1 明确)
//   最近一条还在排队时 `lastVerdict` 就是 null；不能拿上一条 AC 顶替 ——
//   那会让管理员以为"最新这次就通过了"。
// =============================================================================

import { get } from './api.js?v=20261015-5';
import { escapeHtml, toast, renderTopbar, formatLocalTime } from './common.js?v=20261015-5';

const VERDICT_TEXT = {
  AC: '通过', WA: '答案错误', TLE: '超时',
  MLE: '内存超限', RE: '运行错误', CE: '编译错误', JE: '平台错误',
};

export function initAdminPage(root) {
  const el = {
    topbar: root.querySelector('#topbar'),
    refresh: root.querySelector('#refresh'),
    denied: root.querySelector('#denied'),
    sysStats: root.querySelector('#sys-stats'),
    sysRuntime: root.querySelector('#sys-runtime'),
    sysNote: root.querySelector('#sys-note'),
    userCount: root.querySelector('#user-count'),
    users: root.querySelector('#users'),
  };

  renderTopbar(el.topbar, { active: 'admin' });

  el.refresh.addEventListener('click', () => {
    el.refresh.disabled = true;
    el.refresh.textContent = '加载中…';
    load(el).finally(() => {
      el.refresh.disabled = false;
      el.refresh.textContent = '刷新';
    });
  });

  load(el);
}

function load(el) {
  return get('/api/admin/overview').then((reply) => {
    // 未登录：整页不可看 ⇒ 跳登录页并带 redirect（common.js 的约定）
    if (reply.code === 1002) {
      location.href = `/login.html?redirect=${encodeURIComponent('/admin.html')}`;
      return;
    }
    // 已登录但非 admin：先说清原因，再按 T4.14 跳回题库
    if (reply.code === 1003) {
      el.denied.hidden = false;
      setTimeout(() => location.replace('/list.html'), 1500);
      return;
    }
    if (reply.code !== 0) {
      toast(reply.message || '管理端点读取失败，请看 logs/app.log', 'error', 6000);
      el.sysStats.innerHTML = `<div class="notice notice--error">${escapeHtml(reply.message || '读取失败')}</div>`;
      return;
    }
    renderSystem(el, reply.data.system);
    renderUsers(el, reply.data.users || []);
  }).catch((error) => {
    toast(error.message || '无法连接到服务器', 'error');
  });
}

function renderSystem(el, sys) {
  // ---- 计数栅格：复用 app.css 的 .stat，与首页概览条同一套排版 ----
  el.sysStats.innerHTML =
    tile('用户', sys.userCount, `其中管理员 ${sys.adminCount}`) +
    tile('题目', sys.problemCount, `用例 ${sys.caseCount}`) +
    tile('提交', sys.submissionCount, `AC ${sys.acCount} · 失败 ${sys.failedCount}`) +
    tile('未完成提交', sys.judgingCount, 'pending / judging');

  // ---- 运行态三项：进程内数据，不是 DB 计数 ----
  // ⚠️ judgeAlive 单独用 pill 表达而不是一个数字：0 与 不可用是两件事（见文件头）
  const alive = sys.judgeAlive === true;
  el.sysRuntime.innerHTML =
    runtime('队列长度', `${sys.queueLength}`) +
    runtime('并发评测', `${sys.activeJudges}`) +
    '<div class="runtime__item">' +
      `<span class="runtime__label">判题调度器</span>` +
      (alive
        ? '<span class="pill pill--ok">运行中</span>'
        : '<span class="pill pill--warn">未接线</span>') +
    '</div>';

  // 注意：这里写的是 innerHTML，故强调用 <b> 而不是 Markdown 的 ** ——
  // 混用会在界面上露出两个星号
  el.sysNote.innerHTML = alive
    ? ''
    : '判题调度器尚未接线（§9 T2.7）：提交会被拒并返回「判题服务不可用」；' +
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

function renderUsers(el, users) {
  el.userCount.textContent = `共 ${users.length} 人`;

  if (users.length === 0) {
    el.users.innerHTML = '<tr><td colspan="11" class="cell-center muted">还没有用户</td></tr>';
    return;
  }

  el.users.innerHTML = users.map(rowHtml).join('');
}

function rowHtml(user) {
  const isAdmin = user.role === 'admin';
  // AC 率：0 提交渲染「—」（见文件头），不做除零也不编造 0%
  const rate = user.submissionCount > 0
    ? `${Math.round((user.acCount / user.submissionCount) * 100)}%`
    : '—';
  // 最近一条还在排队时 lastVerdict 为 null：不回退到上一条 AC（API.md §7.1 明确）
  const verdict = user.lastVerdict
    ? `<span class="verdict verdict--${escapeHtml(user.lastVerdict.toLowerCase())}">` +
      `${escapeHtml(user.lastVerdict)} ${VERDICT_TEXT[user.lastVerdict] || ''}</span>`
    : '<span class="muted">—</span>';

  return '<tr>' +
    `<td><b>${escapeHtml(user.username)}</b></td>` +
    // 角色徽标复用顶栏那一套（.badge--admin），普通用户不渲染任何徽标（D-41）
    `<td>${isAdmin ? '<span class="badge badge--admin">管理员</span>' : '<span class="muted">普通用户</span>'}</td>` +
    `<td class="mono muted">${escapeHtml(formatLocalTime(user.createdAt))}</td>` +
    `<td class="num mono">${user.submissionCount}</td>` +
    `<td class="num mono">${user.acCount}</td>` +
    `<td class="num mono">${escapeHtml(rate)}</td>` +
    `<td class="num mono">${user.solved}</td>` +
    `<td class="num mono">${user.attempted}</td>` +
    `<td class="num mono">${user.favorites}</td>` +
    `<td class="mono muted">${user.lastSubmitAt ? escapeHtml(formatLocalTime(user.lastSubmitAt)) : '—'}</td>` +
    `<td>${verdict}</td>` +
  '</tr>';
}
