// =============================================================================
// 题目详情页 —— SPEC §9 T4.4~T4.8 / T4.11 / T4.20 / T4.22 / T4.25 / T4.28
// 规格来源：§7.2 左右分栏布局 [D-26] / §7.3 关键交互 / §7.4 空态与错误态
//           §6.3 GET /api/problems/{slug} / POST /api/submissions
//           GET /api/submissions/{id} / POST /api/problems/{slug}/favorite
//
// 【左右分栏的比例存 localStorage】[D-26]
//   比例不进 URL、不进 DB：它是一次会话内的"我今天想多看题面"，
//   而 URL 里的比例会让分享出去的链接带上与内容无关的噪声。
//
// 【结果面板的三种形态不能混同】[D-65] [D-66]
//   AC/WA/TLE/MLE/RE → 渲染逐用例表格；CE → **不渲染表格**（一条用例都没跑），
//   改显编译诊断；JE → 平台错误 + 重试。把三者画成同一个面板，
//   用户会以为"CE 了却说我 0/12 全错"，那是对题面之外的误读。
//
// 【内存列写「峰值RSS」并在表下常驻口径脚注】[D-63]
//   限制是 RLIMIT_AS 口径、度量是 ru_maxrss 口径，两者不是同一个量。
//   若都写「内存」，用户看到"报 MLE 但表里只有 30MB"会以为是 bug。
//   把口径差异写在界面上，是"保留严格值"这个决策能成立的前提。
//
// 【未登录：提交按钮变「登录后提交」并弹【内联登录框】，不跳转】[D-43] T4.28
//   跳转会把正在写的代码连同草稿一起甩掉 —— 这是本项目最不能犯的错之一。
//   登录成功后原地继续，提交意图由"登录框内联完成"自然衔接，不需要 sessionStorage 暂存。
// =============================================================================

import { get, post, currentUser, invalidateCurrentUser } from './api.js?v=20261015-5';
import { escapeHtml, toast, renderTopbar, formatLocalTime } from './common.js?v=20261015-5';
import { renderMarkdown } from './markdown.js?v=20261015-5';
import { createEditor, loadDraft, saveDraft, flushDraft, SAVE_DEBOUNCE_MS } from './editor.js?v=20261015-5';
import { startPolling, pendingSubmissionId, isStillJudging, isSlow } from './poll.js?v=20261015-5';

const MAX_CODE_BYTES = 65536;        // §5.8 / OJ_MAX_CODE_BYTES，与服务端同值
const DIFF_TEXT = { easy: '简单', medium: '中等', hard: '困难' };
const VERDICT_TEXT = {
  AC: '通过', WA: '答案错误', TLE: '超时',
  MLE: '内存超限', RE: '运行错误', CE: '编译错误', JE: '平台错误',
};

// 模块级页面状态。内联登录（bindInlineLogin）需要用到这里的 el / data / user / editor，
// 而它们是 initProblemPage 的局部变量 —— 不外露就只能靠 reload 刷新整页，
// 那会把编辑器里正在写的代码和光标位置一起扔掉。
let page = null;

export function initProblemPage(root) {
  const params = new URLSearchParams(location.search);
  const slug = params.get('slug') || '';
  if (slug === '') {
    location.href = '/list.html';
    return;
  }
  const el = {
    topbar: root.querySelector('#topbar'),
    back: root.querySelector('#back'),
    title: root.querySelector('#p-title'),
    difficulty: root.querySelector('#p-difficulty'),
    tags: root.querySelector('#p-tags'),
    star: root.querySelector('#favorite'),
    tabStatement: root.querySelector('#tab-statement'),
    tabHint: root.querySelector('#tab-hint'),
    tabRecent: root.querySelector('#tab-recent'),
    paneStatement: root.querySelector('#pane-statement'),
    paneHint: root.querySelector('#pane-hint'),
    paneRecent: root.querySelector('#pane-recent'),
    markdown: root.querySelector('#p-markdown'),
    hint: root.querySelector('#p-hint'),
    recent: root.querySelector('#p-recent'),
    next: root.querySelector('#p-next'),
    limits: root.querySelector('#p-limits'),
    editorMount: root.querySelector('#editor'),
    submit: root.querySelector('#submit'),
    reset: root.querySelector('#reset'),
    editorWarn: root.querySelector('#editor-warn'),
    status: root.querySelector('#status'),
    result: root.querySelector('#result'),
    inlineLogin: root.querySelector('#inline-login'),
    split: root.querySelector('#workbench'),
  };

  renderTopbar(el.topbar, { compact: true, active: 'list' });
  el.back.href = '/list.html';

  page = { el, slug, params, data: null, editor: null, poller: null, user: null };

  loadDetail(el, slug, (data) => {
    page.data = data;
    renderHead(el, data);
    setupTabs(el);
    setupEditor(el, data);
    setupSplitter(el);
    renderNext(el, data);
    currentUser().then((u) => {
      page.user = u;
      applyAuthState(el, u);
      restorePolling(el, data, u);
      // 会话过期重登回来：自动载入草稿并提示（[D-44]）
      if (params.get('expired') === '1' && loadDraft(slug).text !== '') {
        toast('已恢复你上次未完成的代码', 'ok');
      }
    });
  });
}

