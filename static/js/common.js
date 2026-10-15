// =============================================================================
// 全站通用前端组件 —— 顶栏（[D-52] 两套形态）+ toast + 几个小工具
// 对应任务：SPEC.md §9 M4 T4.1（顶栏 + common.js）/ T4.18（角色徽标 [D-41]）
//           / T4.19（顶栏两套形态 [D-52]）
// 规格来源：§7.1 顶栏的两套形态（含三张线框）/ [D-41] 角色徽标 / [D-69] CSRF
//           §7.4 空态与错误态
//
// 【顶栏是同一个组件按登录态条件渲染，不是两套模板】
//   §7.1 的要求原文如此。三套线框（未登录 / 普通用户 / admin）看着像三份模板，
//   但它们只在两处不同：**有没有「我的」/「运维」入口**，以及**有没有角色徽标**。
//   写成两套模板，AC-23「普通用户两者都不可见（DOM 中不存在该元素，不是 CSS 隐藏）」
//   就得靠"记得给另一套模板也删掉"来保证 —— 而那个"记得"迟早会漏。
//   条件渲染的写法里，"不渲染"是 default 分支的**默认结果**，不需要记得。
//
// 【为什么顶栏不自己 fetch 而用 api.currentUser()】
//   概览条、admin 页入口判定读的是同一份登录态。各自 fetch 会在 Network 面板里
//   出现多条一模一样的 /api/auth/me，AC-24/AC-35 的验收反而要花时间解释"为什么重复"。
//   currentUser() 在模块级缓存一次，页面生命周期内共用。
//
// 【login.html 不调用本组件】
//   §7.1 明确：登录页【不渲染顶栏】——否则页面上会同时出现「登录」按钮和「登出」按钮。
//   这是该组件唯一的例外，因此它做成"由页面主动调用"而不是"自动挂载"。
// =============================================================================

import { currentUser, logout as apiLogout, get } from './api.js?v=20261015-5';

