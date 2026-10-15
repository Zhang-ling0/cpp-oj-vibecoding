// =============================================================================
// 首页概览条 —— SPEC §9 M4 T4.15 [D-38]
// 规格来源：§7.1「首页概览条」线框 / §6.3 GET /api/me/overview / §7.4 两种空态
//
// 【为什么 overview.js 单列一个文件】
//   §6.1 的原话：概览条（list.html）与掌握分布 tab（history.html tab2）
//   消费的是**同一个端点的同一份数据**，AC-24 要求两者数值必然一致。
//   把"请求 + 缓存 + 渲染"收在这一个模块里，一致性由"只有一份实现"保证，
//   而不是靠约定 —— 约定迟早会破。
//
// 【页面生命周期内只请求一次】[D-38]
//   翻页 / 改筛选只重发 GET /api/problems，**不重取 overview**：
//   概览条不随分页闪烁是它作为"页面头部常量区"的前提。
//
// 【未登录【不隐藏】概览条，而是改成引导】[D-38] §7.4
//   空着会让首页显得残缺，用户也无从知道有这功能。
//   注意：题库列表照常渲染（GET /api/problems 的 Auth=否，D-43）。
// =============================================================================

import { get } from './api.js?v=20261015-5';
import { escapeHtml, formatLocalTime, roleBadgeHtml } from './common.js?v=20261015-5';

const DIFF_TEXT = { easy: '简单', medium: '中等', hard: '困难' };
const VERDICT_TEXT = {
  AC: '通过', WA: '答案错误', TLE: '超时',
  MLE: '内存超限', RE: '运行错误', CE: '编译错误', JE: '平台错误',
};

// 页面生命周期内的一次性缓存（AC-24：切 tab / 翻页不产生额外请求）
let cache = null;

/**
 * 取一次概览数据，页面生命周期内复用同一个 Promise。
 * @returns {Promise<object|null>} null = 未登录或取不到（调用方渲染引导态）
 */
export function loadOverview() {
  if (cache === null) {
    cache = get('/api/me/overview')
      .then((reply) => (reply.code === 0 ? reply.data : null))
      .catch((error) => {
        console.warn('[overview] 取概览失败：', error.message);
        return null;
      });
  }
  return cache;
}

/**
 * 渲染概览条。
 * @param {HTMLElement} mount 概览条容器
 * @param {{username?: string}} [user] 登录态（未登录时省略）
 */
export async function renderOverviewBar(mount, user = null) {
  if (!mount) return;
  mount.innerHTML = '<div class="overview__loading">正在读取你的刷题进度…</div>';

  const data = await loadOverview();
  if (data === null) {
    mount.innerHTML =
      '<div class="overview__guest">' +
        '<div class="overview__guest-text">' +
          '<b>登录后查看你的刷题进度</b>' +
          '<span>通过数、难度分布、最近提交都会出现在这里</span>' +
        '</div>' +
        '<a class="btn" href="/login.html?redirect=%2Flist.html">登录</a>' +
      '</div>';
    return;
  }

  const totals = data.totals || {};
  const solved = totals.solved || 0;
  const total = totals.total || 0;

  // 三个难度迷你进度条。total=0 的难度【整条不渲染】——
  // 渲染一条 "0/0" 的空槽只会在页面上多一条没有信息量的行。
  // 反之 solved=0 且 total>0 时要渲染成 0% 空槽（§7.4 的"新用户零数据"）。
  const bars = (data.byDifficulty || [])
    .filter((row) => row.total > 0)
    .map((row) => {
      const percent = Math.round((row.solved / row.total) * 100);
      return '<div class="bar">' +
               `<div class="bar__label"><span>${DIFF_TEXT[row.difficulty] || row.difficulty}</span>` +
               `<span class="bar__value">${row.solved}/${row.total}</span></div>` +
               `<div class="bar__track"><div class="bar__fill bar__fill--${escapeHtml(row.difficulty)}"` +
               ` style="width:${percent}%"></div></div>` +
             '</div>';
    })
    .join('');

  // 最近 3 条提交（数据来自服务端，前端不编造结论）
  const recent = (data.recent || []).map((item) => {
    const verdict = item.verdict;
    const verdictHtml = verdict
      ? `<span class="verdict verdict--${escapeHtml(verdict.toLowerCase())}">` +
        `${escapeHtml(verdict)} ${VERDICT_TEXT[verdict] || ''}</span>`
      : '<span class="verdict verdict--none">评测中</span>';
    const when = formatLocalTime(item.createdAt).slice(5);   // 只留 MM-DD HH:mm
    return `<a class="recent" href="/problem.html?slug=${encodeURIComponent(item.problemSlug)}">` +
             `<span class="recent__time">${escapeHtml(when)}</span>` +
             `<span class="recent__title">${escapeHtml(item.problemTitle)}</span>` +
             verdictHtml +
           '</a>';
  })
  .join('');

  const recentHtml = recent !== ''
    ? `<div class="overview__recent">${recent}</div>`
    : '<div class="overview__empty">还没有提交记录，去做第一题 →</div>';

  mount.innerHTML =
    '<div class="overview__head">' +
      `<div class="overview__hello">你好，${escapeHtml(user.username || '')}${roleBadgeHtml(user)}</div>` +
      '<div class="overview__stats">' +
        stat(`${solved}/${total}`, '已通过') +
        stat(totals.attempted || 0, '尝试中') +
        stat(totals.favorites || 0, '收藏') +
        stat(totals.submissions || 0, '提交') +
      '</div>' +
    '</div>' +
    `<div class="overview__bars">${bars}</div>` +
    recentHtml;
}

