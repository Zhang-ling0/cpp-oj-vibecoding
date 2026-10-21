// =============================================================================
// 题库列表页（兼首页）—— SPEC §9 M4 T4.3 [D-38] / T4.9 的收藏切换
// 规格来源：§7.1 页面结构 / §7.3 关键交互 / §7.4 空态 / §6.3 GET /api/problems
//           GET /api/tags / POST /api/problems/{slug}/favorite
//
// 【概览条与表格是两个独立的数据流】
//   翻页 / 改筛选只重发 GET /api/problems，**不重取 overview** ——
//   概览条不随分页闪烁是它作为"页面头部常量区"的前提（§7.3 原文）。
//
// 【未登录时【不发】progress 与 favorite 两个查询参数】
//   服务端对匿名请求带这两个参数一律回 1001（API.md §4.2），
//   而不是静默忽略 —— 忽略会返回一个"看起来合法但不是用户要的东西"的列表。
//   所以前端在未登录时直接把这两个控件置灰，而不是发出去再报错。
//
// 【关键词输入 debounce 300ms】
//   每敲一个字发一次请求 = 每敲一个字打一次 DB。
//   300ms 是"手速停顿"的经验值：短到感觉不到，长到足够合并连续输入。
// =============================================================================

import { get, post, currentUser } from './api.js?v=20261021-1';
import { escapeHtml, toast, renderTopbar } from './common.js?v=20261021-1';
import { renderOverviewBar } from './overview.js?v=20261021-1';

const PAGE_SIZE = 20;
const DIFF_TEXT = { easy: '简单', medium: '中等', hard: '困难' };
const PROGRESS_TEXT = { none: '未开始', attempted: '尝试过', solved: '已通过' };

/** 页内状态。分页与筛选放在一起，是因为改任何一个都要重发同一个请求。 */
const state = {
  keyword: '',
  difficulty: '',
  tags: new Set(),
  progress: '',
  favorite: false,
  sort: '',
  page: 1,
  total: 0,
};

export function initListPage(root) {
  const el = {
    topbar: root.querySelector('#topbar'),
    overview: root.querySelector('#overview'),
    keyword: root.querySelector('#f-keyword'),
    difficulty: root.querySelectorAll('[data-difficulty]'),
    progress: root.querySelector('#f-progress'),
    sort: root.querySelector('#f-sort'),
    favorite: root.querySelector('#f-favorite'),
    tags: root.querySelector('#f-tags'),
    resultHint: root.querySelector('#result-hint'),
    tbody: root.querySelector('#rows'),
    pager: root.querySelector('#pager'),
    clear: root.querySelector('#clear-filters'),
  };

  renderTopbar(el.topbar, { active: 'list' });
  currentUser().then((user) => {
    renderOverviewBar(el.overview, user);
    if (user === null) {
      // 匿名禁掉两个个人化控件，并在筛选栏留一句说明为什么是灰的
      el.progress.disabled = true;
      el.favorite.disabled = true;
      el.progress.title = '登录后可按进度筛选';
      el.favorite.title = '登录后可按进度筛选';
      root.querySelector('#personal-hint').hidden = false;
    }
  });

  loadTags(el.tags);
  bindEvents(el);
  fetchList(el);
}

// ---- 标签（筛选栏的多选芯片）-----------------------------------------------------

function loadTags(mount) {
  get('/api/tags').then((reply) => {
    if (reply.code !== 0) {
      mount.textContent = '标签加载失败';
      return;
    }
    const groups = [
      { title: '算法', names: reply.data.algorithm || [] },
      { title: '来源', names: reply.data.source || [] },
    ];
    mount.innerHTML = groups
      .filter((group) => group.names.length > 0)
      .map((group) =>
        '<div class="chipgroup">' +
          `<span class="chipgroup__title">${group.title}</span>` +
          group.names.map((name) =>
            `<button type="button" class="chip" data-tag="${escapeHtml(name)}">${escapeHtml(name)}</button>`
          ).join('') +
        '</div>')
      .join('');
  });
}

// ---- 事件 ------------------------------------------------------------------------

