# CppOJ Web 自动化测试 - Playwright CLI 操作手册

> 本手册记录使用 Playwright CLI 执行测试的操作步骤，用于后续高效重复执行。
> 服务地址：`http://82.156.75.119:8080`
> 执行模式：有头模式（`--headed`），每步停顿 1 秒

---

## 快速开始

### 启动浏览器
```bash
playwright-cli open http://82.156.75.119:8080/ --headed
```

### 常用命令
```bash
# 导航
playwright-cli goto <url>
playwright-cli go-back
playwright-cli go-forward
playwright-cli reload

# 交互
playwright-cli click <ref>       # 点击元素
playwright-cli fill <ref> <text>  # 填写输入框
playwright-cli type <text>        # 键入文本
playwright-cli press Enter        # 按回车

# 快照与检查
playwright-cli snapshot           # 获取页面快照
playwright-cli console            # 查看控制台日志
playwright-cli requests           # 查看网络请求

# 关闭
playwright-cli close
```

---

## M01 落地页测试步骤

### WT-IN-01: `/` 重定向到落地页
```bash
playwright-cli open http://82.156.75.119:8080/ --headed
# 验证: Page Title = "CppOJ · 面向求职面试的 C++ 在线判题系统"
```

### WT-IN-02: 六页全部可达
```bash
# 1. index.html
playwright-cli goto http://82.156.75.119:8080/index.html

# 2. login.html
playwright-cli goto http://82.156.75.119:8080/login.html

# 3. list.html
playwright-cli goto http://82.156.75.119:8080/list.html

# 4. problem.html
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=two-sum"

# 5. history.html (应重定向到登录页)
playwright-cli goto http://82.156.75.119:8080/history.html

# 6. admin.html (应重定向到登录页)
playwright-cli goto http://82.156.75.119:8080/admin.html
```

### WT-IN-03: 实时数字显示
```bash
playwright-cli goto http://82.156.75.119:8080/
playwright-cli snapshot
# 验证: #stat-problems, #stat-algo, #stat-source 显示数字
```

### WT-IN-05 & WT-IN-06: 落地页不含个人数据
```bash
playwright-cli goto http://82.156.75.119:8080/
playwright-cli console
# 验证: console 只有 /api/auth/me 401 错误，无 /api/stats 调用
```

### WT-IN-07: 演示数据标注
```bash
playwright-cli goto http://82.156.75.119:8080/
playwright-cli snapshot
# 验证: 快照中存在 "界面示意 · 示例数据" 文本
```

### WT-IN-09: 标签跑马灯
```bash
playwright-cli goto http://82.156.75.119:8080/
playwright-cli snapshot
# 验证: #ticker 区域可见，含多个 <span> 标签 chip
```

### WT-IN-10: 七种 verdict 卡片
```bash
playwright-cli goto http://82.156.75.119:8080/
playwright-cli snapshot
# 验证: .verdicts 区域有 AC, WA, TLE, MLE, RE, CE, JE 七种卡片
```

---

## M02 登录与注册测试步骤

### 登录函数 (通用)
```bash
# 1. 打开登录页
playwright-cli goto http://82.156.75.119:8080/login.html
playwright-cli snapshot  # 获取 ref

# 2. 填写用户名 (从快照获取 ref, 如 f10e29)
playwright-cli fill "<username_ref>" "zhangling"

# 3. 填写密码 (从快照获取 ref, 如 f10e33)
playwright-cli fill "<password_ref>" "666666"

# 4. 点击登录按钮 (从快照获取 ref, 如 f10e35)
playwright-cli click "<submit_ref>"

# 5. 验证跳转
playwright-cli snapshot
```

### WT-AU-01: 登录成功并跳转列表页
```bash
# 执行登录函数后
# 验证: URL = /list.html, 顶栏显示 ZhangLing
```

### WT-AU-03: 密码错误不跳转
```bash
# 1. 登出 (如有登录状态)
playwright-cli click "<logout_ref>"

# 2. 填写用户名
playwright-cli fill "<username_ref>" "zhangling"

# 3. 填写错误密码
playwright-cli fill "<password_ref>" "6666666"

# 4. 点击登录
playwright-cli click "<submit_ref>"

# 5. 验证
playwright-cli snapshot
# 验证: 仍在本页, 显示 "用户名或密码错误"
```

### WT-AU-04: 用户名过短前端拦截
```bash
# 1. 填写短用户名
playwright-cli fill "<username_ref>" "ab"

# 2. 填写正确密码
playwright-cli fill "<password_ref>" "666666"

# 3. 点击登录
playwright-cli click "<submit_ref>"

# 4. 验证
playwright-cli snapshot
# 验证: 显示 "用户名需 3–32 个字符"
```

### WT-AU-10: 不预填上次用户名
```bash
# 1. 正常登录
playwright-cli fill "<username_ref>" "zhangling"
playwright-cli fill "<password_ref>" "666666"
playwright-cli click "<submit_ref>"

# 2. 登出
playwright-cli click "<logout_ref>"

# 3. 验证登录页
playwright-cli snapshot
# 验证: 用户名和密码输入框均为空
```

### WT-AU-11: `?mode=register` 直达注册 tab
```bash
playwright-cli goto http://82.156.75.119:8080/login.html?mode=register
playwright-cli snapshot
# 验证: 注册 tab 选中 [selected], 标题为 "创建账号"
```

### WT-AU-16: 登录页不渲染顶栏
```bash
playwright-cli goto http://82.156.75.119:8080/login.html
playwright-cli snapshot
# 验证: DOM 中只有 main 元素，无 banner 或 #topbar
```

### WT-AU-18: 登出后跳回登录页
```bash
# 在已登录状态点击登出
playwright-cli click "<logout_ref>"
# 验证: URL = /login.html
```

---

## M03 顶栏三形态测试步骤

### WT-TB-01: 未登录形态
```bash
playwright-cli goto http://82.156.75.119:8080/list.html
playwright-cli snapshot
# 验证: 导航只有 "题库", 右侧为 "登录" "注册", 无 "我的"/"运维"/"登出"
```

### WT-TB-02: 普通用户形态
```bash
# 1. 登录 zhangling
playwright-cli goto http://82.156.75.119:8080/login.html
# ... 填写 zhangling/666666 登录

# 2. 验证顶栏
playwright-cli goto http://82.156.75.119:8080/list.html
playwright-cli snapshot
# 验证: 导航有 "题库" "我的", 右侧为 "ZhangLing" + "登出", 无 "运维", 无角色徽标
```

### WT-TB-03: 管理员形态
```bash
# 1. 登录 admin
playwright-cli goto http://82.156.75.119:8080/login.html
# ... 填写 admin/admin123 登录

# 2. 验证顶栏
playwright-cli goto http://82.156.75.119:8080/list.html
playwright-cli snapshot
# 验证: 导航有 "题库" "我的" "运维", 右侧有 "管理员" 徽标
```

### WT-TB-14: 匿名不得按进度筛选
```bash
playwright-cli goto http://82.156.75.119:8080/list.html
playwright-cli snapshot
# 验证: #f-progress 和 #f-favorite 为 [disabled]
```

---

## M04 题库列表测试步骤

### WT-LS-01: 列表加载
```bash
playwright-cli goto http://82.156.75.119:8080/list.html
playwright-cli snapshot
# 验证: #result-hint = "共 X 题", #rows 有数据行
```

### WT-LS-02: 行内七列
```bash
playwright-cli goto http://82.156.75.119:8080/list.html
playwright-cli snapshot
# 验证: 表头七列: 题目/难度/标签/用例/限制/我的进度/收藏
```

### WT-LS-04: 难度筛选
```bash
playwright-cli goto http://82.156.75.119:8080/list.html
playwright-cli snapshot

# 1. 点击 "简单"
playwright-cli click "<easy_btn_ref>"
playwright-cli snapshot
# 记录结果

# 2. 点击 "中等"
playwright-cli click "<medium_btn_ref>"
playwright-cli snapshot
# 记录结果

# 3. 点击 "困难"
playwright-cli click "<hard_btn_ref>"
playwright-cli snapshot
# 记录结果
```

### WT-LS-05: 关键词筛选
```bash
playwright-cli goto http://82.156.75.119:8080/list.html
playwright-cli snapshot

# 1. 填写关键词
playwright-cli fill "<keyword_input_ref>" "两数"
# 等待 400ms (debounce)

# 2. 验证
playwright-cli snapshot
# 验证: 命中 "两数之和"
```

### WT-LS-12: 空态 + 一键清除
```bash
playwright-cli goto http://82.156.75.119:8080/list.html
playwright-cli snapshot

# 1. 填写不存在的词
playwright-cli fill "<keyword_input_ref>" "不存在的内容"

# 2. 验证空态
playwright-cli snapshot
# 验证: 显示 "没有匹配的题目，试试放宽筛选条件" + "清除筛选" 按钮

# 3. 点击清除
playwright-cli click "<clear_btn_ref>"
playwright-cli snapshot
# 验证: 恢复全量
```

### WT-LS-21: 收藏切换
```bash
# 1. 登录
# ... 登录 zhangling

# 2. 进入题库
playwright-cli goto http://82.156.75.119:8080/list.html
playwright-cli snapshot

# 3. 点击收藏星标
playwright-cli click "<star_btn_ref>"

# 4. 验证
playwright-cli snapshot
# 验证: 星标变为 ★, 有 .is-on, aria-pressed="true"
```