// ---- 加载与渲染 -------------------------------------------------------------------

function loadDetail(el, slug, done) {
  get(`/api/problems/${encodeURIComponent(slug)}`).then((reply) => {
    if (reply.code === 1004) {
      el.result.hidden = false;      // 结果面板是这一页唯一的"主区域之外"的错误出口
      el.result.innerHTML = '<div class="empty"><p>题目不存在或已从题库移除</p>' +
        '<a class="btn btn--ghost" href="/list.html">返回题库</a></div>';
      el.editorMount.hidden = true;
      el.submit.hidden = true;
      return;
    }
    if (reply.code !== 0) {
      toast(reply.message || '题目加载失败', 'error');
      return;
    }
    done(reply.data);
  }).catch((error) => {
    toast(error.message || '无法连接到服务器', 'error');
  });
}

function renderHead(el, data) {
  el.title.textContent = data.title;
  el.difficulty.textContent = DIFF_TEXT[data.difficulty] || data.difficulty;
  el.difficulty.className = `diff diff--${data.difficulty}`;
  el.tags.innerHTML = (data.tags || [])
    .map((tag) => `<span class="tag tag--${escapeHtml(tag.category)}">${escapeHtml(tag.name)}</span>`)
    .join('');
  el.limits.innerHTML =
    `<span title="CPU 时间上限">${data.timeLimitMs} ms</span>` +
    `<span title="地址空间（RLIMIT_AS）上限，比 RSS 口径更严">${Math.round(data.memLimitKb / 1024)} MB 地址空间</span>` +
    `<span>${data.caseCount} 个用例</span>`;
  // 结果面板的口径脚注要引用"地址空间上限"这个数，挂在 dataset 上比再请求一次接口省
  el.limits.dataset.memKb = String(data.memLimitKb);

  const star = el.star;
  const paint = (on) => {
    star.classList.toggle('is-on', on);
    star.textContent = on ? '★' : '☆';
    star.setAttribute('aria-pressed', on ? 'true' : 'false');
    star.setAttribute('aria-label', on ? '取消收藏' : '收藏');
  };
  paint(data.isFavorite === true);
  star.addEventListener('click', async () => {
    // page.user 是 currentUser() 异步补上的，极短时间内点星标会读到 null。
    // 这里再问一次而不是直接跳登录 —— 否则刚进页面手快点一下就被踢去登录页。
    const me = page.user !== null ? page.user : await currentUser();
    if (me === null) { location.href = '/login.html?redirect=' + encodeURIComponent(location.pathname + location.search); return; }
    const reply = await post(`/api/problems/${encodeURIComponent(data.slug)}/favorite`);
    if (reply.code === 1002) { location.href = '/login.html?redirect=' + encodeURIComponent(location.pathname + location.search); return; }
    if (reply.code !== 0) { toast(reply.message || '收藏切换失败', 'error'); return; }
    paint(reply.data.isFavorite === true);
  });

  // 题面渲染【单独兜住异常】：markdown.js 依赖两个 vendored 库，它们曾经真的缺席过
  // （T0.6 下载 DOMPurify 时用错包名，落下 43 字节占位文件）。若让异常一路冒到
  // loadDetail 的 .catch，整页初始化会中断 —— 表现是"标题有、编辑器是空的、题面全无"，
  // 而真正的原因（缺库）被埋在控制台最深处。这里把它降级成本区块的一条错误提示，
  // 页面其余部分照常可用。
  try {
    renderMarkdown(el.markdown, data.statementMd);
    // [D-47]：hint_md 为空 ⇒ **不渲染提示 tab**（不是渲染一个空 tab）
    if (data.hint_md) {
      renderMarkdown(el.hint, data.hint_md);
      el.tabHint.hidden = false;
    }
  } catch (error) {
    console.error('[problem] 题面渲染失败：', error);
    el.markdown.innerHTML =
      `<div class="notice notice--error">题面渲染失败：${escapeHtml(error.message)}</div>`;
    toast('题面渲染失败，请检查前端依赖', 'error', 6000);
  }
  renderRecent(el, data.recentSubmissions || []);
}

