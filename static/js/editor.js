// =============================================================================
// 代码编辑器 —— SPEC §9 T4.5（Ace，替代原计划的 CodeMirror 5）
// 规格来源：§7.3 编辑器配置 / §7.3 草稿保存 [D-27] / 提交 Ctrl+Enter [D-28]
//           编译错误行高亮 [D-64] / 草稿冲突检测 [D-55] / 会话过期恢复 [D-44]
//
// 【为什么用 Ace 而不是 SPEC 里写的 CodeMirror 5】
//   两条理由：一是 Ace 的 C++ 模式与自动补全/括号处理开箱即用，不需要像 CM 那样
//   额外拼 mode + addon（SPEC §6.2 正是为这件事把 CM 拆成了 5 个文件）；
//   二是 Ace 的行注释标注（annotations）能**带 message**，鼠标悬停即可看到诊断文本 ——
//   SPEC §7.3 要求"悬停显示 message"，CodeMirror 的 addLineClass 做不到这一点。
//   代价：vendored 体积从 ~172KB 涨到 ~550KB，vendor/cm/ 下的 5 个 CM 文件成为死文件
//   （已在 SPEC §9 T4.5 记录，待 T4.5 收尾时一并清理）。
//   **仍走 [D-73] 的 vendored 路线**：ace.js 从不从 CDN 取，运行时零外网依赖。
//
// 【草稿：localStorage["draft:"+slug]，change 事件 debounce 800ms】
//   debounce 是必要的：ace 的 change 在粘贴、撤销、自动补全时都会触发，
//   不 debounce 就是每个按键一次 JSON.stringify + 一次同步写盘（localStorage 同步）。
//   800ms 取自 SPEC §7.3，与 T4.5 的约定一致。
//
// 【行高亮只给 file === 'user.cpp' 的诊断】[D-64]
//   harness / link 的错误归属题库或签名不匹配，给出行号只会让用户去改自己没错的行。
// =============================================================================

const DRAFT_PREFIX = 'draft:';
const DRAFT_AT_PREFIX = 'draftAt:';
const SAVE_DEBOUNCE_MS = 800;

/** ace 是否可用：脚本没加载成功时降级到 textarea（R-7 已消除，这里是防御性冗余） */
function aceAvailable() {
  return typeof window.ace !== 'undefined' && window.ace !== null;
}

/**
 * 创建编辑器。
 * @param {HTMLElement} mount 挂载点（ace 会接管它并塞进自己的 DOM）
 * @param {{value?: string, onSubmit?: () => void, onChange?: (text: string) => void}} [options]
 * @returns {{getValue: () => string, setValue: (text: string, keepCursor?: boolean) => void,
 *            focus: () => void, markErrors: (list: Array<object>) => void,
 *            setReadOnly: (flag: boolean) => void, degraded: boolean}}
 */
