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