function renderRecent(el, list) {
  if (list.length === 0) {
    el.recent.innerHTML = '<p class="muted">还没有提交记录</p>';
    return;
  }
  el.recent.innerHTML = list.map((item) => {
    const verdict = item.verdict
      ? `<span class="verdict verdict--${escapeHtml(item.verdict.toLowerCase())}">${escapeHtml(item.verdict)} ${VERDICT_TEXT[item.verdict] || ''}</span>`
      : `<span class="verdict verdict--none">${item.status === 'failed' ? '平台错误' : '评测中'}</span>`;
    const detail = item.caseTotal
      ? `${item.casePassed}/${item.caseTotal} 用例 · ${item.totalMs ?? '—'} ms`
      : (item.message || '');
    return `<button type="button" class="recentrow" data-sub="${item.id}">` +
             `<span class="recentrow__time">${escapeHtml(formatLocalTime(item.createdAt).slice(5))}</span>` +
             verdict +
             `<span class="recentrow__detail muted">${escapeHtml(detail)}</span>` +
           '</button>';
  }).join('');
}

/** 「下一题建议」卡 [D-59]：reason 是服务端算好的文案，前端不编。 */
function renderNext(el, data) {
  const rec = data.nextRecommendation;
  if (rec === undefined) return;            // 字段不下发 = 未登录 ⇒ 整张卡不渲染
  el.next.hidden = false;
  if (rec === null) {
    el.next.innerHTML = '<b>题库已全部通过</b><span>想扩充题库？编辑 problems/*.json 后重启服务</span>';
    return;
  }
  el.next.innerHTML =
    `<b>下一题建议 · ${DIFF_TEXT[rec.difficulty] || rec.difficulty}</b>` +
    `<a href="/problem.html?slug=${encodeURIComponent(rec.slug)}">${escapeHtml(rec.title)}</a>` +
    `<span class="muted">${escapeHtml(rec.reason)}</span>`;
}

// ---- 三个 tab（题面 / 提示 / 最近提交）---------------------------------------------

function setupTabs(el) {
  const tabs = [
    { tab: el.tabStatement, pane: el.paneStatement },
    { tab: el.tabHint, pane: el.paneHint },
    { tab: el.tabRecent, pane: el.paneRecent },
  ];
  for (const { tab, pane } of tabs) {
    tab.addEventListener('click', () => {
      for (const item of tabs) {
        const on = item.tab === tab;
        item.tab.classList.toggle('is-on', on);
        item.tab.setAttribute('aria-selected', on ? 'true' : 'false');
        item.pane.hidden = !on;
      }
    });
  }
  // 最近提交行点开 = 查那条提交的详情并渲染结果面板（复盘的最小闭环）
  el.recent.addEventListener('click', (event) => {
    const row = event.target.closest('[data-sub]');
    if (row === null) return;
    loadSubmission(el, Number(row.dataset.sub), null);
  });
}

// ---- 编辑器与草稿 -----------------------------------------------------------------

