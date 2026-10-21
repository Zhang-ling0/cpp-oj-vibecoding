// =============================================================================
// 提交轮询 —— SPEC §9 T4.7（轮询）/ T4.24（队列位置）/ T4.25（刷新恢复）[D-10] [D-53] [D-54]
// 规格来源：§7.3「提交后每 1.5s GET /api/submissions/{id}；status 变 done/failed 时停止」
//           §7.3「组件卸载时清除定时器」
//           §7.3「页面停留超过 5 分钟无结果则提示"评测耗时较长，可稍后在历史页查看"」
//
// 【为什么 id 存 sessionStorage 而不是 localStorage】[D-53]
//   评测态是 **per-tab** 的：在一个标签页里刷了题刷新一下要继续转圈，
//   但另一个标签页不该也转 —— 那会让用户以为有两条评测在跑。
//   localStorage 是跨标签页共享的，语义正好相反。
//
// 【队列位置每秒递减是【推算】而不是额外请求】[D-54] [D-60]
//   queueAhead 的语义是"你前面还有 N 个"。既然队列在排空，N 自然每秒减一 ——
//   为这个数字单独发一次请求，是拿一次往返换一个本来就能推出来的值。
//   ⚠️ 只做递减不做重取：真实位置由下一次 1.5s 轮询校正，
//   若中途有别人的任务插到前面，推算值会短暂偏乐观 —— 这是可接受的显示误差，
//   而【不能】改成每秒请求：那等于把"看队列"变成了压测自己的服务。
// =============================================================================

import { get } from './api.js?v=20261021-1';

const POLL_INTERVAL_MS = 1500;
const SLOW_AFTER_MS = 5 * 60 * 1000;      // 5 分钟仍未出结果 → 提示可稍后去历史页看
const JUDGING_KEY = (slug) => `judging:${slug}`;

/**
 * 启动轮询。
 * @param {{submissionId: number, slug: string, onTick: (data: object|null, info: object) => void,
 *          onFinish: (data: object) => void, onError: (error: Error, code: number) => void}} handlers
 * @returns {{stop: () => void}}
 */
export function startPolling({ submissionId, slug, onTick, onFinish, onError }) {
  const startedAt = Date.now();
  let queueAhead = 0;
  let stopped = false;

  if (slug) sessionStorage.setItem(JUDGING_KEY(slug), String(submissionId));

  // 队列位置每秒递减的独立心跳：它不携带任何数据，只驱动显示层
  const countdown = setInterval(() => {
    if (queueAhead > 0) queueAhead -= 1;
    onTick(null, { queueAhead, elapsedMs: Date.now() - startedAt });
  }, 1000);

  const timer = setInterval(async () => {
    let reply;
    try {
      reply = await get(`/api/submissions/${submissionId}`);
    } catch (cause) {
      // 网络断 ≠ 评测失败：§7.4 要求 toast + 指数退避重试，这里只提示不放弃
      onError(new Error(cause.message || '网络异常'), 0);
      return;
    }

    if (reply.code === 1002) {
      // 会话过期：草稿得带过去，否则用户回来看到的是空编辑器（[D-44]）
      stop();
      onError(new Error(reply.message || '登录状态已过期'), 1002);
      return;
    }
    if (reply.code !== 0) {
      stop();
      onError(new Error(reply.message || '查询提交状态失败'), reply.code);
      return;
    }

    const data = reply.data;
    queueAhead = 0;                      // 轮询响应不带 queueAhead，D-60 的位置只在提交响应里给一次
    onTick(data, { queueAhead, elapsedMs: Date.now() - startedAt });

    if (data.status === 'done' || data.status === 'failed') {
      stop();
      onFinish(data);
    }
  }, POLL_INTERVAL_MS);

  function stop() {
    if (stopped) return;
    stopped = true;
    clearInterval(timer);
    clearInterval(countdown);
    if (slug) sessionStorage.removeItem(JUDGING_KEY(slug));
  }

  return { stop };
}

/** 是否已经超过 5 分钟（调用方据此显示"评测耗时较长"提示）。 */
export function isSlow(elapsedMs) {
  return elapsedMs >= SLOW_AFTER_MS;
}

/**
 * 刷新后恢复轮询 [D-53]：从 sessionStorage 取本页题目上次的提交 id。
 * @returns {number|null} 提交 id；没有或已过终态返回 null
 */
export function pendingSubmissionId(slug) {
  if (!slug) return null;
  const raw = sessionStorage.getItem(JUDGING_KEY(slug));
  if (raw === null) return null;
  const id = Number(raw);
  return Number.isFinite(id) && id > 0 ? id : null;
}

/**
 * 页面加载时判断这条 pending 提交是否仍在评测中。
 * 终态（done/failed）返回 false —— 它已经出结果了，没必要重建轮询。
 */
export function isStillJudging(submissionId) {
  return get(`/api/submissions/${submissionId}`)
    .then((reply) => (reply.code === 0 &&
                      (reply.data.status === 'pending' || reply.data.status === 'judging')))
    .catch(() => false);
}

export { POLL_INTERVAL_MS, JUDGING_KEY };
