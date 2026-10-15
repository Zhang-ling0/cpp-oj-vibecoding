// =============================================================================
// 落地页 —— 大屏门面（居中排布 · 极光渐变 · Bento 网格）
// 规格来源：[D-38]「首页 = list.html 兼落地页，不新增首页」← 本页是一次显式偏离，
//           理由与边界写在 index.html 的文件头与 SPEC §7.1；§1.1 三条核心价值；
//           §6.3 GET /api/problems、GET /api/tags
//
// 【本页唯一的"数据"是数字与标签，全部来自匿名可读的端点】
//   `GET /api/problems`（取 total）与 `GET /api/tags` 都是 Auth=否（D-43）；
//   `GET /api/stats` 需要登录（1002），所以【不能】用它 —— 落地页恰恰是未登录的人
//   先看到的那一屏。取不到就整行隐藏，**不显示 0**：0 会被读成"题库里一道题都没有"。
//
// 【跑马灯为什么要复制两份标签】
//   CSS 动画从 0 走到 -50%，内容必须是"两份相同标签"才无缝；
//   只有一份时动画走到一半就露出空白边缘，那是最容易被看穿的一处偷懒。
// =============================================================================

import { get } from './api.js?v=20261015-5';
import { renderTopbar, currentUser } from './common.js?v=20261015-5';

export function initLandingPage(root) {
  renderTopbar(root.querySelector('#topbar'), { active: 'landing' });
  renderLiveStats(root);
}

/** 实时数字 + 标签跑马灯：任一接口失败都只是"少一块装饰"，页面其余部分照常。 */
function renderLiveStats(root) {
  const box = root.querySelector('#live-stats');

  Promise.all([
    get('/api/problems', { size: 1 }),      // 只需要 total，不必把列表拉回来
    get('/api/tags'),
  ]).then(([problems, tags]) => {
    if (problems.code === 0) {
      root.querySelector('#stat-problems').textContent = String(problems.data.total ?? 0);
      box.hidden = false;
    }
    if (tags.code === 0) {
      root.querySelector('#stat-algo').textContent = String((tags.data.algorithm || []).length);
      root.querySelector('#stat-source').textContent = String((tags.data.source || []).length);
      renderTicker(root, [...(tags.data.algorithm || []), ...(tags.data.source || [])]);
    }
    adjustCta(root);
  }).catch(() => {
    // 网络不可达时保持隐藏：静态文案仍然完整可读，不该因为三个数字缺席而报错
  });
}

/** 标签跑马灯。aria-hidden：装饰性重复内容，读屏会念两遍同样的词。 */
function renderTicker(root, names) {
  const ticker = root.querySelector('#ticker');
  if (ticker === null || names.length === 0) return;
  // 标签很少时不做跑马灯：3 个标签慢速滚动比静止更吵，且看不出是循环
  if (names.length < 6) {
    ticker.hidden = false;
    ticker.innerHTML = `<div class="ticker__row">${names.map(esc).map((n) => `<span>${n}</span>`).join('')}</div>`;
    return;
  }
  const chips = names.map((n) => `<span>${esc(n)}</span>`).join('');
  ticker.hidden = false;
  ticker.innerHTML = `<div class="ticker__row">${chips}${chips}</div>`;   // 两份，供 -50% 位移无缝衔接
}

function esc(text) {
  return String(text).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

/** 未登录给「开始刷题 / 登录」，已登录把第二个按钮换成「我的提交」。 */
async function adjustCta(root) {
  const user = await currentUser();
  if (user === null) return;
  const cta = root.querySelector('.hero__cta');
  if (cta === null) return;
  cta.innerHTML =
    '<a class="btn btn--lg" href="/list.html">开始刷题</a>' +
    `<a class="btn btn--ghost btn--lg" href="/history.html">我的提交（${esc(user.username)}）</a>`;
}
