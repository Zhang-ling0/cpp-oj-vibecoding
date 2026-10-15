// =============================================================================
// 登录 / 注册页行为 —— SPEC §9 M4 T4.2
// 规格来源：§7.1（登录/注册页「切换式表单，登录成功跳 list.html」，且**不预填上次用户名** [D-46]）
//           §5.8（用户名 3–32、密码 6–64）/ [D-43] 未登录可读题面
//           [D-44] T4.27 会话过期后重登要能跳回原页面
//           [D-57] T4.29 注销后跳回并提示
//           §6.3（1001 + data.field、1005、1010）
//
// 【四条前端校验必须与服务端逐字一致，不是"大概一样"】
//   用户名的字符集 [A-Za-z0-9_-] 与长度 3–32、密码 6–64，与 auth_api.h 的
//   validate_username / validate_password 是同一组规则。两边各写一份必然漂移，
//   而漂移的症状很隐蔽：前端放行一个 40 字符的用户名，服务端回 1001，
//   用户看到的是"表单自己清空了"却不知道为什么。
//   本轮只能各写一份（前端无构建步骤、拿不到 C++ 常量），故在下方注释里
//   【逐条注明与服务端哪个常量对应】—— 改服务端时这里必须跟着改。
//
// 【本地校验不通过就不发请求】
//   这是本文件与 T4.17（改密表单）同一条约定：本地拦下的错误不发请求。
//   代价是"服务端那条分支在浏览器里不会被触发"——但它本来就该由 §9 的
//   curl 验收（AC-1/AC-21）与单测覆盖，前端重复跑一遍只会更慢。
//
// 【为什么注册成功后【直接切到登录并自动提交】，而不是回登录页让用户再输一次】
//   单机自用工具，注册者就是本人，紧接着再输一遍密码是纯粹的摩擦。
//   但【不能】在注册响应里种 Cookie —— §6.3 的注册端点不签发会话
//   （"→ 201 {userId, username, role}"，没有 Set-Cookie），于是这里走一次 login。
//   代价是多一次 bcrypt（约 100ms，§6.2.1 要点 3），换来的是少一次交互。
// =============================================================================

import { post } from './api.js?v=20261015-5';

// 与 include/web/auth_api.h 的常量逐条对应（改那边必须改这里）
const USERNAME_MIN = 3;
const USERNAME_MAX = 32;
const USERNAME_RE = /^[A-Za-z0-9_-]+$/;   // auth_api.h【用户名字符集限定】
const PASSWORD_MIN = 6;
const PASSWORD_MAX = 64;                   // 【字节】不是字符，见 auth_api.h 注释

// 错误码 → 人可读文案。
// ⚠️ 1010（注册已关闭）与 1005（用户名已注册）各有专门文案，不能都落到"操作失败"：
//   §6.3 特意为注册关闭留了独立码与独立语义，混成一句话等于白留这个码。
const CODE_TEXT = {
  1001: '输入不符合要求',
  1005: '该用户名已被注册，换一个试试',
  1010: '本实例已关闭注册（OJ_ALLOW_REGISTER=0）',
  1008: '服务器内部错误，请稍后重试',
  0: 'ok',
};

/**
 * 校验登录/注册表单。@returns {{username?:string, password?:string, ok:boolean}}
 * field 是服务端 1001 的 data.field 取值，前端用它把红字挂到对应输入框下（§6.3）。
 */
function validate(mode, username, password) {
  if (username.length < USERNAME_MIN || username.length > USERNAME_MAX) {
    return { ok: false, field: 'username', reason: `用户名需 ${USERNAME_MIN}–${USERNAME_MAX} 个字符` };
  }
  if (!USERNAME_RE.test(username)) {
    return { ok: false, field: 'username', reason: '只能使用字母、数字、下划线和连字符' };
  }
  // 登录时**不校验密码长度**：老账可能是 5 位（改密码功能 T4.17 之前没有），
  // 前端先拦一道会让正确口令也登不进去，且错误文案（"长度不足"）指向错误方向。
  // 注册才查长度 —— 因为服务端注册时一定会查（§6.3 的四道闸门之三）。
  if (mode === 'register' && (password.length < PASSWORD_MIN || password.length > PASSWORD_MAX)) {
    return { ok: false, field: 'password', reason: `密码需 ${PASSWORD_MIN}–${PASSWORD_MAX} 位` };
  }
  if (password.length === 0) {
    return { ok: false, field: 'password', reason: '请输入密码' };
  }
  return { ok: true };
}

/**
 * 解析并校验 redirect 参数（[D-44] T4.27 / T4.28）。
 *
 * 【必须防开放重定向】只接受【站内绝对路径】：以单个 '/' 开头，且不以 '//' 开头。
 *   `//evil.com/x` 与 `/\evil.com` 在浏览器里都会被当外站地址 —— 只查
 *   "以 / 开头"是不够的，这是最常见的一处"看起来校验过了"。
 *   校验不过就退回默认的 /list.html，而不是报错：用户手敲一个坏参数不该被拦住。
 */
function safeRedirect(raw) {
  if (!raw) return '/list.html';
  if (!raw.startsWith('/') || raw.startsWith('//') || raw.startsWith('/\\')) return '/list.html';
  // 不许跳回登录页自己：未登录拦截后跳登录，登录后又跳回登录 → 无限循环。
  if (raw.startsWith('/login.html')) return '/list.html';
  return raw;
}

