// =============================================================================
// CppOJ 前端请求层 —— 统一包络的解析与错误语义（SPEC §9 T4.1 通用 api.js）
// 规格来源：§6.3 统一响应包络 `{code, message, data}` / [D-29] 数字业务码
//           §1.4 页面加载 < 1s / [D-43] 未登录可读题面
//
// 【一个请求在 JS 里只有两种结局，别造第三种】
//   ① 拿到了包络 → resolve({code, message, data})，**由调用方看 code 决定怎么办**
//   ② 没拿到包络（网络断 / 服务没起 / 返回的不是 JSON）→ reject(ApiError)
//
// 【为什么不"code !== 0 就 throw"】
//   未登录（1002）不是一个异常：顶栏要【渲染未登录形态】并继续跑，
//   判题轮询遇到 1002 要跳登录页并把草稿带过去（[D-44] T4.27）。
//   两者都不是"调用栈里没人能处理"的错误 —— 让它们 throw，调用点就得
//   everywhere 加 try/catch，而漏掉一处就变成"未登录时白屏"。
//   故：包络内的失败【一律 resolve】，判断权在调用点，这是本项目里刻意的分工。
//
// 【为什么 reject 时仍然带 code】
//   ② 的场景下 HTTP 状态码往往还在（404 页面、502 网关），
//   由它反推业务码比一律填 0 有用：调用点至少能区分"服务端返回了 404 HTML"
//   与"根本没连上"。
// =============================================================================

/** 请求失败（没有拿到统一包络）。code 由 HTTP 状态反推，未知则为 0。 */
export class ApiError extends Error {
  constructor(message, code = 0, status = 0) {
    super(message);
    this.name = 'ApiError';
    this.code = code;
    this.status = status;
  }
}

// HTTP 状态 → 业务码。只在【拿不到包络】时用它做兜底，
// 语义上不追求完备（§6.3 的码表真源在 errors.h，这里只是让前端有个可判断的数）。
const STATUS_TO_CODE = {
  400: 1001,
  401: 1002,
  403: 1003,
  404: 1004,
  409: 1005,
  413: 1001,
  429: 1006,
  500: 1008,
  503: 1009,
};

function buildQuery(query) {
  if (!query) return '';
  const parts = [];
  for (const [key, value] of Object.entries(query)) {
    if (value === undefined || value === null || value === '') continue;
    parts.push(`${encodeURIComponent(key)}=${encodeURIComponent(value)}`);
  }
  return parts.length ? `?${parts.join('&')}` : '';
}

/**
 * 发一个请求并解析统一包络。
 * @param {'GET'|'POST'|'DELETE'} method
 * @param {string} path 以 / 开头的端点路径
 * @param {{body?: object, query?: object}} [options]
 * @returns {Promise<{code:number, message:string, data:object}>}
 * @throws {ApiError} 拿不到包络时
 */
export async function request(method, path, options = {}) {
  const url = path + buildQuery(options.query);
  const init = {
    method,
    // 【必须显式写 same-origin】：sid 是 HttpOnly Cookie（[D-23]），
    // 前端 JS 读不到它，只能靠浏览器自动带。默认值本就是 same-origin，
    // 但显式写出来是为了让"为什么登录态能生效"在代码里可见 —— 将来若有人
    // 为了调跨域 API 改成 'include'，这一行就是他们会先看到的地方。
    credentials: 'same-origin',
    headers: { 'Accept': 'application/json' },
  };
  if (options.body !== undefined) {
    init.headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(options.body);
  }

  let response;
  try {
    response = await fetch(url, init);
  } catch (cause) {
    // fetch 只在网络层失败时 reject（连不上 / DNS / CORS / 中途断开）
    throw new ApiError('无法连接到服务器，请确认服务已启动', 0, 0);
  }

  // 【先判 JSON 再判状态】：静态文件 404 返回的是 HTML。
  // 直接 response.json() 会抛 SyntaxError，而那会把"页面不存在"误报成
  // "服务器内部错误"，用户会去刷新而不是去检查地址。
  let payload = null;
  try {
    payload = await response.json();
  } catch (cause) {
    throw new ApiError(`服务返回的不是 JSON（HTTP ${response.status}）`,
                       STATUS_TO_CODE[response.status] ?? 0, response.status);
  }

  if (payload === null || typeof payload !== 'object' || typeof payload.code !== 'number') {
    throw new ApiError('响应包络格式不正确', 0, response.status);
  }

  // HTTP 状态与业务码不同步时以业务码为准，但留下一条可见的信号：
  // [D-29] 要求两者同步，一旦不同步就是服务端 bug，必须能在浏览器里看出来。
  if (response.status >= 400 && payload.code === 0) {
    console.warn(`[api] HTTP ${response.status} 但业务码为 0：包络与状态码不同步`, payload);
  }

  return { code: payload.code, message: payload.message || '', data: payload.data || {} };
}

export const get = (path, query) => request('GET', path, { query });
export const post = (path, body) => request('POST', path, { body });
export const del = (path, body) => request('DELETE', path, { body });

/**
 * 会话状态。页面生命周期内只问一次 [D-52]：顶栏与概览条问的是同一个端点，
 * 各问一次会在 Network 面板里出现两条一模一样的请求，验收时反倒要解释"为什么重复"。
 * @returns {Promise<{userId:number, username:string, role:string}|null>} null = 未登录
 */
let mePromise = null;
export function currentUser() {
  if (mePromise === null) {
    mePromise = get('/api/auth/me')
      .then((reply) => (reply.code === 0 ? reply.data : null))
      // 拿不到包络时按未登录处理：顶栏退化成未登录形态而不是整页报错。
      // 这条 request 的信息仍会进 console，且不会冒泡成未捕获异常。
      .catch((error) => {
        console.warn('[api] 获取登录态失败，按未登录处理：', error.message);
        return null;
      });
  }
  return mePromise;
}

/** 登出并让会话缓存失效（否则同页面的顶栏刷新后仍是登录态）。 */
export async function logout() {
  try {
    await post('/api/auth/logout');
  } finally {
    mePromise = null;
  }
}

/**
 * 让登录态缓存失效 —— 登录之后调用。
 *
 * 【为什么登录端点里不自动清】
 *   mePromise 是**模块级**的，而登录请求本身也可能来自本页之外的调用方；
 *   在 login() 里顺手清缓存，等于让"发一个登录请求"产生一个
 *   "顺手把别人的缓存也作废"的副作用。这里显式调用，副作用范围可见。
 */
export function invalidateCurrentUser() {
  mePromise = null;
}