function bindEvents(el) {
  let timer = 0;
  el.keyword.addEventListener('input', () => {
    clearTimeout(timer);
    timer = setTimeout(() => {
      state.keyword = el.keyword.value.trim();
      state.page = 1;
      fetchList(el);
    }, 300);
  });

  for (const button of el.difficulty) {
    button.addEventListener('click', () => {
      for (const other of el.difficulty) {
        other.classList.toggle('is-on', other === button);
        other.setAttribute('aria-pressed', other === button ? 'true' : 'false');
      }
      state.difficulty = button.dataset.difficulty;
      state.page = 1;
      fetchList(el);
    });
  }

  el.tags.addEventListener('click', (event) => {
    const chip = event.target.closest('[data-tag]');
    if (chip === null) return;
    const name = chip.dataset.tag;
    // 多选是 AND 语义（服务端语义如此）：命中全部所选标签的题才留下
    if (state.tags.has(name)) state.tags.delete(name);
    else state.tags.add(name);
    chip.classList.toggle('is-on', state.tags.has(name));
    chip.setAttribute('aria-pressed', state.tags.has(name) ? 'true' : 'false');
    state.page = 1;
    fetchList(el);
  });

  el.progress.addEventListener('change', () => {
    state.progress = el.progress.value;
    state.page = 1;
    fetchList(el);
  });
  el.sort.addEventListener('change', () => {
    state.sort = el.sort.value;
    state.page = 1;
    fetchList(el);
  });
  el.favorite.addEventListener('change', () => {
    state.favorite = el.favorite.checked;
    state.page = 1;
    fetchList(el);
  });

  el.clear.addEventListener('click', () => resetFilters(el));

  el.pager.addEventListener('click', (event) => {
    const button = event.target.closest('[data-page]');
    if (button === null) return;
    const page = Number(button.dataset.page);
    if (!Number.isFinite(page) || page < 1 || page === state.page) return;
    state.page = page;
    fetchList(el);
    window.scrollTo({ top: 0, behavior: 'smooth' });
  });

  el.tbody.addEventListener('click', (event) => {
    // 空态里的「清除筛选」与收藏星标共用一个委托入口：
    // 两者的差别只在于有没有 [data-*]，而分两个 addEventListener
    // 会让"表格区能点些什么"这件事散在两处。
    if (event.target.closest('[data-clear]') !== null) {
      resetFilters(el);
      return;
    }
    const star = event.target.closest('[data-star]');
    if (star === null) return;
    toggleFavorite(el, star);
  });
}

/** 清除全部筛选条件（§7.4 空态里的"清除筛选"按钮共用它）。 */
function resetFilters(el) {
  state.keyword = '';
  state.difficulty = '';
  state.progress = '';
  state.favorite = false;
  state.sort = '';
  state.tags.clear();
  state.page = 1;
  el.keyword.value = '';
  el.progress.value = '';
  el.sort.value = '';
  el.favorite.checked = false;
  for (const button of el.difficulty) {
    const on = button.dataset.difficulty === '';
    button.classList.toggle('is-on', on);
    button.setAttribute('aria-pressed', on ? 'true' : 'false');
  }
  for (const chip of el.tags.querySelectorAll('[data-tag]')) {
    chip.classList.remove('is-on');
    chip.setAttribute('aria-pressed', 'false');
  }
  fetchList(el);
}

// ---- 列表请求与渲染 ---------------------------------------------------------------

function fetchList(el) {
  el.tbody.innerHTML = '<tr><td colspan="7" class="cell-center muted">加载中…</td></tr>';

  // 只发"有值"的参数：空串发过去也会被 buildQuery 丢掉，但显式列出来更清楚
  const query = {
    keyword: state.keyword,
    difficulty: state.difficulty,
    tags: [...state.tags].join(','),
    progress: state.progress,
    favorite: state.favorite ? 1 : '',
    sort: state.sort,
    page: state.page,
    size: PAGE_SIZE,
  };

  get('/api/problems', query).then((reply) => {
    if (reply.code === 1001) {
      // 参数不合法（例如标签同名过多）：把服务端话术原样透出，不自己编
      toast(reply.message || '筛选条件不合法', 'error');
      el.tbody.innerHTML = emptyRow(reply.message || '筛选条件不合法');
      return;
    }
    if (reply.code !== 0) {
      el.tbody.innerHTML = emptyRow('题目列表加载失败，请稍后重试');
      return;
    }

    const list = reply.data.list || [];
    // 服务端会把非法 page 夹回真实页码（如删完题后停在第 9 页），
    // 跟着它走，否则用户会卡在一个永远返回空的页码上
    state.page = reply.data.page || 1;
    state.total = reply.data.total || 0;

    el.resultHint.textContent = state.total === 0
      ? '没有匹配的题目'
      : `共 ${state.total} 题`;
    el.tbody.innerHTML = list.length === 0 ? emptyRow() : list.map(rowHtml).join('');
    renderPager(el);
  }).catch((error) => {
    el.tbody.innerHTML = emptyRow(error.message || '无法连接到服务器');
  });
}