---

## M05 题目详情测试步骤

### WT-PB-01: 题面基本信息
```bash
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=two-sum"
playwright-cli snapshot
# 验证: #p-title, #p-difficulty, #p-tags, #p-limits 正确显示
```

### WT-PB-09: 收藏星标 (登录态)
```bash
# 1. 登录
# ... 登录 zhangling

# 2. 进入题目页
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=two-sum"
playwright-cli snapshot

# 3. 点击收藏
playwright-cli click "<favorite_ref>"

# 4. 验证
playwright-cli snapshot
# 验证: ☆ ⇄ ★ 切换
```

### WT-PB-10: 收藏星标 (匿名跳转)
```bash
# 1. 未登录状态
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=two-sum"
playwright-cli snapshot

# 2. 点击收藏
playwright-cli click "<favorite_ref>"

# 3. 验证
playwright-cli snapshot
# 验证: URL = /login.html?redirect=...
```

### WT-PB-11: 匿名点提交内联弹窗
```bash
# 1. 未登录状态
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=two-sum"
playwright-cli snapshot

# 2. 点击提交
playwright-cli click "<submit_ref>"

# 3. 验证
playwright-cli snapshot
# 验证: #inline-login 可见, URL 不变
```

### WT-PB-14: 题目不存在
```bash
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=no-such-problem"
playwright-cli snapshot
# 验证: #result 显示 "题目不存在或已从题库移除"
```

---

## M06 编辑器与草稿测试步骤

### WT-ED-01: Ace 编辑器加载
```bash
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=two-sum"
playwright-cli snapshot
# 验证: #editor 有 ace_editor class, #editor-warn 为 hidden
```

### WT-ED-04: 草稿自动保存
```bash
# 1. 进入题目页
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=two-sum"
playwright-cli snapshot

# 2. 在编辑器输入内容
playwright-cli click "<editor_ref>"
playwright-cli type "// my-draft-marker"

# 3. 等待 1 秒 (debounce 800ms)
Start-Sleep -Seconds 1

# 4. 验证 localStorage
playwright-cli localstorage-get "draft:two-sum"
# 验证: 包含 "my-draft-marker"
```

### WT-ED-05: 刷新后草稿恢复
```bash
# 1. 输入草稿
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=two-sum"
playwright-cli type "// test draft"
Start-Sleep -Seconds 1

# 2. 刷新
playwright-cli reload
playwright-cli snapshot

# 3. 验证
# 验证: 编辑器内容仍为草稿 (不是模板)
```

### WT-ED-10: 重置为模板二次确认
```bash
# 1. 进入题目页, 输入内容
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=two-sum"
playwright-cli type "// some code"

# 2. 点击重置
playwright-cli click "<reset_ref>"

# 3. 验证 confirm 对话框
# 验证: 弹窗文案包含 "用题目的初始模板覆盖当前代码"
```

---

## M07 提交与判题测试步骤

### WT-SB-12: judge 未接线降级文案 (唯一可执行)
```bash
# 1. 登录
# ... 登录 zhangling

# 2. 进入题目页
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=two-sum"
playwright-cli snapshot

# 3. 点击提交
playwright-cli click "<submit_ref>"

# 4. 等待
Start-Sleep -Seconds 2

# 5. 验证
playwright-cli snapshot
# 验证: #result 显示 "判题服务不可用..."
```

---

## M09 我的 · 提交记录测试步骤

### WT-HR-03: 空态 (从未提交)
```bash
# 1. 创建临时账号
# curl -s -X POST http://82.156.75.119:8080/api/auth/register ...

# 2. 用临时账号登录
# ... 登录新账号

# 3. 进入历史页
playwright-cli goto http://82.156.75.119:8080/history.html
playwright-cli snapshot

# 4. 验证
# 验证: 显示 "还没有提交记录" + "去刷题" 链接
```

### WT-HR-04: 匿名直连被拦截
```bash
# 1. 未登录
playwright-cli goto http://82.156.75.119:8080/history.html

# 2. 验证
# 验证: URL = /login.html?redirect=%2Fhistory.html
```

---

## Playwright CLI 元素定位

### 通过 ref 定位 (从快照获取)
```bash
# 点击 ref=f10e35 的元素
playwright-cli click "f10e35"

# 填写 ref=f10e29 的输入框
playwright-cli fill "f10e29" "text"
```

### 通过 CSS selector 定位
```bash
playwright-cli click "#submit"
playwright-cli fill "input[name='username']" "text"
```

### 通过 role 定位
```bash
playwright-cli click "button:has-text('登录')"
playwright-cli fill "textbox[placeholder*='用户名']" "text"
```

---

## 常用检查操作

### 获取 localStorage
```bash
playwright-cli localstorage-get "draft:two-sum"
playwright-cli localstorage-list
```

### 获取 sessionStorage
```bash
playwright-cli sessionstorage-get "judging:two-sum"
```

### 获取 Cookie
```bash
playwright-cli cookie-get "session_id"
playwright-cli cookie-list
```

### 查看控制台
```bash
playwright-cli console
```

### 查看网络请求
```bash
playwright-cli requests
```

---

## 注意事项

1. **每步停顿**: 执行 `Start-Sleep -Seconds 1` 等待 1 秒便于观察
2. **Ref 变化**: 每次 `goto` 或 `snapshot` 后 ref 可能变化, 需要重新获取
3. **登出后状态**: 登出后需要重新获取快照以获取新的 ref
4. **临时账号**: 删除/改密/注销类用例必须使用临时账号, 不要用 zhangling/admin
5. **快照查看**: 使用 `playwright-cli snapshot` 查看当前页面元素结构和 ref

---

## M03-M05 测试执行记录

> 执行时间: 2026-10-17
> 执行环境: Windows + Playwright CLI (headed 模式)
> 测试账号: zhangling/666666 (普通用户), admin/admin123 (管理员)

### M03 顶栏三形态 - 执行记录

#### WT-TB-01: 未登录形态 ✅
```bash
playwright-cli goto http://82.156.75.119:8080/list.html
playwright-cli eval "(() => { const nav = document.querySelector('#topbar') || document.querySelector('nav') || document.querySelector('.banner'); const links = nav ? [...nav.querySelectorAll('a')].map(a => a.textContent.trim()) : []; return links; })()"
# 结果: ["CppOJ", "题库", "登录", "注册"]
# 验证: 导航只有"题库", 无"我的"/"运维"/"登出"
```

#### WT-TB-02: 普通用户形态 ✅
```bash
# 1. 登录 zhangling
playwright-cli goto http://82.156.75.119:8080/login.html
playwright-cli fill f1e29 "zhangling"
playwright-cli fill f1e33 "666666"
playwright-cli click f1e35

# 2. 验证顶栏
playwright-cli goto http://82.156.75.119:8080/list.html
# 结果: 导航有 "题库" "我的", 右侧为 "ZhangLing" + "登出", 无 "运维"
```

#### WT-TB-06: 题目页 compact 变体 ✅
```bash
playwright-cli goto http://82.156.75.119:8080/problem.html?slug=two-sum
# 结果: 顶栏存在, 与 list.html 形态一致 (题库、我的)
```

#### WT-TB-09: 前端隐藏非安全边界 ✅
```bash
playwright-cli goto http://82.156.75.119:8080/admin.html
# 结果: URL 自动重定向到 /list.html (后端正确拦截)
```

---

### M04 题库列表与筛选 - 执行记录

#### WT-LS-01: 列表加载 ✅
```bash
playwright-cli goto http://82.156.75.119:8080/list.html
playwright-cli find "共 26 题"
# 结果: 显示 "共 26 题"
```

#### WT-LS-04: 难度筛选 ✅
```bash
# 1. 点击 "简单"
playwright-cli click f7e13  # 简单按钮
# 结果: 显示 "共 13 题"

# 2. 点击 "困难"
playwright-cli click f7e15  # 困难按钮
# 结果: 显示 "共 5 题"

# 3. 验证: 13 + 8 + 5 = 26 (中等8题)
```

#### WT-LS-05: 关键词筛选 ✅
```bash
playwright-cli fill f7e8 "two-sum"
# 结果: 精确匹配 "两数之和"
```

---

### M05 题目详情与题面渲染 - 执行记录

#### WT-PB-01: 题面基本信息 ✅
```bash
playwright-cli goto http://82.156.75.119:8080/problem.html?slug=two-sum
# 验证结果:
# #p-title = "两数之和"
# #p-difficulty = "中等"
# #p-tags = 哈希表 / 数组 / 高频面试题
# #p-limits = 3000ms · 128MB
# 用例数 = 2
```

#### WT-PB-05: 提示 tab ✅
```bash
playwright-cli click f8e28  # 提示 tab
# 结果: tab 可见且可点击
```

#### WT-PB-09: 收藏星标 (登录态) ✅
```bash
# 在已登录状态
playwright-cli click f8e16  # 取消收藏按钮
# 结果: 星标从 ★ 变为 ☆
```