function setupEditor(el, data) {
  const draft = loadDraft(data.slug);
  const initial = draft.text !== '' ? draft.text : data.codeTemplate;

  let saveTimer = 0;
  page.editor = createEditor(el.editorMount, {
    value: initial,
    onSubmit: () => submit(el, data),
    onChange: (text) => {
      clearTimeout(saveTimer);
      saveTimer = setTimeout(() => saveDraft(data.slug, text), SAVE_DEBOUNCE_MS);
    },
  });
  el.editorMount.dataset.slug = data.slug;

  // Ace 没加载成功时【说破】而不是静默降级（§9 T0.6：不能等用户自己发现没有高亮）
  // ⚠️ 这里判空：缺一个提示元素不该让后面【全部】初始化中断（题面已经渲染完了，
  // 结果 tab / 提交按钮全都没有 —— 那正是本次 DOMPurify 事故的故障形态）
  if (page.editor.degraded && el.editorWarn !== null) {
    el.editorWarn.hidden = false;
    console.error('[problem] 未检测到 window.ace，已降级为 textarea');
  }

  // [D-55] 草稿冲突：storage 事件是"另一个标签页写了同一个草稿键"的直接信号。
  // 本方案【问过再载入】，绝不自动覆盖 —— 用户在另一个标签页写的代码不该被静默顶掉；
  // 反过来也不弹"载入还是保留"的确认框，那只是把覆盖藏到了一层确认后面。
  window.addEventListener('storage', (event) => {
    if (event.key !== `draft:${data.slug}` || event.newValue === null) return;
    if (event.newValue === page.editor.getValue()) return;
    if (confirm('此代码已在另一个标签页更新，是否载入？')) {
      page.editor.setValue(event.newValue);
      toast('已载入另一个标签页的代码', 'ok');
    }
  });

  el.reset.addEventListener('click', () => {
    if (!confirm('用题目的初始模板覆盖当前代码？当前草稿会被替换。')) return;
    page.editor.setValue(data.codeTemplate);
    flushDraft(data.slug, data.codeTemplate);
    page.editor.markErrors([]);
  });
}

function setupSplitter(el) {
  const splitter = el.split.querySelector('.splitter');
  const left = el.split.querySelector('.pane-left');
  const KEY = 'split:problem';

  const saved = Number(localStorage.getItem(KEY) || 0);
  if (saved >= 25 && saved <= 75) left.style.flexBasis = `${saved}%`;

  let dragging = false;
  splitter.addEventListener('mousedown', (event) => {
    dragging = true;
    event.preventDefault();
    document.body.style.cursor = 'col-resize';
    document.body.style.userSelect = 'none';
  });
  window.addEventListener('mousemove', (event) => {
    if (!dragging) return;
    const rect = el.split.getBoundingClientRect();
    const percent = Math.min(75, Math.max(25, ((event.clientX - rect.left) / rect.width) * 100));
    left.style.flexBasis = `${percent}%`;
  });
  window.addEventListener('mouseup', () => {
    if (!dragging) return;
    dragging = false;
    document.body.style.cursor = '';
    document.body.style.userSelect = '';
    localStorage.setItem(KEY, left.style.flexBasis.replace('%', ''));
  });
  // 键盘可达：splitter 是 button，方向键调 2%
  splitter.addEventListener('keydown', (event) => {
    if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
    event.preventDefault();
    const current = parseFloat(left.style.flexBasis) || 50;
    const next = Math.min(75, Math.max(25, current + (event.key === 'ArrowLeft' ? -2 : 2)));
    left.style.flexBasis = `${next}%`;
    localStorage.setItem(KEY, String(next));
  });
}

// ---- 登录态 ------------------------------------------------------------------------

function applyAuthState(el, u) {
  if (u === null) {
    el.submit.textContent = '登录后提交';
    el.submit.classList.add('btn--ghost');
  } else {
    el.submit.textContent = '提交';
  }
}

// ---- 提交与轮询 --------------------------------------------------------------------

function submit(el, data) {
  if (page.user === null) { openInlineLogin(el); return; }

  const code = page.editor.getValue();
  // 提交前校验 [D-28]：trim 后非空、≤64KB —— 不合规就不发请求
  if (code.trim() === '') { toast('请先写点代码再提交', 'error'); page.editor.focus(); return; }
  const bytes = new TextEncoder().encode(code).length;
  if (bytes > MAX_CODE_BYTES) {
    toast(`代码 ${(bytes / 1024).toFixed(1)}KB 超过 ${MAX_CODE_BYTES / 1024}KB 上限`, 'error');
    return;
  }

  el.submit.disabled = true;
  el.submit.textContent = '提交中…';
  // 提交那一刻的代码必须立刻落盘：debounce 还没到就刷新页面会丢掉最后几百毫秒的输入
  flushDraft(data.slug, code);

  post('/api/submissions', { problemSlug: data.slug, language: 'cpp', code })
    .then((reply) => {
      if (reply.code === 1002) { returnToLogin(); return; }
      if (reply.code !== 0) {
        // 1009 = judge 未接线；1006/1007 = 限流。这三种都要说清"为什么没跑起来"
        toast(submitErrorText(reply), 'error', 6000);
        el.result.hidden = false;
        el.result.innerHTML = `<div class="notice notice--error">${escapeHtml(submitErrorText(reply))}</div>` +
          // 运维提示与用户文案分开：改文案不该混入内部状态，改内部状态也不该改文案
          (reply.code === 1009
            ? '<p class="result__foot">当前 judge 调度器尚未接线（T2.7），提交未进入队列、也未写入任何记录</p>'
            : '');
        return;
      }
      page.editor.markErrors([]);
      el.result.innerHTML = '';
      beginPolling(el, reply.data.submissionId, reply.data.queueAhead || 0);
    })
    .catch((error) => toast(error.message || '提交失败', 'error'))
    .finally(() => {
      el.submit.disabled = false;
      el.submit.textContent = page.user === null ? '登录后提交' : '提交';
    });
}