export function createEditor(mount, options = {}) {
  const textarea = mount.querySelector('textarea') || mount;

  if (!aceAvailable()) {
    // 降级路径：功能全在（提交、草稿、高亮提示都在调用方），只是没有语法着色
    textarea.value = options.value || '';
    textarea.addEventListener('input', () => options.onChange && options.onChange(textarea.value));
    textarea.addEventListener('keydown', (event) => {
      if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
        event.preventDefault();
        options.onSubmit && options.onSubmit();
      }
    });
    return {
      degraded: true,
      getValue: () => textarea.value,
      setValue: (text) => { textarea.value = text; },
      focus: () => textarea.focus(),
      markErrors: (list) => {
        textarea.setCustomValidity('');   // 降级态没有行级标注，改用 title 提示整体信息
        if (list.length > 0) textarea.title = `第 ${list[0].line} 行：${list[0].message}`;
      },
      setReadOnly: (flag) => { textarea.readOnly = flag; },
    };
  }

  const ace = window.ace;
  // basePath 指向 vendor 目录：ace 会按它去拼 mode/theme/worker 的 URL。
  // 不设的话它去猜一个相对当前页面的路径（problem.html 在根目录时恰好能中，
  // 但那属于"碰巧能用"，换个部署路径就静默失效且不报错）。
  ace.config.set('basePath', '/vendor/ace/');
  ace.config.set('modePath', '/vendor/ace/');
  ace.config.set('themePath', '/vendor/ace/');

  const editor = ace.edit(mount);
  editor.setTheme('ace/theme/tomorrow');           // 浅色主题，与全站浅色底一致
  editor.session.setMode('ace/mode/c_cpp');
  editor.setOptions({
    fontSize: '13px',
    showLineNumbers: true,
    showPrintMargin: false,
    tabSize: 4,
    useSoftTabs: true,           // 用空格不用 Tab：用户拷到本地能直接编译
    enableBasicAutocompletion: true,
    enableLiveAutocompletion: false,
    highlightActiveLine: true,
    scrollPastEnd: 0.2,
    wrap: false,                 // 代码不折行：折行会让行号与诊断对不上
    displayIndentGuides: true,
    // 【关闭 ace 的 C++ 语法检查 worker】
    //   worker 负责的是"边打边报语法错"，而本项目的语法诊断真源是服务端 g++ 的输出
    //   （submission_compile_errors + compilerOutput，见 §5.6）。两套诊断同时开着，
    //   用户会看到编辑器报的红线与判题结论不一致 —— 那是把权威交给了次优来源。
    //   另外 cdnjs 的 ace 包**不含 worker-*.js**（官方 ace-builds 仓库里有，
    //   但本机到 raw.githubusercontent.com 不通），开着会 404 且不报错 —— 最坏的一种失效。
    useWorker: false,
  });
  editor.commands.addCommand({
    name: 'submitCode',
    bindKey: { win: 'Ctrl-Enter', mac: 'Command-Enter' },
    exec: () => options.onSubmit && options.onSubmit(),
  });
  editor.setValue(options.value || '', -1);        // -1 = 光标停在文末
  editor.session.on('change', () => {
    options.onChange && options.onChange(editor.getValue());
  });

  return {
    degraded: false,
    getValue: () => editor.getValue(),
    setValue: (text, keepCursor) => {
      const pos = keepCursor ? editor.getCursorPosition() : null;
      editor.setValue(text, keepCursor ? 1 : -1);
      if (pos !== null) editor.moveCursorToPosition(pos);
    },
    focus: () => editor.focus(),
    markErrors: (list) => {
      // 先清空上一次的高亮，否则改对了一行、旧的标记还留在那儿
      editor.session.clearAnnotations();
      const rows = list
        .filter((item) => item.file === 'user.cpp' && item.line > 0)
        .map((item) => ({
          row: item.line - 1,          // 接口的 line 是 1-based，ace 的 row 是 0-based
          text: `${item.severity}: ${item.message}`,
          type: item.severity === 'warning' ? 'warning' : 'error',
        }));
      if (rows.length > 0) {
        editor.session.setAnnotations(rows);
        editor.scrollToLine(rows[0].row, true, true, true);   // 滚到第一条，别让用户自己找
      }
    },
    setReadOnly: (flag) => editor.setReadOnly(flag),
  };
}

// ---- 草稿（[D-27] / [D-55] / [D-44]）----------------------------------------------

/** 读草稿；同时返回 draftAt，供挂载时判断"是不是别的标签页刚改过"。 */
export function loadDraft(slug) {
  try {
    const text = localStorage.getItem(DRAFT_PREFIX + slug);
    const at = Number(localStorage.getItem(DRAFT_AT_PREFIX + slug) || 0);
    return { text: text === null ? '' : text, savedAt: at };
  } catch (cause) {
    return { text: '', savedAt: 0 };
  }
}

/** 存草稿 + 记时间戳（两个键必须同时写，[D-55] 的冲突检测靠后者）。 */
export function saveDraft(slug, text) {
  try {
    localStorage.setItem(DRAFT_PREFIX + slug, text);
    localStorage.setItem(DRAFT_AT_PREFIX + slug, String(Date.now()));
  } catch (cause) {
    // 隐私模式 / 配额满：草稿丢了不该阻断提交，故静默失败
    console.warn('[editor] 草稿写入失败：', cause.message);
  }
}

export function clearDraft(slug) {
  try {
    localStorage.removeItem(DRAFT_PREFIX + slug);
    localStorage.removeItem(DRAFT_AT_PREFIX + slug);
  } catch (cause) { /* 同上 */ }
}

/** 立即落盘一次（提交成功时用：提交那一刻的代码必须存住，不能等 debounce）。 */
export function flushDraft(slug, text) {
  saveDraft(slug, text);
}

export { SAVE_DEBOUNCE_MS, DRAFT_PREFIX, DRAFT_AT_PREFIX };