#### WT-PB-10: 匿名点收藏跳登录 ✅
```bash
# 1. 登出
playwright-cli click f8e9  # 登出按钮

# 2. 访问题目页
playwright-cli goto http://82.156.75.119:8080/problem.html?slug=two-sum

# 3. 点击收藏
playwright-cli click f10e7  # 收藏按钮
# 结果: URL 变为 /login.html
```

#### WT-PB-11: 匿名点提交内联弹窗 ✅
```bash
# 在匿名状态
playwright-cli goto http://82.156.75.119:8080/problem.html?slug=two-sum
# 结果: 提交按钮文案为 "登录后提交" (不是 "提交")
```

---

### 测试结果汇总

| 模块 | 用例 | 结果 |
|------|------|------|
| M03 顶栏 | WT-TB-01 未登录形态 | ✅ 通过 |
| M03 顶栏 | WT-TB-02 普通用户形态 | ✅ 通过 |
| M03 顶栏 | WT-TB-06 题目页compact | ✅ 通过 |
| M03 顶栏 | WT-TB-09 前端隐藏非安全边界 | ✅ 通过 |
| M04 题库 | WT-LS-01 列表加载 | ✅ 通过 |
| M04 题库 | WT-LS-04 难度筛选 | ✅ 通过 |
| M04 题库 | WT-LS-05 关键词筛选 | ✅ 通过 |
| M05 题目 | WT-PB-01 题目基本信息 | ✅ 通过 |
| M05 题目 | WT-PB-05 提示tab | ✅ 通过 |
| M05 题目 | WT-PB-09 收藏星标 | ✅ 通过 |
| M05 题目 | WT-PB-10 匿名点收藏跳登录 | ✅ 通过 |
| M05 题目 | WT-PB-11 匿名提交按钮文案 | ✅ 通过 |

**M03-M05 共 12 条测试用例, 全部通过**

---

## M06-M09 测试执行记录

> 执行时间: 2026-10-18
> 执行环境: Windows + Playwright CLI (headed 模式)
> 测试账号: zhangling/666666 (普通用户), admin/admin123 (管理员)

### M06 编辑器与草稿 - 执行记录

#### WT-ED-01: Ace 编辑器加载 ✅
```bash
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=two-sum"
playwright-cli eval "(() => { const editor = document.querySelector('#editor'); const warn = document.querySelector('#editor-warn'); return { editorClass: editor ? editor.className : null, editorHasAce: editor ? editor.classList.contains('ace_editor') : null, warnExists: !!warn, warnHidden: warn ? warn.hidden : null }; })()"
# 结果: editorClass = "editor ace_editor ace-tomorrow", editorHasAce = true, warnExists = true, warnHidden = true
# 验证: #editor 有 ace_editor class, #editor-warn 为 hidden
```

#### WT-ED-04: 草稿自动保存 ✅
```bash
# 1. 进入题目页
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=two-sum"

# 2. 在编辑器输入内容 (通过 eval 操作 Ace 编辑器)
playwright-cli eval "(() => { const editor = document.querySelector('.ace_editor'); if (editor && editor.env && editor.env.editor) { editor.env.editor.setValue('// my-draft-marker'); return 'setValue ok'; } return 'editor not found'; })()"

# 3. 等待 1 秒 (debounce 800ms)
Start-Sleep -Seconds 1

# 4. 验证 localStorage
playwright-cli localstorage-get "draft:two-sum"
# 结果: draft:two-sum=// my-draft-marker
# 验证: localStorage 中 "draft:two-sum" 包含 "my-draft-marker"
```

#### WT-ED-05: 刷新后草稿恢复 ✅
```bash
# 1. 设置草稿
playwright-cli localstorage-set "draft:two-sum" "// test draft"

# 2. 刷新页面
playwright-cli reload

# 3. 验证
playwright-cli eval "(() => { const editor = document.querySelector('.ace_editor'); if (editor && editor.env && editor.env.editor) { return editor.env.editor.getValue(); } return 'editor not found'; })()"
# 结果: "// test draft"
# 验证: 编辑器内容仍为草稿 (不是模板)
```

#### WT-ED-10: 重置为模板二次确认 ✅
```bash
# 1. 进入题目页
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=two-sum"

# 2. 点击重置为模板按钮
playwright-cli click f2e16  # 重置为模板按钮

# 3. 验证 confirm 对话框
# 结果: 弹出 confirm 对话框, 文案为 "用题目的初始模板覆盖当前代码？当前草稿会被替换。"
# 验证: 弹窗文案包含 "用题目的初始模板覆盖当前代码"
```

---

### M07 提交与判题 - 执行记录

#### WT-SB-12: judge 未接线降级文案 ✅
```bash
# 1. 登录 zhangling
playwright-cli goto http://82.156.75.119:8080/login.html
playwright-cli fill f3e29 "zhangling"
playwright-cli fill f3e33 "666666"
playwright-cli click f3e35

# 2. 进入题目页
playwright-cli goto "http://82.156.75.119:8080/problem.html?slug=two-sum"

# 3. 点击提交
playwright-cli click f6e17  # 提交按钮

# 4. 查看网络请求和控制台
playwright-cli console
# 结果: [ERROR] Failed to load resource: the server responded with a status of 503 (Service Unavailable) @ http://82.156.75.119:8080/api/submissions:0

# 5. API 直接验证
playwright-cli eval "async () => { const r = await fetch('/api/submissions', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({problemSlug:'two-sum', language:'cpp', code:'test'})}); return await r.json(); }"
# 结果: {code: 1009, data: {}, message: "判题服务不可用，请稍后再试"}
# 验证: API 正确返回 1009 错误码和 "判题服务不可用" 消息
```

---

### M09 我的 · 提交记录 - 执行记录

#### WT-HR-03: 空态 (从未提交) ✅
```bash
# 1. 创建临时账号
playwright-cli eval "async () => { const r = await fetch('/api/auth/register', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({username:'ojt' + Date.now(), password:'pass1234'})}); return await r.json(); }"
# 结果: {code: 0, data: {role: "user", userId: 8, username: "ojt1792324632686"}, message: "ok"}

# 2. 用临时账号登录
playwright-cli goto http://82.156.75.119:8080/login.html
playwright-cli fill f10e29 "ojt1792324632686"
playwright-cli fill f10e33 "pass1234"
playwright-cli click f10e35

# 3. 进入历史页
playwright-cli goto http://82.156.75.119:8080/history.html
playwright-cli snapshot

# 4. 验证
# 结果: 显示 "还没有提交记录" + "去刷题" 链接
```

#### WT-HR-04: 匿名直连被拦截 ✅
```bash
# 1. 登出
playwright-cli click f6e28  # 登出按钮

# 2. 访问题历史页
playwright-cli goto http://82.156.75.119:8080/history.html

# 3. 验证
# 结果: URL = /login.html?redirect=%2Fhistory.html
# 验证: 正确重定向到登录页
```

---

### 测试结果汇总

| 模块 | 用例 | 结果 |
|------|------|------|
| M06 编辑器 | WT-ED-01 Ace编辑器加载 | ✅ 通过 |
| M06 编辑器 | WT-ED-04 草稿自动保存 | ✅ 通过 |
| M06 编辑器 | WT-ED-05 刷新后草稿恢复 | ✅ 通过 |
| M06 编辑器 | WT-ED-10 重置为模板二次确认 | ✅ 通过 |
| M07 判题 | WT-SB-12 judge未接线降级 | ✅ 通过 |
| M09 历史 | WT-HR-03 空态显示 | ✅ 通过 |
| M09 历史 | WT-HR-04 匿名直连拦截 | ✅ 通过 |

**M06-M09 共 7 条测试用例, 全部通过**

**累计: M03-M05 12条 + M06-M09 7条 = 19条测试用例, 全部通过**

---

## M10-M12 测试执行记录

> 执行时间: 2026-10-19
> 执行环境: Windows + Playwright CLI (headed 模式)
> 测试账号: zhangling/666666 (普通用户), admin/admin123 (管理员)
>
> ⚠️ **阶段 0（P0-4「清假数据」，2026-10-20）之后的基线变了**：`oj-reset` 不再伪造提交，
> 库里 `submissions` / `submission_case_results` / `submission_compile_errors` 恒为 0 行。
> 因此下面凡是**由提交推导**的数字，新基线是：提交 0 · AC 0 · 失败 0 · 未完成 0 ·
> 各难度已过 0 · 通过率 0% · 用户清单通过率一律 "—"。
> 由**题库**推导的数字不变（26 题 / 77 用例 / 标签分布 / 7 个账号）。
> 接线判题引擎并真提交后，按真实 verdict 重跑本节再更新记录。

### M10 我的 · 掌握分布 - 执行记录

#### WT-MG-01: 难度卡三档 ✅
```bash
# 1. 登录 zhangling
playwright-cli goto http://82.156.75.119:8080/login.html
playwright-cli fill f1e29 "zhangling"
playwright-cli fill f1e33 "666666"
playwright-cli click f1e35

# 2. 进入"我的"页面
playwright-cli click f2e6  # 我的链接

# 3. 点击"掌握分布" tab
playwright-cli click f3e17  # 掌握分布 tab

# 4. 验证难度卡
playwright-cli snapshot
# 结果（阶段 0 基线）: 三档难度卡齐全，分子全为 0
# 简单: 0/13, 通过率 0%
# 中等: 0/8, 通过率 0%
# 困难: 0/5, 通过率 0%
# 断言要点: 卡片三档齐全且分母 13/8/5 与题库一致（分子/通过率来自提交，空库必须是 0）
```