/**
 * 提交失败的用户文案。
 * 【原则：能用服务端话术就不自己编】—— 与列表页对 1001 的处理同一条约定。
 * 服务端说"判题服务不可用，请稍后再试"，前端就别擅自改成"judge 调度器未接线"：
 * 那是实现状态，不是用户能据此行动的信息。内部提示另起一行（见调用处）。
 */
function submitErrorText(reply) {
  if (reply.code === 1006) return '提交过于频繁，请稍后再试（每分钟上限 10 次）';
  if (reply.code === 1007) return '你已有 2 个评测在进行中，请等它们结束';
  if (reply.code === 1004) return '题目不存在';
  if (reply.code === 1001) return reply.message || '提交内容不合法';
  return reply.message || '提交失败';
}

function beginPolling(el, submissionId, queueAhead) {
  if (page.poller !== null) page.poller.stop();
  setStatus(el, 'judging', queueText(queueAhead));
  page.poller = startPolling({
    submissionId,
    slug: el.editorMount.dataset.slug,
    onTick: (data, info) => {
      if (data === null) {
        // 心跳：只更新队列位置的显示
        if (info.queueAhead > 0) setStatus(el, 'judging', queueText(info.queueAhead));
        if (isSlow(info.elapsedMs)) {
          setStatus(el, 'judging', '评测耗时较长，可稍后在「我的」页查看');
        }
        return;
      }
      // 终态由 onFinish 统一渲染；这里只处理"还在跑"的状态，
      // 否则同一次轮询会把结果面板渲染两遍（onTick 一次、onFinish 一次）
      if (data.status === 'pending' || data.status === 'judging') {
        setStatus(el, 'judging', '评测中…');
      }
    },
    onFinish: (data) => {
      setStatus(el, 'done', statusText(data));
      // highlight=true：只有"刚提交的这条"才允许改编辑器上的行高亮。
      // 在「我的提交」里回看历史提交时若也高亮，会把旧诊断画到用户的新代码上。
      renderResult(el, data, { highlight: true });
      toast(data.verdict === 'AC' ? '提交通过' : `提交结束：${VERDICT_TEXT[data.verdict] || data.status}`,
            data.verdict === 'AC' ? 'ok' : 'error');
    },
    onError: (error, code) => {
      if (code === 1002) { returnToLogin(); return; }
      toast(error.message, 'error');
    },
  });
}

/** 刷新后恢复轮询 [D-53]：sessionStorage 里有未出结果的提交就重建状态区。 */
function restorePolling(el, data, u) {
  if (u === null) return;
  const id = pendingSubmissionId(data.slug);
  if (id === null) return;
  isStillJudging(id).then((yes) => {
    if (!yes) return;
    beginPolling(el, id, 0);
  });
}

function loadSubmission(el, submissionId) {
  setStatus(el, 'judging', '读取提交结果…');
  get(`/api/submissions/${submissionId}`).then((reply) => {
    if (reply.code === 1002) { returnToLogin(); return; }
    if (reply.code !== 0) { toast(reply.message || '读取失败', 'error'); return; }
    const data = reply.data;
    if (data.status === 'pending' || data.status === 'judging') { beginPolling(el, submissionId, 0); return; }
    setStatus(el, 'done', statusText(data));
    renderResult(el, data);
  });
}

// ---- 结果面板（§7.2 的三种形态）-----------------------------------------------------

function statusText(data) {
  if (data.status === 'failed') return '平台错误，本次评测未能完成';
  // [D-65]：CE 时 caseTotal 是【题库用例数】而非 0，写"通过 0/12"会让人以为跑了 12 个全错
  if (data.verdict === 'CE') return `编译错误 · ${data.caseTotal} 个用例未执行`;
  return `${VERDICT_TEXT[data.verdict] || data.verdict} · 通过 ${data.casePassed}/${data.caseTotal}`;
}