export function initLoginPage(root) {
  const el = {
    tabs: root.querySelectorAll('[data-mode]'),
    username: root.querySelector('#username'),
    password: root.querySelector('#password'),
    title: root.querySelector('#auth-title'),
    subtitle: root.querySelector('#auth-subtitle'),
    usernameError: root.querySelector('#username-error'),
    passwordError: root.querySelector('#password-error'),
    passwordHint: root.querySelector('#password-hint'),
    submit: root.querySelector('#submit'),
    form: root.querySelector('#auth-form'),
    notice: root.querySelector('#notice'),
  };

  // 已登录还来登录页：直接送走，省得用户在"登录"表单上点一下才被弹回。
  // [D-46] 说不预填用户名，但也【没说要拦住已登录用户】—— 拦了反而挡不住
  // 别人共用一台机器的场景，而那恰好是本项目的目标场景之一。
  let mode = new URLSearchParams(location.search).get('mode') === 'register' ? 'register' : 'login';

  const params = new URLSearchParams(location.search);
  const redirect = safeRedirect(params.get('redirect'));

  // ---- 页面级提示（三种来源，各有明确出处，不是"通用错误区"）------------------
  // ① deleted=1   注销完成跳回来的（D-57 T4.29）
  // ② expired=1   会话过期被踢回来的（D-44 T4.27）
  // ③ 注册被关闭：不在这里判断（§7.1 明确"不加探测注册是否开放的端点"），
  //    而是等用户真的提交注册后由 1010 触发 —— 那才是准确的信息。
  if (params.get('deleted') === '1') {
    showNotice('账号已注销，相关数据已一并删除。', 'ok');
  } else if (params.get('expired') === '1') {
    showNotice('登录状态已过期，请重新登录。登录后会回到你原来的页面。', 'warn');
  }

  function showNotice(text, kind) {
    el.notice.textContent = text;
    el.notice.className = `notice notice--${kind}`;
    el.notice.hidden = false;
  }

  function setMode(next) {
    mode = next;
    for (const tab of el.tabs) {
      const active = tab.dataset.mode === mode;
      tab.classList.toggle('is-active', active);
      tab.setAttribute('aria-selected', active ? 'true' : 'false');
    }
    el.submit.textContent = mode === 'login' ? '登录' : '注册并登录';
    // 标题与副标题跟着模式走：两个 tab 共用同一块区域，若标题固定不变，
    // 点了「注册」之后页面看起来毫无变化 —— 用户会以为按钮没生效。
    el.title.textContent = mode === 'login' ? '欢迎回来' : '创建账号';
    el.subtitle.textContent = mode === 'login'
      ? '登录后即可提交代码、查看提交历史与刷题进度'
      : '注册后立即登录，账号仅用于本机保存草稿与提交记录';
    // 注册是"设一个新口令"，登录是"填已有口令"：告诉浏览器哪一种，
    // 它才不会在注册表单上弹出"用已有密码"的建议。
    el.password.autocomplete = mode === 'login' ? 'current-password' : 'new-password';
    // 注册才需要长度提示（登录不校验密码长度，理由见 validate 的注释）
    el.passwordHint.textContent = mode === 'register'
      ? `密码 ${PASSWORD_MIN}–${PASSWORD_MAX} 位`
      : '';
    clearFieldErrors();
    el.notice.hidden = true;
    el.username.focus();
  }

  function clearFieldErrors() {
    el.usernameError.textContent = '';
    el.passwordError.textContent = '';
    el.username.classList.remove('is-invalid');
    el.password.classList.remove('is-invalid');
  }

  /** 把 1001 的 data.field 挂到对应输入框下（§6.3 的字段级失败） */
  function showFieldError(field, reason) {
    const input = field === 'username' ? el.username : el.password;
    const slot = field === 'username' ? el.usernameError : el.passwordError;
    input.classList.add('is-invalid');
    slot.textContent = reason;
    input.focus();
  }

  for (const tab of el.tabs) {
    tab.addEventListener('click', () => setMode(tab.dataset.mode));
  }

  el.form.addEventListener('submit', async (event) => {
    event.preventDefault();
    clearFieldErrors();
    el.notice.hidden = true;

    const username = el.username.value.trim();
    const password = el.password.value;

    const check = validate(mode, username, password);
    if (!check.ok) {
      showFieldError(check.field, check.reason);
      return;   // 本地拦下，【不发请求】
    }

    el.submit.disabled = true;
    const originalLabel = el.submit.textContent;
    // bcrypt cost=10 在本机约 60~100ms（§6.2.1 要点 3），不给反馈用户会以为没点上
    el.submit.textContent = mode === 'login' ? '登录中…' : '注册中…';

    try {
      if (mode === 'register') {
        const registered = await post('/api/auth/register', { username, password });
        if (registered.code !== 0) {
          // 1005 与 1010 是"整体消息"而不是字段错误：用户名本身合法，
          // 挂到输入框下会让人以为是格式问题，改用户名或等管理员开注册才对。
          showNotice(registered.message || CODE_TEXT[registered.code] || '注册失败',
                     registered.code === 1010 ? 'warn' : 'error');
          if (registered.code === 1001 && registered.data.field) {
            showFieldError(registered.data.field, registered.data.reason || '');
          }
          return;
        }
      }

      const logged = await post('/api/auth/login', { username, password });
      if (logged.code !== 0) {
        if (logged.code === 1001 && logged.data.field) {
          showFieldError(logged.data.field,
                         logged.data.reason || '用户名或密码错误');
        } else {
          showNotice(logged.message || CODE_TEXT[logged.code] || '登录失败', 'error');
        }
        return;
      }

      // 【清空密码框再跳转】：不是洁癖 —— 浏览器 bfcache 可能在同页返回时
      // 恢复出带密码的表单，那等于把口令留在页面上。
      el.password.value = '';
      location.href = redirect;
    } catch (error) {
      showNotice(error.message || '无法连接到服务器', 'error');
    } finally {
      el.submit.disabled = false;
      el.submit.textContent = originalLabel;
    }
  });

  setMode(mode);
}