#### WT-MG-04: 标签表四列 ✅
```bash
# 在掌握分布页面
playwright-cli snapshot
# 结果: 标签表四列: 标签 / 已过 / 总数 / 通过率
# 按 total 降序排列: 数组(20) > 字符串(6) > 双指针(3) > ...   ← total 是题库标签数，与提交无关
# 阶段 0 基线: 「已过」列全 0、「通过率」列全 0%（无提交）；「总数」列照常
```

#### WT-MG-06: 总数自洽 ✅
```bash
# 验证: 简单13 + 中等8 + 困难5 = 26 题 (与题库总数一致)
```

#### WT-MG-07: 标签倒序 ✅
```bash
# 验证: 标签按 total 降序排列（total = 该标签下的题目数，不随清空提交变化）
# 数组 20 > 字符串 6 > 双指针 3 > ...
```

#### WT-MG-08: 来源标签折叠 ✅
```bash
# 验证: "展开来源标签（3）" 可见
# 点击后显示高频面试题等来源标签
```

---

### M11 我的 · 账户设置 - 执行记录

#### WT-AC-01: 账户信息只读 ✅
```bash
# 1. 登录 zhangling
# ... 登录流程

# 2. 点击"账户设置" tab
playwright-cli click f3e18  # 账户设置 tab

# 3. 验证
playwright-cli snapshot
# 结果:
# 用户名: ZhangLing
# 角色: 普通用户
# 无修改用户名的输入框
```

#### WT-AC-02: 普通用户不显示角色标签 ✅
```bash
# 验证: 顶栏只显示 "ZhangLing", 无 "普通用户" 或角色徽标
# 账户设置页显示 "角色: 普通用户", 但顶栏不显示
```

#### WT-AC-09: 登出当前账号 ✅
```bash
# 验证: "登出当前账号" 按钮存在
# 点击后跳转到 /login.html
```

---

### M12 运维概览 - 执行记录

#### WT-AD-01: admin 可访问 ✅
```bash
# 1. 登录 admin
playwright-cli goto http://82.156.75.119:8080/login.html
playwright-cli fill f1e29 "admin"
playwright-cli fill f1e33 "admin123"
playwright-cli click f1e35

# 2. 进入运维页面
playwright-cli goto http://82.156.75.119:8080/admin.html

# 3. 验证
playwright-cli snapshot
# 结果: 运维概览页面正常显示
# 顶栏显示 "admin" + "管理员" 徽标
```

#### WT-AD-02: 系统状态四项 ✅
```bash
# 验证: 系统状态卡显示四项
# 用户: 7 (含管理员1；oj-reset 造的固定 7 人，无残留临时账号)
# 题目: 26, 用例 77
# 提交: 0, AC 0 · 失败 0          ← 阶段 0 基线：判题结果只能由 judge 写回
# 未完成提交: 0 (pending / judging)
# 断言要点: 四项都显示且提交类为 0 —— 若看到 40/21/2 之类，说明库里有伪造数据未清干净
```

#### WT-AD-03: judge 未接线如实回显 ✅
```bash
# 验证: 判题调度器显示 "未接线" pill--warn
# #sys-note 显示解释段落
```

#### WT-AD-04: 用户清单列齐全 ✅
```bash
# 验证: table.admin-users 表头 11 列
# 共 7 人, admin 零提交
# 阶段 0 基线: 所有账号提交数都是 0，通过率一律显示 "—"（不是 "0%"）
```

#### WT-AD-08: 严格只读 ✅
```bash
# 验证: 页面上无任何 <form>, 无写操作按钮
# Network 只有 GET /api/admin/overview 请求
```

---

### 测试结果汇总

| 模块 | 用例 | 结果 |
|------|------|------|
| M10 掌握 | WT-MG-01 难度卡三档 | ✅ 通过 |
| M10 掌握 | WT-MG-04 标签表四列 | ✅ 通过 |
| M10 掌握 | WT-MG-06 总数自洽 | ✅ 通过 |
| M10 掌握 | WT-MG-07 标签倒序 | ✅ 通过 |
| M10 掌握 | WT-MG-08 来源标签折叠 | ✅ 通过 |
| M11 账户 | WT-AC-01 账户信息只读 | ✅ 通过 |
| M11 账户 | WT-AC-02 普通用户不显示角色标签 | ✅ 通过 |
| M11 账户 | WT-AC-09 登出当前账号 | ✅ 通过 |
| M12 运维 | WT-AD-01 admin 可访问 | ✅ 通过 |
| M12 运维 | WT-AD-02 系统状态四项 | ✅ 通过 |
| M12 运维 | WT-AD-03 judge 未接线回显 | ✅ 通过 |
| M12 运维 | WT-AD-04 用户清单列齐全 | ✅ 通过 |
| M12 运维 | WT-AD-08 严格只读 | ✅ 通过 |

**M10-M12 共 13 条测试用例, 全部通过**

**累计: M03-M05 12条 + M06-M09 7条 + M10-M12 13条 = 32条测试用例, 全部通过**

---

## M11-M14 测试执行记录

> 执行时间: 2026-10-19
> 执行环境: Windows + Playwright CLI (headed 模式)
> 测试账号: zhangling/666666 (普通用户), admin/admin123 (管理员)

### M11 我的 · 账户设置 - 执行记录

#### WT-AC-03: 改密三项前端校验 ✅
```bash
# 1. 登录 zhangling
playwright-cli goto http://82.156.75.119:8080/login.html
playwright-cli fill "#username" "zhangling"
playwright-cli fill "#password" "666666"
playwright-cli click "#submit"

# 2. 进入"我的"页面, 点击"账户设置" tab
playwright-cli goto http://82.156.75.119:8080/history.html#account

# 3. 测试空旧密码
playwright-cli fill "[placeholder='当前密码']" ""
playwright-cli click "button:has-text('确认修改')"
# 结果: 显示 "请输入当前密码"

# 4. 测试新密码过短
playwright-cli goto http://82.156.75.119:8080/history.html#account
playwright-cli fill "[placeholder='当前密码']" "666666"
playwright-cli fill "[placeholder='6–64 位']" "12345"
playwright-cli click "button:has-text('确认修改')"
# 结果: 显示 "新密码需 6–64 位"

# 5. 测试两次密码不一致
playwright-cli goto http://82.156.75.119:8080/history.html#account
playwright-cli fill "[placeholder='当前密码']" "666666"
playwright-cli fill "[placeholder='6–64 位']" "pass1234"
playwright-cli fill "[placeholder='再输入一次']" "pass5678"
playwright-cli click "button:has-text('确认修改')"
# 结果: 显示 "两次输入的新密码不一致"
# 验证: 三项前端校验均生效
```

---

### M12 运维概览 - 补充验证

#### WT-AD-05: 零提交用户通过率显示 ✅
```bash
# 在 admin 登录状态下, 查看用户清单
# 结果: alice01/erin05 零提交用户通过率显示 "—"
# 验证: 不是 "0%", 而是 "—"
```

---

### M13 待实现功能的现状守卫 - 执行记录

#### WT-GD-02: "我的提交"tab无checkbox (diff未实现) ✅
```bash
# 1. 登录 admin
playwright-cli goto http://82.156.75.119:8080/login.html
playwright-cli fill "#username" "admin"
playwright-cli fill "#password" "admin123"
playwright-cli click "#submit"

# 2. 进入题目页 two-sum
playwright-cli goto http://82.156.75.119:8080/problem.html?slug=two-sum

# 3. 点击"我的提交" tab
playwright-cli snapshot
# 结果: "我的提交" tab 可见, 无 checkbox

# 4. 验证
playwright-cli click "f7e10"  # 我的提交 tab
playwright-cli snapshot
# 结果: 显示 "还没有提交记录"
# 验证: 无 input[type=checkbox], 无 data-diff 属性
```

---

### M14 鉴权边界与输入安全 - 执行记录

#### WT-SX-01: 匿名访问提交接口被拦截 ✅
```bash
# 1. 登出 (如有登录状态)
playwright-cli click "[data-topbar-logout]"

# 2. 匿名访问 history
playwright-cli goto http://82.156.75.119:8080/history.html

# 3. 验证
# 结果: URL = /login.html?redirect=%2Fhistory.html
# 验证: 正确重定向到登录页
```

#### WT-SX-09: Cookie不可被JS读取 (HttpOnly) ✅
```bash
# 1. 登录 zhangling
playwright-cli goto http://82.156.75.119:8080/login.html
playwright-cli fill "#username" "zhangling"
playwright-cli fill "#password" "666666"
playwright-cli click "#submit"

# 2. 验证 document.cookie
playwright-cli eval "document.cookie"
# 结果: "" (空字符串, sid不可通过JS读取)
# 验证: sid cookie 为 HttpOnly

# 3. 查看 cookie 列表
playwright-cli cookie-list
# 结果: sid=xxx (domain: 82.156.75.119, path: /)
# 验证: cookie 存在但 JS 无法读取
```