function rowHtml(item) {
  const progress = item.myProgress || 'none';
  const tags = (item.tags || []).map((tag) =>
    `<span class="tag tag--${escapeHtml(tag.category)}">${escapeHtml(tag.name)}</span>`
  ).join('');
  const star = item.isFavorite
    ? `<button type="button" class="star is-on" data-star="${escapeHtml(item.slug)}" aria-pressed="true" aria-label="取消收藏">★</button>`
    : `<button type="button" class="star" data-star="${escapeHtml(item.slug)}" aria-pressed="false" aria-label="收藏">☆</button>`;

  return '<tr>' +
    `<td class="cell-title"><a href="/problem.html?slug=${encodeURIComponent(item.slug)}">` +
      `${escapeHtml(item.title)}</a><span class="cell-slug mono">${escapeHtml(item.slug)}</span></td>` +
    `<td><span class="diff diff--${escapeHtml(item.difficulty)}">${DIFF_TEXT[item.difficulty] || item.difficulty}</span></td>` +
    `<td class="cell-tags">${tags}</td>` +
    `<td class="mono">${item.caseCount}</td>` +
    `<td class="mono muted" title="CPU 时间上限 / 地址空间上限（AS 口径）">` +
      `${item.timeLimitMs}ms · ${Math.round(item.memLimitKb / 1024)}MB</td>` +
    `<td><span class="prog prog--${escapeHtml(progress)}">${PROGRESS_TEXT[progress] || progress}</span></td>` +
    `<td>${star}</td>` +
  '</tr>';
}

function emptyRow(message) {
  if (message !== undefined) {
    return `<tr><td colspan="7" class="cell-center muted">${escapeHtml(message)}</td></tr>`;
  }
  // §7.4 的空态：无结果时给"放宽筛选"的下一步，而不是只说一句"没有数据"
  return '<tr><td colspan="7" class="cell-center">' +
    '<div class="empty">' +
      '<p>没有匹配的题目，试试放宽筛选条件</p>' +
      '<button type="button" class="btn btn--ghost" data-clear>清除筛选</button>' +
    '</div></td></tr>';
}

function renderPager(el) {
  const pages = Math.max(1, Math.ceil(state.total / PAGE_SIZE));
  if (pages <= 1) {
    el.pager.innerHTML = '';
    return;
  }
  const numbers = pageWindow(state.page, pages);
  const buttons = numbers.map((n) =>
    n === '…'
      ? '<span class="pager__gap">…</span>'
      : `<button type="button" class="pager__btn${n === state.page ? ' is-on' : ''}"` +
        ` data-page="${n}"${n === state.page ? ' aria-current="page"' : ''}>${n}</button>`
  ).join('');

  el.pager.innerHTML =
    `<button type="button" class="pager__btn" data-page="${state.page - 1}"` +
      `${state.page <= 1 ? ' disabled' : ''}>上一页</button>` +
    buttons +
    `<button type="button" class="pager__btn" data-page="${state.page + 1}"` +
      `${state.page >= pages ? ' disabled' : ''}>下一页</button>` +
    `<span class="pager__info">第 ${state.page} / ${pages} 页</span>`;
}

/** 页码窗口：首页尾页 + 当前页 ±2，省略号用 '…' 占位（上限 9 个按钮）。 */
function pageWindow(current, pages) {
  const numbers = [];
  const push = (n) => { if (n >= 1 && n <= pages && numbers[numbers.length - 1] !== n) numbers.push(n); };
  push(1);
  for (let n = current - 2; n <= current + 2; n += 1) push(n);
  push(pages);
  for (let i = 1; i < numbers.length; i += 1) {
    if (numbers[i] - numbers[i - 1] > 1) numbers.splice(i, 0, '…');
  }
  return numbers;
}

// ---- 收藏切换（T4.9）--------------------------------------------------------------

async function toggleFavorite(el, button) {
  const slug = button.dataset.star;

  if (button.dataset.pending === '1') return;      // 连点保护：请求在飞时忽略第二次
  button.dataset.pending = '1';
  try {
    const reply = await post(`/api/problems/${encodeURIComponent(slug)}/favorite`);
    if (reply.code === 1002) {
      // 未登录点收藏：不静默失败，也不隐式写操作（§7.4）——去登录
      location.href = `/login.html?redirect=${encodeURIComponent('/list.html')}`;
      return;
    }
    if (reply.code !== 0) {
      toast(reply.message || '收藏切换失败', 'error');
      return;
    }
    const on = reply.data.isFavorite === true;
    button.classList.toggle('is-on', on);
    button.textContent = on ? '★' : '☆';
    button.setAttribute('aria-pressed', on ? 'true' : 'false');
    button.setAttribute('aria-label', on ? '取消收藏' : '收藏');
    // 「仅看收藏」筛选开着时，取消收藏会让当前行不再属于结果集 —— 重取列表，
    // 否则页面会留下一条已经不符合筛选条件的行（用户会以为筛选漏了）。
    if (state.favorite && !on) {
      fetchList(el);
    }
  } catch (error) {
    toast(error.message || '网络异常，收藏未切换', 'error');
  } finally {
    button.dataset.pending = '0';
  }
}
