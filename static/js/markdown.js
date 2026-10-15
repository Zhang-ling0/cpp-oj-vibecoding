// =============================================================================
// Markdown 渲染 —— SPEC §9 T4.6 / T4.20 / [D-14] [D-47] [D-48]
// 规格来源：§7.3「marked.parse() 后经 DOMPurify.sanitize()，白名单允许
//           pre/code/h1~h6/table/ul/ol/li/strong/em/a/blockquote —— img 已移除 [D-48]」
//           §7.3「hint_md 与 statement_md 走【同一条】渲染管线」
//
// 【为什么必须过 DOMPurify，而不是"marked 的输出是安全的"】
//   marked 只负责把 Markdown 变成 HTML，**不判断这段 HTML 可不可信**。
//   题面来自 problems/*.json（本地文件），但它同时也是 API 的输出 ——
//   一旦哪天题库改成可从别处灌入，`<img onerror>` / `<script>` 就会直达页面。
//   白名单是唯一可靠的防线，而它同时顺手满足了 [D-48] 的"禁图片"。
//
// 【为什么白名单里没有 img】
//   §7.4 与 D-48 明确：题面禁图片，树/图/矩阵题一律用 ``` 里的字符画。
//   去掉 img 之后，这类题在窄屏上的行为就是"横向滚动"，不会把图形折行破坏掉。
//
// 【两个库是 UMD 而非 ES Module】
//   marked.min.js / purify.min.js 以 <script defer> 引入，挂到 window 上。
//   本项目无构建步骤（§7.1），不为了 ESM 特意去 npm 打包 —— 引入方式统一成
//   "非模块库走 window、自己的模块走 import"，比混用两种加载器更少出错点。
// =============================================================================

/** DOMPurify 白名单：只放行题面真正会用到的标签。 */
const ALLOWED_TAGS = [
  'h1', 'h2', 'h3', 'h4', 'h5', 'h6',
  'p', 'br', 'hr', 'div', 'span',
  'ul', 'ol', 'li',
  'strong', 'em', 'del', 'code', 'pre', 'blockquote',
  'table', 'thead', 'tbody', 'tr', 'th', 'td',
  'a', 'sup', 'sub',
];

/**
 * 依赖自检。
 *
 * 【为什么必须显式检查，而不是直接用 `DOMPurify.sanitize(...)`】
 *   这两个库是 `<script defer>` 引入的 UMD，挂到 globalThis 上。它们**曾经真的
 *   缺席过**：T0.6 阶段下载 DOMPurify 时用错了包名，vendor 目录里落下的是一个
 *   43 字节的占位文件（内容是一行报错文本），于是 `DOMPurify` 是 undefined，
 *   题面渲染整段抛 ReferenceError，页面表现是"题目载入不出来"却查不到原因。
 *   一句 `typeof DOMPurify === 'undefined'` 把这种"静默失败"变成"说人话的报错"。
 */
function assertDeps() {
  if (typeof marked === 'undefined') {
    throw new Error('marked 未加载：检查 static/vendor/marked.min.js 是否存在且非占位文件');
  }
  if (typeof DOMPurify === 'undefined') {
    throw new Error('DOMPurify 未加载：检查 static/vendor/purify.min.js 是否存在且非占位文件');
  }
}

let purifyConfigured = false;

/** 首次使用时配置一次 DOMPurify（重复 configure 没有收益，只增加不确定性）。 */
function ensurePurify() {
  if (purifyConfigured) return;
  DOMPurify.addHook('afterSanitizeAttributes', (node) => {
    // 题面里的链接只允许 http/https，且一律新标签打开 + 切断 opener：
    // 没有 rel="noopener" 的 target="_blank" 能通过 window.opener 反向操纵本页。
    if (node.tagName === 'A' && node.hasAttribute('href')) {
      const href = node.getAttribute('href') || '';
      if (!/^https?:\/\//i.test(href)) {
        node.removeAttribute('href');
        return;
      }
      node.setAttribute('target', '_blank');
      node.setAttribute('rel', 'noopener noreferrer');
    }
  });
  purifyConfigured = true;
}

/**
 * 把 Markdown 渲染成 HTML 字符串（已 sanitize）。
 * @param {string} markdown 原文；null/空 → 返回空串
 * @returns {string} 可直接插进 innerHTML 的安全 HTML
 */
export function markdownToHtml(markdown) {
  if (!markdown) return '';
  assertDeps();
  ensurePurify();
  const raw = marked.parse(markdown);
  return DOMPurify.sanitize(raw, { ALLOWED_TAGS, ALLOWED_ATTR: ['href', 'title', 'colspan', 'rowspan', 'target', 'rel'] });
}

/**
 * 把 Markdown 渲染进指定容器（题面与提示共用这一个入口，[D-47] 的"同一条管线"
 * 就是指这里的同一个函数，而不是两处长得一样的调用）。
 * @param {HTMLElement} mount
 * @param {string} markdown
 */
export function renderMarkdown(mount, markdown) {
  if (!mount) return;
  mount.innerHTML = markdownToHtml(markdown);
}