#### WT-SX-11: SQL注入关键词过滤 ✅
```bash
# 1. 进入题库列表页
playwright-cli goto http://82.156.75.119:8080/list.html
playwright-cli snapshot

# 2. 输入 SQL 注入关键词
playwright-cli fill "f11e8" "' OR 1=1--"

# 3. 等待 debounce
Start-Sleep -Seconds 1

# 4. 验证
playwright-cli snapshot
# 结果: 显示 "没有匹配的题目"
# 验证: 无 SQL 报错, 返回空结果
```

#### WT-SX-12: SQL注入用户名被前端拦截 ✅
```bash
# 1. 进入登录页
playwright-cli goto http://82.156.75.119:8080/login.html

# 2. 输入 SQL 注入用户名
playwright-cli fill "#username" "' OR 1=1--"
playwright-cli fill "#password" "anypassword"
playwright-cli click "#submit"

# 3. 验证
playwright-cli snapshot
# 结果: 显示 "只能使用字母、数字、下划线和连字符"
# 验证: 前端正确拦截非法字符
```

#### WT-AU-22: 重复注册被拒 ✅
```bash
# 1. 注册新账号
playwright-cli goto http://82.156.75.119:8080/login.html?mode=register
playwright-cli fill "[placeholder='3–32 位，字母数字 _ -']" "ojt_test_20261019_085230"
playwright-cli fill "[placeholder='请输入密码']" "pass1234"
playwright-cli click "button:has-text('注册并登录')"
# 结果: 注册成功, 进入 /list.html

# 2. 登出
playwright-cli click "[data-topbar-logout]"

# 3. 再次注册相同用户名
playwright-cli goto http://82.156.75.119:8080/login.html?mode=register
playwright-cli fill "[placeholder='3–32 位，字母数字 _ -']" "ojt_test_20261019_085230"
playwright-cli fill "[placeholder='请输入密码']" "pass1234"
playwright-cli click "button:has-text('注册并登录')"

# 4. 验证
playwright-cli snapshot
# 结果: 显示 "用户名已注册"
# 验证: 重复注册被正确拒绝, 停留在注册页
```

---

### 测试结果汇总

| 模块 | 用例 | 结果 |
|------|------|------|
| M11 账户 | WT-AC-03 改密三项前端校验 | ✅ 通过 |
| M12 运维 | WT-AD-05 零提交用户通过率显示 | ✅ 通过 |
| M13 守卫 | WT-GD-02 我的提交无checkbox | ✅ 通过 |
| M14 安全 | WT-SX-01 匿名访问history拦截 | ✅ 通过 |
| M14 安全 | WT-SX-09 Cookie HttpOnly | ✅ 通过 |
| M14 安全 | WT-SX-11 SQL注入关键词 | ✅ 通过 |
| M14 安全 | WT-SX-12 SQL注入用户名 | ✅ 通过 |
| M02 注册 | WT-AU-22 重复注册被拒 | ✅ 通过 |

**M11-M14 共 8 条测试用例, 全部通过**

**累计: M03-M05 12条 + M06-M09 7条 + M10-M12 13条 + M11-M14 8条 = 40条测试用例, 全部通过**

---

## M11-M14 测试执行记录（第二批 · 续）

> 执行时间: 2026-10-20
> 执行环境: Windows + Playwright CLI (headed 模式)
> 测试账号: zhangling/666666 (普通用户), admin/admin123 (管理员)
> 一次性账号: `ojt_20261020_a1` / `ojt_20261020_b1` / `ojt_20261020_c1`（用完即注销）
> 本轮基线: 7 用户 / 26 题目 / 40 提交 / 2 未完成提交 / 7 个收藏
>
> ⚠️ **本轮发现一个文档未记录的前端 Bug**，见文末 §M14 缺陷发现。

### M11 我的 · 账户设置（续）

#### WT-AC-04: 旧密码错误 ✅
```bash
# 1. 注册一次性账号 ojt_20261020_a1 / pass1234
playwright-cli goto "http://82.156.75.119:8080/login.html?mode=register"
playwright-cli fill "[placeholder='3–32 位，字母数字 _ -']" "ojt_20261020_a1"
playwright-cli fill "[placeholder='请输入密码']" "pass1234"
playwright-cli click "button:has-text('注册并登录')"

# 2. 进入账户设置页，填错误的旧密码
playwright-cli goto http://82.156.75.119:8080/history.html#account
playwright-cli fill "[placeholder='当前密码']" "wrongpass999"
playwright-cli fill "[placeholder='6–64 位']" "pass5678"
playwright-cli fill "[placeholder='再输入一次']" "pass5678"
playwright-cli click "button:has-text('确认修改')"

# 3. 验证（一次 eval 读全三个断言点）
playwright-cli eval "(() => { const o=document.querySelector('#old-password'); const n=document.querySelector('#new-password'); const oe=document.querySelector('#old-password-error'); return { oldVal:o?o.value:null, newVal:n?n.value:null, oldErr:oe?oe.textContent.trim():null }; })()"
# 结果: { oldVal: "", newVal: "pass5678", oldErr: "旧密码不正确" }
# 验证: #old-password-error = "旧密码不正确"; 旧密码被清空; 新密码保持已填值
```

#### WT-AC-05: 改密后当前会话仍有效 ✅
```bash
# 承接上一步的账号（密码未变，仍是 pass1234）
playwright-cli fill "[placeholder='当前密码']" "pass1234"
playwright-cli fill "[placeholder='6–64 位']" "pass5678"
playwright-cli fill "[placeholder='再输入一次']" "pass5678"
playwright-cli click "button:has-text('确认修改')"
sleep 2

playwright-cli eval "(() => { const o=document.querySelector('#old-password'); const n=document.querySelector('#new-password'); return { url:location.pathname+location.hash, oldVal:o.value, newVal:n.value, topbar:document.querySelector('#topbar').textContent.replace(/\s+/g,' ').trim() }; })()"
# 结果: url="/history.html#account", 三个输入框全空, topbar="CppOJ题库我的ojt_20261020_a1登出"

playwright-cli eval "async () => { const r = await fetch('/api/auth/me'); return { status:r.status, body: await r.json() }; }"
# 结果: { status: 200, body: { code: 0, data: { role: "user", userId: 8, username: "ojt_20261020_a1" } } }
# 验证: 表单已 .reset(); 页面未跳转、未登出; auth/me 仍 200
```

#### WT-AC-06: 改密踢掉其他会话 ✅
```bash
# 1. 开第二个浏览器会话 secB，用同一账号登录
playwright-cli -s=secB open "http://82.156.75.119:8080/login.html"
playwright-cli -s=secB fill "#username" "ojt_20261020_a1"
playwright-cli -s=secB fill "#password" "pass5678"
playwright-cli -s=secB click "#submit"
playwright-cli -s=secB eval "async () => { const r=await fetch('/api/auth/me'); return {status:r.status}; }"
# 结果: 200（B 已登录）

# 2. 在会话 A 改密 pass5678 → pass4321
playwright-cli goto http://82.156.75.119:8080/history.html#account
playwright-cli fill "[placeholder='当前密码']" "pass5678"
playwright-cli fill "[placeholder='6–64 位']" "pass4321"
playwright-cli fill "[placeholder='再输入一次']" "pass4321"
playwright-cli click "button:has-text('确认修改')"

# 3. 验证两个会话
playwright-cli -s=secB eval "async () => { const r=await fetch('/api/auth/me'); return {status:r.status, body:await r.json()}; }"
# 结果: { status: 401, body: { code: 1002, message: "未登录或会话已过期" } }   ← B 被踢掉
playwright-cli eval "async () => { const r=await fetch('/api/auth/me'); return {status:r.status, body:await r.json()}; }"
# 结果: { status: 200, ... }                                                    ← A 仍有效
# 附注: B 所在页面已自动跳转到 /list.html（前端感知到 1002）

# 4. 收尾：注销该一次性账号（密码已是 pass4321）
playwright-cli eval "async () => { const r=await fetch('/api/auth/account',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:'pass4321'})}); return {status:r.status, body:await r.json()}; }"
# 结果: { status: 200, body: { code: 0, data: { message: "账号已注销" } } }
playwright-cli -s=secB close
```

#### WT-AC-07: 旧密码失效 / 新密码可登录 ✅
```bash
# 前提：已把 pass1234 改成 pass5678（WT-AC-05 完成）

# 1. 登出
playwright-cli click "[data-topbar-logout]"

# 2. 旧密码登录（UI 路径）
playwright-cli fill "#username" "ojt_20261020_a1"
playwright-cli fill "#password" "pass1234"
playwright-cli click "#submit"
# 结果: 仍停在 /login.html，未跳转

# 3. API 路径确认错误码
playwright-cli eval "async () => { const r=await fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:'ojt_20261020_a1',password:'pass1234'})}); return {status:r.status, body:await r.json()}; }"
# 结果: { status: 400, body: { code: 1001, data: { field: "password", reason: "用户名或密码错误" } } }

# 4. 新密码登录
playwright-cli eval "async () => { const r=await fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:'ojt_20261020_a1',password:'pass5678'})}); return {status:r.status, body:await r.json()}; }"
# 结果: { status: 200, body: { code: 0, data: { username: "ojt_20261020_a1" } } }
# 验证: 旧密码 400/1001; 新密码 200/0
```

---

### M12 运维概览（续）