/** HTML 转义。所有把服务端字符串插进 innerHTML 的地方都必须过它。 */
export function escapeHtml(raw) {
  return String(raw)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

/**
 * 角色徽标的 HTML 片段（[D-41] T4.18）。
 *
 * 【为什么抽出来而不是在顶栏里写死】
 *   首页概览条的「你好，alice [管理员]」与顶栏是同一段逻辑（SPEC T4.15 明写
 *   "复用这段逻辑，不另写一份"）。两处各写一遍，早晚会出现"顶栏改了徽标、
 *   概览条没改"的不一致 —— 而这种不一致只有肉眼能发现。
 *
 *   注意普通用户返回【空串】：不渲染任何徽标，也不显示"普通用户"字样。
 */
export function roleBadgeHtml(user) {
  if (user === null || user === undefined || user.role !== 'admin') return '';
  return '<span class="badge badge--admin">管理员</span>';
}


/**
 * 渲染顶栏。
 * @param {HTMLElement} mount 顶栏容器（页面自带一个带 .topbar class 的 <header>）
 * @param {{compact?: boolean, active?: string}} [options]
 *        compact = 题目页的单行变体（§7.2），用的是【同一个组件】不是第二份实现
 *        active  = 当前页高亮的导航项
 * @returns {Promise<void>} 渲染完成后 resolve（调用方可 await 以便排版）
 */
export async function renderTopbar(mount, options = {}) {
  if (!mount) return;
  const user = await currentUser();
  const nav = [
    { key: 'list', href: '/list.html', label: '题库', show: true },
    // 未登录时「我的」「运维」【不渲染】（AC-35 要求 DOM 中不存在该元素）
    { key: 'me', href: '/history.html', label: '我的', show: user !== null },
    { key: 'admin', href: '/admin.html', label: '运维', show: user !== null && user.role === 'admin' },
  ].filter((item) => item.show);

  const navHtml = nav
    .map((item) => {
      const active = item.key === options.active ? ' class="is-active"' : '';
      return `<a href="${item.href}"${active}>${escapeHtml(item.label)}</a>`;
    })
    .join('');

  // [D-41]：普通用户【不渲染任何角色徽标】（不显示"普通用户"字样）——
  // 那是一条对每个用户都无信息量的标签。片段由 roleBadgeHtml 统一生成，
  // 首页概览条复用它，两处不可能出现"一个改了另一个没改"。
  const badgeHtml = roleBadgeHtml(user);

  const rightHtml = user === null
    ? '<a class="btn btn--ghost" href="/login.html">登录</a>' +
      '<a class="btn" href="/login.html?mode=register">注册</a>'
    : `<span class="muted">${escapeHtml(user.username)}</span>${badgeHtml}` +
      '<button type="button" class="btn btn--ghost" data-topbar-logout>登出</button>';

  mount.className = options.compact ? 'topbar topbar--compact' : 'topbar';
  mount.innerHTML =
    '<a class="topbar__brand" href="/list.html">CppOJ</a>' +
    `<nav class="topbar__nav">${navHtml}</nav>` +
    `<div class="topbar__right">${rightHtml}</div>`;

  const logoutButton = mount.querySelector('[data-topbar-logout]');
  if (logoutButton !== null) {
    logoutButton.addEventListener('click', async () => {
      logoutButton.disabled = true;
      await apiLogout();
      // 登出后回登录页而不是原地刷新：顶栏会重新问一次 auth/me 并渲染未登录形态，
      // 原地刷新也能work，但会把用户留在一个当前身份已经无效的页面上
      // （例如 history.html 直连时会被后端拒绝），回登录页更可预期。
      location.href = '/login.html';
    });
  }
}

// ---- toast（§7.4 的"提示"）----------------------------------------------------

let toastHost = null;

/**
 * 弹一条轻提示。
 * @param {string} message
 * @param {'info'|'ok'|'error'} [kind]
 * @param {number} [ms] 自动消失时间
 */
export function toast(message, kind = 'info', ms = 3000) {
  if (toastHost === null) {
    toastHost = document.createElement('div');
    toastHost.className = 'toast-host';
    document.body.appendChild(toastHost);
  }
  const node = document.createElement('div');
  node.className = kind === 'info' ? 'toast' : `toast toast--${kind}`;
  node.textContent = message;
  toastHost.appendChild(node);
  setTimeout(() => node.remove(), ms);
}

// ---- 未登录守卫（[D-43] / T4.28）----------------------------------------------

/**
 * 直连受保护页面（如 history.html）时的跳转，**带上回跳地址**。
 * ⚠️ 只适用于"整个页面都不可看"的场景。提交/收藏这类动作【不要】用跳转 ——
 *   跳转会丢正在写的代码，§1.3 P10 明确要求"弹内联登录框"，那是 T4.28 的活。
 * @param {string} [extra] 追加参数，如 'expired=1'（T4.27）
 */
export function redirectToLogin(extra = '') {
  const target = location.pathname + location.search;
  location.href = `/login.html?redirect=${encodeURIComponent(target)}${extra}`;
}

// ---- 相对时间（§7.1 时间本地化；T4.12 的基础件）--------------------------------

/** 把 UTC 时间串转成 Date。存储是 UTC（[D-17]），显示是本地。 */
export function parseUtcTime(utcText) {
  if (!utcText) return null;
  // 后端下发的是 "2026-10-14 20:01:02"（空格分隔、无时区），
  // 只有带 Z 或带偏移的形式才原样交给 Date —— 否则它会被当本地时间解析，整体差 8 小时
  const when = new Date(utcText.endsWith('Z') || utcText.includes('+') ? utcText : `${utcText}Z`);
  return Number.isNaN(when.getTime()) ? null : when;
}

/** 把 UTC ISO 字符串转成本地可读时间。存储是 UTC（[D-17]），显示是本地。 */
export function formatLocalTime(utcText) {
  const when = parseUtcTime(utcText);
  if (when === null) return utcText || '';
  const pad = (n) => String(n).padStart(2, '0');
  return `${when.getFullYear()}-${pad(when.getMonth() + 1)}-${pad(when.getDate())} ` +
         `${pad(when.getHours())}:${pad(when.getMinutes())}`;
}

/**
 * 相对时间（T4.12）：「刚刚 / 3 分钟前 / 昨天 15:24 / 10-14 20:01」。
 *
 * 【为什么列表里要相对时间、详情里要绝对时间】
 *   扫历史列表时用户问的是"我刚才是什么时候提交的"（相对），
 *   复盘某条提交时问的是"准确几点几分"（绝对）—— 后者交给 title 属性，
 *   于是同一条数据既好扫又能查准，不必让用户把鼠标停在每一行上。
 *
 * 【为什么超过 24h 就只到"月-日"】
 *   再往前就写成 "10-12"，同年之内不必带年份；跨年时补上年份。
 */
export function relativeTime(utcText) {
  const when = parseUtcTime(utcText);
  if (when === null) return utcText || '';
  const pad = (n) => String(n).padStart(2, '0');
  const clock = `${pad(when.getHours())}:${pad(when.getMinutes())}`;
  const diff = Date.now() - when.getTime();

  if (diff >= 0 && diff < 60 * 1000) return '刚刚';
  if (diff < 60 * 60 * 1000) return `${Math.floor(diff / 60000)} 分钟前`;
  if (diff < 24 * 60 * 60 * 1000) return `${Math.floor(diff / 3600000)} 小时前`;

  const yesterday = new Date(when.getTime());
  yesterday.setDate(yesterday.getDate() - 1);
  const isYesterday = yesterday.toDateString() === new Date().toDateString();
  if (isYesterday) return `昨天 ${clock}`;

  const md = `${pad(when.getMonth() + 1)}-${pad(when.getDate())}`;
  return when.getFullYear() === new Date().getFullYear()
    ? `${md} ${clock}`
    : `${when.getFullYear()}-${md} ${clock}`;
}

export { currentUser, get };