function setStatus(el, state, text) {
  el.status.dataset.state = state;
  el.status.textContent = text;
  el.result.hidden = state !== 'done';
}

function queueText(n) {
  return n > 0 ? `评测中… 前面还有 ${n} 个评测` : '评测中…';
}

/**
 * 渲染结果面板。
 * @param {{highlight?: boolean}} [options] highlight=true 时才把编译诊断画到编辑器上
 */
function renderResult(el, data, options = {}) {
  const highlight = options.highlight === true;
  el.result.hidden = false;

  // ---- CE：编译错误。**不渲染逐用例表**（一条用例都没跑）----
  if (data.verdict === 'CE') {
    el.result.innerHTML =
      `<div class="result__head"><span class="verdict verdict--ce">编译错误</span>` +
      `<span>${data.caseTotal} 个用例未执行</span></div>` +
      renderCompilePanel(data) +
      `<pre class="result__raw">${escapeHtml(data.compilerOutput || '')}</pre>`;
    if (highlight) page.editor.markErrors(data.compileErrors || []);
    return;
  }

  // ---- JE：平台错误 [D-66] ----
  if (data.verdict === 'JE' || data.status === 'failed') {
    el.result.innerHTML =
      '<div class="result__head"><span class="verdict verdict--je">平台错误</span>' +
      '<span>这通常是系统问题，不是你的代码问题</span></div>' +
      (data.canRetry ? '<button type="button" class="btn btn--ghost" id="retry">重试这次提交</button>' : '') +
      `<pre class="result__raw">${escapeHtml(data.compilerOutput || data.message || '')}</pre>`;
    bindRetry(el, data.id);
    return;
  }

  // ---- AC/WA/TLE/MLE/RE：逐用例表格 ----
  const rows = (data.caseResults || []).map((item) => {
    const passed = item.passed ? 'yes' : 'no';
    return `<tr class="case case--${passed}">` +
      `<td class="mono">${item.caseIdx}</td>` +
      `<td class="mono">${escapeHtml(item.argsText ?? '—')}</td>` +
      `<td class="mono">${escapeHtml(item.outputText ?? (item.errorMsg || '—'))}</td>` +
      `<td class="mono">${escapeHtml(item.expectedText ?? '—')}</td>` +
      `<td class="mono">${item.timeMs ?? '—'}</td>` +
      `<td class="mono">${item.memKb != null ? (item.memKb / 1024).toFixed(1) + ' MB' : '—'}</td>` +
      `<td>${escapeHtml(item.verdict)}</td>` +
    '</tr>';
  }).join('');

  el.result.innerHTML =
    `<div class="result__head">` +
      `<span class="verdict verdict--${escapeHtml((data.verdict || '').toLowerCase())}">${escapeHtml(VERDICT_TEXT[data.verdict] || data.verdict || '')}</span>` +
      `<span>通过 ${data.casePassed}/${data.caseTotal}</span>` +
      `<span>总耗时 ${data.totalMs ?? '—'} ms</span>` +
      `<span>峰值RSS ${data.maxMemKb != null ? (data.maxMemKb / 1024).toFixed(1) + ' MB' : '—'}</span>` +
    '</div>' +
    (data.message ? `<p class="result__msg">${escapeHtml(data.message)}</p>` : '') +
    '<table class="table result__table"><thead><tr>' +
      '<th>#</th><th>输入</th><th>你的输出</th><th>期望输出</th><th>耗时</th><th>峰值RSS</th><th>结论</th>' +
    '</tr></thead><tbody>' + rows + '</tbody></table>' +
    // 口径脚注【不折叠】：用户第一次看到"报 MLE 但 RSS 才 30MB"时必须能立刻找到解释 [D-63]
    `<p class="result__foot">ⓘ 限制口径：地址空间 ${Math.round(el.limits.dataset.memKb / 1024)} MB ｜ 列为实测峰值 RSS，仅供参考</p>`;

  if (highlight) page.editor.markErrors([]);
}