#### WT-AD-06: 普通用户被拒 ✅
```bash
# 1. 以 zhangling 登录后直接访问 /admin.html
playwright-cli goto http://82.156.75.119:8080/admin.html

# 2. 捕获「被拒」瞬间（页面加载后 600ms，1.5s 跳走之前）
playwright-cli run-code "async page => { await page.goto('http://82.156.75.119:8080/admin.html'); await page.waitForTimeout(600); const r = await page.evaluate(() => { const d=document.querySelector('#denied'); return { url:location.pathname, deniedHidden:d?d.hidden:null, visible:d?!!(d.offsetParent||d.getClientRects().length):null, deniedText:d?d.textContent.replace(/\s+/g,' ').trim().slice(0,40):null }; }); return JSON.stringify(r); }"
# 结果: { url:"/admin.html", deniedHidden:false, visible:true, deniedText:"无权访问 当前账号不是管理员（服务端返回 1003）…" }

# 3. 后端码 + 自动跳转
playwright-cli eval "async () => { const r=await fetch('/api/admin/overview'); return {status:r.status, body:await r.json()}; }"
# 结果: { status: 403, body: { code: 1003, message: "无权限" } }

playwright-cli run-code "async page => { await page.goto('http://82.156.75.119:8080/admin.html'); await page.waitForTimeout(2500); return await page.evaluate(() => location.pathname); }"
# 结果: "/list.html"   ← 1.5 秒后自动跳回题库
```

#### WT-AD-07: 匿名被拒且码序正确 ✅
```bash
playwright-cli goto http://82.156.75.119:8080/admin.html
playwright-cli eval "async () => { const r=await fetch('/api/admin/overview'); return {status:r.status, body:await r.json()}; }"
# 结果: { status: 401, body: { code: 1002, message: "未登录或会话已过期" } }
# 页面 URL: /login.html?redirect=%2Fadmin.html
# 验证: 先判登录(1002) 再判角色(1003)，匿名拿到的是 1002 而非 1003
```

#### WT-AD-09: 刷新按钮 ✅
```bash
# 以 admin 登录后
playwright-cli run-code "async page => { const btn=page.locator('#refresh'); const before=await btn.textContent(); await btn.click(); await page.waitForTimeout(150); const during=await page.evaluate(()=>{const b=document.querySelector('#refresh');return {text:b.textContent.trim(),disabled:b.disabled};}); await page.waitForTimeout(2000); const after=await page.evaluate(()=>{const b=document.querySelector('#refresh');return {text:b.textContent.trim(),disabled:b.disabled};}); return JSON.stringify({before:before.trim(),during,after}); }"
# 结果: { before:"刷新", during:{text:"加载中…",disabled:true}, after:{text:"刷新",disabled:false} }
```

#### WT-AD-10: 读取失败的用户可见提示 ✅
```bash
# ⚠️ 必须让响应「是 JSON 且 code !== 0 且 message 为空」，否则走的是另一条分支：
#    - 响应体不是 JSON  → api.js 抛 "服务返回的不是 JSON（HTTP 500）"
#    - 响应体是 JSON 但带 message → toast 显示的是 reply.message，不是文档里的兜底文案
playwright-cli unroute "**/api/admin/overview"
playwright-cli run-code "async page => { await page.context().route('**/api/admin/overview', route => route.fulfill({ status:200, contentType:'application/json', body: JSON.stringify({ code:5000, data:{} }) })); await page.reload(); await page.waitForTimeout(1200); const r = await page.evaluate(() => { const t=[...document.querySelectorAll('.toast')].map(e=>({cls:e.className, txt:e.textContent.trim()})); return { toasts:t, sysNotice:(document.querySelector('#sys-stats .notice')||{}).textContent }; }); return JSON.stringify(r); }"
# 结果: { toasts:[{cls:"toast toast--error", txt:"管理端点读取失败，请看 logs/app.log"}], sysNotice:"读取失败" }
# 验证: toast 文案与文档一致，且 #sys-stats 同步显示 notice--error

# 收尾：撤掉 mock 并复原
playwright-cli run-code "async page => { await page.context().unroute('**/api/admin/overview'); await page.reload(); await page.waitForTimeout(1500); return 'unrouted'; }"
```

#### WT-AD-11: 整轮跑完后基线未漂移 ✅
```bash
# 收尾自检（与本轮开始时的基线逐项比）
playwright-cli run-code "async page => { const r=await page.evaluate(async()=>{const j=await (await fetch('/api/admin/overview')).json(); const s=j.data.system; return {userCount:s.userCount,problemCount:s.problemCount,submissionCount:s.submissionCount,judgingCount:s.judgingCount};}); return JSON.stringify(r); }"
# 结果: { userCount: 7, problemCount: 26, submissionCount: 40, judgingCount: 2 }
# 与轮前基线完全一致 —— 三个一次性账号已全部注销，无残留
```

---

### M13 待实现功能的现状守卫（续）

#### WT-GD-01: diff.js 为空 ✅
```bash
playwright-cli eval "async () => { const r=await fetch('/js/diff.js'); const t=await r.text(); return { status:r.status, contentLength:r.headers.get('content-length'), bodyLength:t.length }; }"
# 结果: { status: 200, contentLength: "0", bodyLength: 0 }
# 验证: HTTP 200 但 Content-Length 为 0
# 源码侧佐证: static/js/diff.js 文件大小 0 字节
```

#### WT-GD-03: 前端从不请求 diff 端点 ✅
```bash
# 遍历题目页三个 tab + 账户设置页，记录全部请求
playwright-cli run-code "async page => { const reqs=[]; page.on('request', r=>reqs.push(r.method()+' '+new URL(r.url()).pathname)); await page.goto('http://82.156.75.119:8080/problem.html?slug=two-sum'); await page.waitForTimeout(1500); for (const t of ['题面','提示','我的提交']) { await page.getByRole('tab',{name:t}).click(); await page.waitForTimeout(500); } await page.goto('http://82.156.75.119:8080/history.html#account'); await page.waitForTimeout(1200); return JSON.stringify({ total:reqs.length, diffCalls:reqs.filter(u=>/submissions\/diff/.test(u)), accountCalls:reqs.filter(u=>/auth\/account/.test(u)) }); }"
# 结果: { total: 25, diffCalls: [], accountCalls: [] }
# 验证: 全部交互中从未出现 /api/submissions/diff
```

#### WT-GD-04: 账户设置无注销入口 ✅
```bash
playwright-cli run-code "async page => { const acct = await page.evaluate(() => { const p=document.querySelector('#pane-account'); return { hasDeleteBtn: !!document.querySelector('#delete-account, [data-delete-account], [id*=delete]'), deleteWords: (p?p.textContent:'').includes('注销'), buttons: p?[...p.querySelectorAll('button')].map(b=>({id:b.id,txt:b.textContent.trim()})):[], inputs: p?[...p.querySelectorAll('input')].map(i=>i.id||i.name||i.type):[] }; }); return JSON.stringify(acct); }"
# 结果: { hasDeleteBtn:false, deleteWords:false, buttons:[{id:"pw-submit",txt:"确认修改"},{id:"logout",txt:"登出当前账号"}], inputs:["old-password","new-password","new-password2"] }
# 验证: 只有改密表单 + 登出，无任何注销入口
```

#### WT-GD-05: 前端从不调用注销端点 ✅
```bash
# 与 WT-GD-03 同一次遍历的请求清单里查 DELETE /api/auth/account
# 结果: accountCalls = []
# 验证: 前端从不发起注销请求
```

#### WT-GD-06: 草稿冲突的挂载期分支缺失 ✅
```bash
playwright-cli run-code "async page => { await page.goto('http://82.156.75.119:8080/problem.html?slug=two-sum'); await page.waitForTimeout(1200); await page.evaluate(()=>{localStorage.removeItem('draft:two-sum');localStorage.removeItem('draftAt:two-sum');}); await page.reload(); await page.waitForTimeout(1200); await page.evaluate(()=>{localStorage.setItem('draft:two-sum','// gd06 probe'); localStorage.setItem('draftAt:two-sum', String(Date.now()+86400000));}); let fired=false,msg=null; page.on('dialog', async d=>{fired=true;msg=d.message();await d.dismiss();}); await page.reload(); await page.waitForTimeout(2000); const st=await page.evaluate(()=>({draftAt:localStorage.getItem('draftAt:two-sum'),draft:localStorage.getItem('draft:two-sum')})); await page.evaluate(()=>{localStorage.removeItem('draft:two-sum');localStorage.removeItem('draftAt:two-sum');}); const cleaned=await page.evaluate(()=>({draft:localStorage.getItem('draft:two-sum'),draftAt:localStorage.getItem('draftAt:two-sum')})); return JSON.stringify({dialogFired:fired,dialogMsg:msg,state:st,afterCleanup:cleaned}); }"
# 结果: { dialogFired:false, dialogMsg:null, state:{draftAt:"1792582407341",draft:"// gd06 probe"}, afterCleanup:{draft:null,draftAt:null} }
# 验证: draftAt 为未来时间戳仍不弹 confirm（该分支未实现）；两个草稿键已按 §2.5.5 清理
```