function stat(value, label) {
  return `<div class="stat"><b>${escapeHtml(value)}</b><span>${escapeHtml(label)}</span></div>`;
}

// ---- 掌握分布（「我的」页 tab2）—— SPEC §9 T4.16 [D-39] ----------------------------

/**
 * 渲染掌握分布。
 *
 * 【与首页概览条【同源同数值】，AC-24 因此必然成立】
 *   两者读的是同一个 `loadOverview()` 的同一个 Promise（§6.1 要求单列本文件的原因）。
 *   数值一致不是靠"两边口径相同"的约定，而是靠"只有一份实现"——
 *   口径一旦要改，只改服务端一处 SQL，前端两个渲染点同时变。
 *
 * 【标签表的口径坑：多标签题在【每个】标签下各计 1 次】[D-39]
 *   一道挂 3 个标签且已 AC 的题，在 3 个标签的 total/solved 里各出现 1 次。
 *   因此 byTag 的 total 之和 ≥ 题目总数，这不是重复计数，是多对多的正常展开。
 *
 * 【通过率 total=0 渲染「—」而不是 NaN%】(与 T4.16、T4.14 同一条约定)
 *   0/0 不是"通过率 0"。
 */
export function renderProgressPanel(el) {
  el.innerHTML = '<p class="muted">正在统计…</p>';
  return loadOverview().then((data) => {
    if (data === null) {
      el.innerHTML = '<div class="notice notice--warn">登录状态已过期，请重新登录后查看掌握分布。</div>';
      return;
    }

    // ---- 左：难度卡（3 行；total=0 的难度【整行不渲染】）----
    const diffs = (data.byDifficulty || []).filter((row) => row.total > 0);
    const diffHtml = diffs.length === 0
      ? '<p class="muted">题库暂无题目</p>'
      : diffs.map((row) => {
          const pct = Math.round((row.solved / row.total) * 100);
          return '<div class="diffcard__row">' +
                   `<div class="diffcard__label"><span>${DIFF_TEXT[row.difficulty] || row.difficulty}</span>` +
                     `<span class="diffcard__value">${row.solved}/${row.total}</span></div>` +
                   `<div class="bar__track"><div class="bar__fill bar__fill--${escapeHtml(row.difficulty)}" ` +
                     `style="width:${pct}%"></div></div>` +
                   `<div class="diffcard__pct">通过率 ${pct}%</div>` +
                 '</div>';
        }).join('');

    // ---- 右：标签表（算法标签默认展开，来源标签折叠在「展开更多」里）----
    const all = data.byTag || [];
    const algo = all.filter((row) => row.category === 'algorithm');
    const source = all.filter((row) => row.category === 'source');
    const table = (rows) => rows.length === 0
      ? '<p class="muted">题库暂无标签分类</p>'
      : `<table class="tagtable"><thead><tr><th>标签</th>` +
        '<th class="num">已过</th><th class="num">总数</th><th class="num">通过率</th></tr></thead><tbody>' +
        rows.map((row) => tagRowHtml(row)).join('') + '</tbody></table>';

    el.innerHTML =
      '<div class="progress-grid">' +
        '<section class="card panel"><h3 class="panel__title">按难度</h3>' +
          `<div class="diffcard">${diffHtml}</div></section>` +
        '<section class="card panel"><h3 class="panel__title">按标签</h3>' +
          table(algo) +
          (source.length > 0
            ? `<details class="more"><summary>展开来源标签（${source.length}）</summary>${table(source)}</details>`
            : '') +
        '</section>' +
      '</div>';
  });
}

function tagRowHtml(row) {
  // total=0 → 「—」；服务端按 total DESC, name 排序，前端不再重排
  const rate = row.total > 0 ? `${Math.round((row.solved / row.total) * 100)}%` : '—';
  return '<tr>' +
    `<td>${escapeHtml(row.name)}</td>` +
    `<td class="num">${row.solved}</td>` +
    `<td class="num">${row.total}</td>` +
    `<td class="num">${escapeHtml(rate)}</td>` +
  '</tr>';
}