function renderCompilePanel(data) {
  const errors = data.compileErrors || [];
  const mine = errors.filter((item) => item.file === 'user.cpp');
  const others = errors.filter((item) => item.file !== 'user.cpp');

  let html = '';
  if (mine.length > 0) {
    html += '<ul class="ce-list">' + mine.map((item) =>
      `<li><b>第 ${item.line} 行</b> ${escapeHtml(item.message)}</li>`).join('') + '</ul>';
  }
  if (others.length > 0) {
    // [D-64]：归属题库 / 链接期的错误**不给行号**，并说清"这不是你的代码出错"
    html += '<div class="notice notice--warn">⚠️ 这不是你的代码出错 —— ' +
            (others[0].file === 'link'
              ? '链接期报错，请核对类名 / 方法名 / 参数类型 / 返回类型是否与题面一致。'
              : '错误来自平台生成的测试代码，通常说明该题配置有误，请联系管理员。') +
            '</div>';
  }
  return html;
}

function bindRetry(el, submissionId) {
  const button = el.result.querySelector('#retry');
  if (button === null) return;
  button.addEventListener('click', async () => {
    button.disabled = true;
    const reply = await post(`/api/submissions/${submissionId}/retry`);
    if (reply.code !== 0) { toast(reply.message || '重跑失败', 'error'); button.disabled = false; return; }
    // [D-58] 重跑是【新增一条】记录，原记录不变；用新的 id 启动轮询
    beginPolling(el, reply.data.newSubmissionId, 0);
  });
}

// ---- 未登录内联登录框（T4.28 / [D-43]）---------------------------------------------

function openInlineLogin(el) {
  el.inlineLogin.hidden = false;
  el.inlineLogin.querySelector('input').focus();
}

/**
 * 内联登录框的提交逻辑（T4.28）。
 *
 * 【登录成功后【自动把刚才那次提交补上】，且不刷新页面】
 *   ① 不刷新：刷新会把编辑器内容和光标位置一起扔掉，用户正在写的东西不该因为
 *      "登录了一下"而消失。改为就地更新登录态 + 重新拉一次题目详情
 *      （recentSubmissions 与 nextRecommendation 匿名时不下发/为空）。
 *   ② 自动补提交：用户点「登录后提交」本身就表达了提交意图，让他登录完再按一次
 *      Ctrl+Enter 是把系统的责任推回给用户。
 *
 * 【为什么要单独导出，而不是在 initProblemPage 里顺手绑上】
 *   这段是纯 DOM 行为、无其他依赖；导出后可以被单测直接调用，
 *   也不至于让 initProblemPage 变成一个"什么都干"的入口。
 */
export function bindInlineLogin(root) {
  const box = root.querySelector('#inline-login');
  if (box === null) return;
  const username = box.querySelector('#inline-username');
  const password = box.querySelector('#inline-password');
  const go = box.querySelector('#inline-go');

  const doLogin = async () => {
    if (username.value.trim() === '' || password.value === '') {
      toast('请输入用户名与密码', 'error');
      return;
    }
    go.disabled = true;
    go.textContent = '登录中…';
    try {
      const reply = await post('/api/auth/login', { username: username.value.trim(), password: password.value });
      if (reply.code !== 0) {
        toast(reply.message || '登录失败', 'error');
        go.disabled = false;
        go.textContent = '登录';
        return;
      }
      // 与登录页同一条规矩：清空密码框，避免口令留在 DOM 与 bfcache 里
      password.value = '';
      box.hidden = true;
      if (page === null) return;

      invalidateCurrentUser();                    // api.js 的 mePromise 缓存的是"未登录"
      page.user = await currentUser();
      applyAuthState(page.el, page.user);

      // 登录后这两个区块的数据才存在（匿名请求里 recentSubmissions 为空数组、
      // nextRecommendation 整个字段不下发），不重拉就一直是登录前的样子
      const detail = await get(`/api/problems/${encodeURIComponent(page.slug)}`);
      if (detail.code === 0) {
        renderRecent(page.el, detail.data.recentSubmissions || []);
        renderNext(page.el, detail.data);
      }

      submit(page.el, page.data);                // 把刚才那次提交补上
    } catch (error) {
      toast(error.message || '无法连接到服务器', 'error');
      go.disabled = false;
      go.textContent = '登录';
    }
  };

  go.addEventListener('click', doLogin);
  password.addEventListener('keydown', (event) => { if (event.key === 'Enter') doLogin(); });
}

function returnToLogin() {
  const target = location.pathname + location.search;
  location.href = `/login.html?redirect=${encodeURIComponent(target)}&expired=1`;
}