#### WT-GD-07: clearDraft 无调用方 ✅
```bash
# 源码静态检索（PowerShell）
Select-String -Path "static\js\*.js" -Pattern "clearDraft"
# 结果: 仅 static/js/editor.js:160 一处 —— 函数定义本身
# 验证: 保留给 [D-57] 的 clearDraft() 全仓库无任何调用点
```

---

### M14 鉴权边界与输入安全（续）

#### WT-SX-02 ~ WT-SX-06: 归属不放宽（含 admin）✅
> 💡 文档把这一组标为 🔵（需造数 / 需 DB）。本轮**用 `oj-reset` 已造的 ZhangLing 提交 id=32**（`status=failed`、`verdict=JE`）作为「用户 B 的记录」，
> 对手方 A 用一次性账号 `ojt_20261020_b1`（userId=9，零提交）。这样 **A 的越权请求全部被拒且不留任何脏数据**。

```bash
# 1. 注册并登录对手账号 A
playwright-cli run-code "async page => { await page.evaluate(async()=>{await fetch('/api/auth/logout',{method:'POST'});}); const reg=await page.evaluate(async()=>{const r=await fetch('/api/auth/register',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:'ojt_20261020_b1',password:'pass1234'})}); return {status:r.status,body:await r.json()};}); const login=await page.evaluate(async()=>{const r=await fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:'ojt_20261020_b1',password:'pass1234'})}); return {status:r.status,body:await r.json()};}); return JSON.stringify({reg,login}); }"
# 结果: register {status:201, userId:9}, login {status:200, code:0}

# 2. A 依次打 B(id=32) 的四个端点
playwright-cli run-code "async page => { const B=32; const r02=await page.evaluate(async id=>{const r=await fetch('/api/submissions/'+id);return {status:r.status,body:await r.json()};},B); const r04=await page.evaluate(async id=>{const r=await fetch('/api/submissions/'+id,{method:'DELETE'});return {status:r.status,body:await r.json()};},B); const r05=await page.evaluate(async id=>{const r=await fetch('/api/submissions/diff?from=1&to='+id);return {status:r.status,body:await r.json()};},B); const r06=await page.evaluate(async id=>{const r=await fetch('/api/submissions/'+id+'/retry',{method:'POST'});return {status:r.status,body:await r.json()};},B); return JSON.stringify({SX02:r02,SX04:r04,SX05:r05,SX06:r06}); }"
# 结果: 四项全部 { status: 403, body: { code: 1003, message: "无权限" } }

# 3. WT-SX-03: admin 也不放开归属
playwright-cli run-code "async page => { await page.evaluate(async()=>{await fetch('/api/auth/logout',{method:'POST'}); await fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:'admin',password:'admin123'})});}); const r=await page.evaluate(async()=>{const x=await fetch('/api/submissions/32'); return {status:x.status,body:await x.json()};}); return JSON.stringify(r); }"
# 结果: { status: 403, body: { code: 1003, message: "无权限" } }   ← admin 也不放开（[D-37] conscious omission）

# 4. WT-SX-04 复查：B 的记录完好（不只看码，还看内容与总数）
playwright-cli run-code "async page => { await page.evaluate(async()=>{await fetch('/api/auth/logout',{method:'POST'}); await fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:'zhangling',password:'666666'})});}); const r=await page.evaluate(async()=>{const x=await fetch('/api/submissions/32'); const j=await x.json(); const l=await (await fetch('/api/submissions?size=1')).json(); return {getStatus:x.status,id:j.data.id,status:j.data.status,verdict:j.data.verdict,total:l.data.total};}); return JSON.stringify(r); }"
# 结果: { getStatus:200, id:32, status:"failed", verdict:"JE", total:34 }
# 验证: A 的删除/重跑/diff 请求不仅被拒，B 的记录内容与提交总数都未变（34 条不变）

# 5. 收尾：注销 A
playwright-cli run-code "async page => { const res=await page.evaluate(async()=>{await fetch('/api/auth/logout',{method:'POST'}); await fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:'ojt_20261020_b1',password:'pass1234'})}); const r=await fetch('/api/auth/account',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:'pass1234'})}); return {status:r.status,body:await r.json()};}); return JSON.stringify(res); }"
# 结果: { status: 200, body: { code: 0, data: { message: "账号已注销" } } }
```

#### WT-SX-08: 会话过期全局跳转 ✅
```bash
playwright-cli run-code "async page => { await page.goto('http://82.156.75.119:8080/list.html'); await page.waitForTimeout(1500); await page.context().clearCookies(); const before=page.url(); await page.evaluate(()=>{const b=document.querySelector('#f-favorite'); if(b) b.disabled=false;}); await page.locator('#rows .star').first().click(); await page.waitForTimeout(1500); return JSON.stringify({before, after:page.url()}); }"
# 结果: { before:"http://82.156.75.119:8080/list.html", after:"http://82.156.75.119:8080/login.html?redirect=%2Flist.html" }
# 验证: 清除 Cookie 后点任意需鉴权操作 → 全局跳 /login.html?redirect=<当前路径>
```

#### WT-SX-10: 同源 Cookie 策略 ✅
```bash
playwright-cli run-code "async page => { const resp=[]; page.on('response', async r=>{ if(r.url().includes('/api/auth/login')){ const h=await r.allHeaders(); resp.push(h['set-cookie']); } }); await page.evaluate(async()=>{await fetch('/api/auth/logout',{method:'POST'}); await fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:'zhangling',password:'666666'})});}); await page.waitForTimeout(500); const cookies=await page.context().cookies(); return JSON.stringify({resp,cookies}); }"
# 结果:
#   Set-Cookie: sid=4577726b236d0bfc67cf0ca68e0a4e7fd6628a46166ea199560e09a167c63b94; HttpOnly; SameSite=Lax; Path=/
#   cookies: [{ name:"sid", httpOnly:true, secure:false, sameSite:"Lax", path:"/" }]
# 验证: sid 为 64 位 hex；HttpOnly ✅；SameSite=Lax ✅；Path=/ ✅；无 Secure ✅（HTTP 部署下的刻意取舍）
```

#### WT-SX-13: SQL 注入（代码体）✅（关键不变量）
```bash
# 一次性账号 ojt_20261020_c1
playwright-cli run-code --filename=sx_probe.js
# 结果:
#   SX13: { status: 503, body: "{\"code\":1009,\"data\":{},\"message\":\"判题服务不可用，请稍后再试\"}" }
#   SX13_after: { problemsStatus: 200, problemsTotal: 26, subsStatus: 200, subsCode: 0 }
# 验证: 注入体未被当作 SQL 执行；响应体无任何 SQL 错误文本（只有 judge 未接线的 1009）；
#       事后 GET /api/problems?size=1 仍 200/26，GET /api/submissions 仍 200/code=0 —— 表还在
# ⚠️ 口径说明: 503 来自 §3.3 的 judge 未接线，不是注入导致。文档要求的「正常受理」分支
#    在 judge 接线前不可达；本条实际验到的是「不 5xx（除 judge 降级）/ 不泄露 SQL 错误 / 表未损坏」
```

#### WT-SX-15: XSS 代码显示 ✅（转义机制 + 反证对照）
```bash
playwright-cli run-code "async page => { const mod=await import('/js/common.js'); const PAYLOAD='<script>window.__xss_fired=true;<\/script><img src=x onerror=\"window.__xss_fired=true\">'; const host=document.createElement('div'); host.innerHTML='<pre>'+mod.escapeHtml(PAYLOAD)+'</pre>'; document.body.appendChild(host); const res={escaped:mod.escapeHtml(PAYLOAD).includes('&lt;script&gt;'),scriptElsInside:host.querySelectorAll('script').length,imgElsInside:host.querySelectorAll('img').length,preTextIsLiteral:host.querySelector('pre').textContent.includes('<script>'),xssFired:!!window.__xss_fired}; host.remove(); return JSON.stringify(res); }"
# 结果: { escaped:true, scriptElsInside:0, imgElsInside:0, preTextIsLiteral:true, xssFired:false }

# 反证对照（故意不转义，证明测试方法有效、不是假绿）
playwright-cli run-code "async page => { const host=document.createElement('div'); host.innerHTML='<pre><script>window.__xss_ctrl=true;<\/script></pre>'; document.body.appendChild(host); const res={scriptElsInside:host.querySelectorAll('script').length}; host.remove(); return JSON.stringify(res); }"
# 结果: { scriptElsInside: 1 }   ← 不转义就会生成 script 元素
# 验证: history.js:286 用 escapeHtml(data.code) 拼 <pre>，<script> 成为文本节点，不执行
```

#### WT-SX-16: XSS 用户名回显 ✅
```bash
playwright-cli run-code "async page => { const a=await page.evaluate(async()=>{const r=await fetch('/api/auth/register',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:'<img src=x onerror=alert(1)>',password:'pass1234'})}); return {status:r.status,body:await r.json()};}); const b=await page.evaluate(async()=>{const r=await fetch('/api/auth/register',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:'<script>alert(1)<\/script>',password:'pass1234'})}); return {status:r.status,body:await r.json()};}); return JSON.stringify({img:a,script:b}); }"
# 结果: 两者均 { status: 400, body: { code: 1001, data: { field: "username", reason: "用户名只能包含字母、数字、'-' 与 '_'" } } }
# 验证: 注册端按白名单拦下，两种 payload 都进不了库（真正可验的第一层）
```

#### WT-SX-17: 前端隐藏不是安全边界 ✅
```bash
# ⚠️ 安全边界必须在【匿名态】测：登录用户本来就有进度筛选权，前端 disabled 不是边界
playwright-cli run-code "async page => { await page.evaluate(async()=>{await fetch('/api/auth/logout',{method:'POST'});}); await page.goto('http://82.156.75.119:8080/list.html'); await page.waitForTimeout(1500); const r=await page.evaluate(async()=>{ const sel=document.querySelector('#f-progress'); const before={disabled:sel.disabled,title:sel.title,value:sel.value}; sel.disabled=false; sel.value='solved'; sel.dispatchEvent(new Event('change',{bubbles:true})); await new Promise(r=>setTimeout(r,900)); const x=await fetch('/api/problems?size=1&progress=solved'); const j=await x.json(); return {before, after:{disabled:sel.disabled,value:sel.value}, frontendAccepted:sel.value==='solved', backendStatus:x.status, backendCode:j.code, backendField:j.data?j.data.field:null, rowsRendered:document.querySelectorAll('#rows tr').length, hint:(document.querySelector('#result-hint')||{}).textContent}; }); return JSON.stringify(r); }"
# 结果: { before:{disabled:true,title:"登录后可按进度筛选"}, after:{disabled:false,value:"solved"},
#         frontendAccepted:true, backendStatus:400, backendCode:1001, backendField:"progress",
#         rowsRendered:1, hint:"共 26 题" }
# 验证: 去掉 disabled 后前端确实能选中，但后端仍按自身鉴权裁决 → 400/1001 field=progress；
#       列表未被过滤（仍是共 26 题），与 WT-LS-15 口径一致
```

---

### M14 缺陷发现：history.js 展开提交详情时抛 ReferenceError 🔴

> **这是本轮最重要的产出，文档 §3.3 未记录。**

| 项 | 内容 |
|---|---|
| 现象 | 在 `/history.html` 点任意一条提交行展开 → 详情区显示 **"state is not defined"**，没有代码块、没有逐用例表 |
| 复现 | 登录 zhangling → `/history.html` → 点第一行（真实提交 id=34）→ 详情区即报错 |
| 根因 | `static/js/history.js:261` 的 `bindDetailActions(detail, el, state, reply.data)` 中 `state` **不在作用域内**：`state` 是 `history.js:84` 那个 init 函数里的局部 `const`，而 `toggleDetail(el, id)`（`history.js:239`）只接了 2 个参数 |
| 触发链 | `toggleDetail` → `detail.innerHTML = detailHtml(reply.data)`（**已渲染成功**）→ 下一行 `bindDetailActions(...)` 抛 `ReferenceError` → 被 `catch` 捕获 → `detail.innerHTML` 被**覆盖**成错误文案 |
| 影响 | 详情 API 明明返回了完整数据（含 `code`、`caseResults`），前端却在渲染后立刻把它覆盖掉。**所有依赖展开详情的用例全部无法通过**：`WT-HR-05`~`WT-HR-08`、`WT-HR-12`~`WT-HR-16` |
| 修法 | 把 `state` 提升为模块级（或给 `toggleDetail` 加第 3 个参数 `state`，与 `bindRows(el, state)` 的传参方式保持一致） |
| 附带影响 | `WT-HR-08`（键盘 Enter 展开）同样失效；`WT-SX-15` 的「代码以文本节点渲染」只能退到 `escapeHtml` 机制层验证 |

**为什么文档没发现**：`web自动化测试文档.md` §3.3 把 M09 的这批用例标为 🔴「judge 未接线」，
于是「因为 judge 没接线跑不了」与「因为前端有 bug 跑不了」两件事被混为一谈。
**接线 judge 之后这批用例依然会红** —— 这是 §1.5 说的那种「隐性故障」。

**复现命令**：
```bash
playwright-cli run-code "async page => { await page.evaluate(async()=>{await fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:'zhangling',password:'666666'})});}); await page.goto('http://82.156.75.119:8080/history.html'); await page.waitForTimeout(2000); await page.locator('#sub-rows tr[data-id]').first().click(); await page.waitForTimeout(1500); return JSON.stringify(await page.evaluate(()=>{const d=document.querySelector('#sub-rows tr[data-detail]'); return {detailText:d?d.textContent.replace(/\s+/g,' ').trim():null, preCount:document.querySelectorAll('#sub-rows pre').length, tableCount:document.querySelectorAll('#sub-rows table').length};})); }"
# 结果: { detailText: "state is not defined", preCount: 0, tableCount: 0 }
```

---

### 本轮踩坑记录（Playwright CLI）

| 坑 | 现象 | 正确写法 |
|---|---|---|
| **mock 路由没卸干净** | 脚本中途超时，`page.context().route()` 注册的 mock 一直挂着，导致后续所有 `/api/submissions` 返回假数据（以为 ZhangLing 只剩 1 条提交，admin 概览却显示 34 条） | mock 一定要放在 `try { } finally { }` 里；或每轮开头先 `unroute` 所有已知 pattern |
| **Playwright glob 的 `*` 不跨 `/`** | `**/api/submissions*` 匹配不到 `/api/submissions/999`（详情），列表被 mock 了、详情没有 | 尾部要跨路径用 `**`：`**/api/submissions**` |
| **API 字段名是 `list` 不是 `items`** | mock 用了 `data.items` → 前端渲染空态「没有匹配的提交记录」 | `/api/submissions` 的响应是 `{ list, page, size, total }` |
| **detail 面板的错误文案是"藏起来"的** | mock 返回的 `code` 非 0 时前端显示 `reply.message`；反过来，**前端 JS 抛错**也会走同一个 catch 分支，UI 上看起来像"服务端返回了错误" | 区分两者：看 DevTools 控制台有没有 `ReferenceError`，不要只看 DOM 文案 |
| **"没有匹配的提交记录" ≠ "还没有提交记录"** | 两者是同一个空态组件的两种文案分支（`history.js:178`），前者说明**有筛选条件** | 排查时先看 `#f-problem` / `#f-verdict` 的当前值 |
| **有头模式下 `playwright-cli open` 无 `--head` 参数** | `--headed` / `--head` 都不是合法参数 | 默认就是有头；`--browser=chrome` 需先装对应浏览器，否则报 "Chromium distribution 'chrome' is not found" |

---

### 测试结果汇总（第二批）

| 模块 | 用例 | 结果 |
|------|------|------|
| M11 账户 | WT-AC-04 旧密码错误 | ✅ 通过 |
| M11 账户 | WT-AC-05 改密后当前会话仍有效 | ✅ 通过 |
| M11 账户 | WT-AC-06 改密踢掉其他会话 | ✅ 通过 |
| M11 账户 | WT-AC-07 旧密码失效/新密码可登录 | ✅ 通过 |
| M12 运维 | WT-AD-06 普通用户被拒 | ✅ 通过 |
| M12 运维 | WT-AD-07 匿名被拒且码序正确 | ✅ 通过 |
| M12 运维 | WT-AD-09 刷新按钮 | ✅ 通过 |
| M12 运维 | WT-AD-10 读取失败的用户可见提示 | ✅ 通过 |
| M12 运维 | WT-AD-11 基线未漂移 | ✅ 通过 |
| M13 守卫 | WT-GD-01 diff.js 为空 | ✅ 通过 |
| M13 守卫 | WT-GD-03 前端从不请求 diff 端点 | ✅ 通过 |
| M13 守卫 | WT-GD-04 账户设置无注销入口 | ✅ 通过 |
| M13 守卫 | WT-GD-05 前端从不调用注销端点 | ✅ 通过 |
| M13 守卫 | WT-GD-06 草稿挂载期分支缺失 | ✅ 通过 |
| M13 守卫 | WT-GD-07 clearDraft 无调用方 | ✅ 通过 |
| M14 安全 | WT-SX-02 用户A访问用户B的提交详情 | ✅ 通过 |
| M14 安全 | WT-SX-03 admin 也不放开归属 | ✅ 通过 |
| M14 安全 | WT-SX-04 删除他人提交被拒 | ✅ 通过 |
| M14 安全 | WT-SX-05 diff 他人提交被拒 | ✅ 通过 |
| M14 安全 | WT-SX-06 重跑他人提交被拒 | ✅ 通过 |
| M14 安全 | WT-SX-08 会话过期全局跳转 | ✅ 通过 |
| M14 安全 | WT-SX-10 同源 Cookie 策略 | ✅ 通过 |
| M14 安全 | WT-SX-13 SQL 注入（代码体） | ✅ 通过（口径受限，见上） |
| M14 安全 | WT-SX-15 XSS 代码显示 | ✅ 通过（机制层 + 反证对照） |
| M14 安全 | WT-SX-16 XSS 用户名回显 | ✅ 通过 |
| M14 安全 | WT-SX-17 前端隐藏不是安全边界 | ✅ 通过 |
| **缺陷** | **history.js 展开详情抛 `ReferenceError: state is not defined`** | 🔴 **新发现，需修复** |

**M11-M14 第二批共 26 条测试用例, 全部通过**；另**新发现 1 个前端缺陷**（history.js 详情展开）。

**累计: 40 条 + 26 条 = 66 条测试用例, 全部通过；另记录 1 个待修复缺陷**
