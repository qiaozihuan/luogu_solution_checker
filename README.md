luogu_check 使用教程
一个命令行小工具，用来判断洛谷题目能不能提交题解，也支持按难度批量爬取。
---
一、它在判断什么
洛谷题目页右侧栏有个「提交题解」按钮，只有接口字段 `acceptSolution == true` 才会出现；
为 `false` 时页面会显示：
> 本题目已不接受新题解提交
> 当题目的题解数量已足够多、做题思路已足够丰富，题目过于简单，或处于月赛保护期时，题解提交入口会被关闭。
程序直接读这个字段，所以结论和你在网页上看到的完全一致。四种结论：
结论	含义
可提交题解	`acceptSolution = true`，页面上有「提交题解」按钮
不可提交	`acceptSolution = false`，题目已关闭题解入口
未知（需登录）	没给 Cookie。匿名访问时该字段恒为 `false`，不可信
抓取失败	网络错误 / 页面结构异常
> \*\*必须带 Cookie。\*\* 不带 Cookie 时所有题都会显示「未知（需登录）」，这是故意设计的——匿名数据不可信，不编结论。
---
二、准备工作
1. 进入目录
```powershell
cd D:\\Documents\\deepseek-harness\\default-workspace\\luogu\_solution\_checker
```
后续命令都在这个目录下执行。
2. 填 Cookie（只做一次）
打开 `cookie.txt`，把最后一行改成你自己的：
```
\_uid=你的UID; \_\_client\_id=你的client\_id
```
获取方法：浏览器登录洛谷 → 按 F12 → 网络(Network) → 随便点一个请求 → 复制请求头里的 `Cookie`，
从中挑出 `\_uid` 和 `\_\_client\_id` 两项即可。
注意：
`C3VK` 不用填，它只有 5 分钟有效期，程序会自动获取。
`gdxidpyhxdE` 之类访客标记不用填。
文件里以 `#` 开头的行是注释，会被忽略。
也可以直接用 Netscape 格式的 cookies.txt，程序能识别。
3. 验证 Cookie 是否有效
```powershell
.\\luogu\_check.exe --cookie cookie.txt P1000
```
看到 `正在校验 Cookie 登录态... 通过（uid=xxx，用户名 xxx）` 就说明没问题。
如果提示 Cookie 已失效，回第 2 步重新取一份。
---
三、基本用法
查几道指定的题
```powershell
.\\luogu\_check.exe --cookie cookie.txt P1000 P1044 P9093
```
输出：
```
\[ 1/ 3] P1000   入门             超级玛丽游戏              不可提交
        └ 洛谷原话：本题目已不接受新题解提交（题解数量已足够多、题目过于简单或处于月赛保护期）
\[ 2/ 3] P1044   普及-            \[NOIP 2003 普及组] 栈      不可提交
\[ 3/ 3] P9093   普及/提高-       \[PA 2020] Punkty rankingowe  可提交题解
```
从文件里读题号
```powershell
.\\luogu\_check.exe --cookie cookie.txt -f 题号.txt
```
`题号.txt` 里题号可以用空格、逗号、分号、换行分隔，随便写。
---
四、按难度爬取（核心功能）
先看各难度有多少题
```powershell
.\\luogu\_check.exe --list-difficulty
```
```
序号  难度             名称               题目数
1     入门             入门               1190
2     普及-            普及-              2038
3     普及/提高-       普及/提高-         2432
...
```
爬某个难度
```powershell
.\\luogu\_check.exe --cookie cookie.txt -d 普及- --limit 20
```
`-d` 后面可以写：
写法	例子
中文难度名	`-d 普及-`
数字序号	`-d 2`
逗号并列多个	`-d 入门,普及-`
区间	`-d 入门至普及/提高-` 或 `-d 1-3`
全部	`-d all`（等价于 `-d 0`）
难度名对照：
序号	名称
1	入门
2	普及-
3	普及/提高-
4	普及+/提高
5	提高+/省选-
6	省选/NOI-
7	NOI/NOI+/CTSC
> 难度名本身就带 `-`（比如「普及/提高-」），所以区间的分隔符除了 `-` 还支持「至」「到」「\~」，
> 写法有歧义时会自动逐个位置试切分。
控制爬取范围
```powershell
-d 普及- --pages 1-3        # 只翻题目列表的第 1\~3 页（每页 50 道）
-d 普及- --offset 100       # 跳过前 100 道
-d 普及- --limit 50         # 最多取 50 道
-d 普及- --keyword 背包      # 只留题名含「背包」的（走服务端搜索，覆盖全站）
```
这几个可以组合。比如分批跑完 2038 道普及- 题：
```powershell
.\\luogu\_check.exe --cookie cookie.txt -d 普及- --offset 0   --limit 500 --csv 批次1.csv
.\\luogu\_check.exe --cookie cookie.txt -d 普及- --offset 500 --limit 500 --csv 批次2.csv
.\\luogu\_check.exe --cookie cookie.txt -d 普及- --offset 1000 --limit 500 --csv 批次3.csv
```
保存成 CSV
加 `--csv 文件名` 即可：
```powershell
.\\luogu\_check.exe --cookie cookie.txt -d 普及- --limit 100 --csv 结果.csv
```
CSV 是 UTF-8 带 BOM，Excel 双击直接打开不乱码。中文文件名、带空格的文件名都支持。
列：题号 / 题目 / 难度 / 类型 / 提交数 / 通过数 / 通过率 / acceptSolution / 结论 / 说明。
和题号文件叠加
`-d` 和 `-f` 可以一起用，结果取并集：
```powershell
.\\luogu\_check.exe --cookie cookie.txt -d 入门 -f 我关注的题.txt
```
---
五、速度与限流
洛谷有反爬，程序默认每道题之间随机等 3~10 秒。速度相关参数：
```powershell
--delay 4000         # 固定间隔 4 秒（等价于 min=max=4000）
--delay-min 5000     # 最少等 5 秒
--delay-max 15000    # 最多等 15 秒
--timeout 20         # 单次请求超时 20 秒
--retry 2            # 失败的题重试 2 次
```
> \*\*别把 delay 调得太小。\*\* 我在测试时用过 0.8 秒，2038 道题要跑将近 30 分钟；
> 如果想安全一点，保持默认或 `--delay 5000` 更稳妥。请求太密集可能被临时封 IP。
顺便估算一下：默认 3~10 秒（平均 6.5 秒），100 道题约 11 分钟，1000 道题约 1.8 小时。
---
六、完整示例
找几道「普及- 到普及/提高-」里还能提交题解的题：
```powershell
.\\luogu\_check.exe --cookie cookie.txt -d 普及-至普及/提高- --pages 1-4 --csv 候选.csv
```
然后打开 `候选.csv`，把「结论」列筛成「可提交题解」就是答案。
检查某个比赛的题目：
```powershell
.\\luogu\_check.exe --cookie cookie.txt P1001 P1002 P1003 P1004 --delay 5000
```
---
七、常见问题
Q：为什么全是「未知（需登录）」？
没带 `--cookie`。登录态是判断的前提。
Q：Cookie 明明没过期，程序却说失效？
先确认 `cookie.txt` 最后一行没有被 `#` 注释掉，且 `\_uid` 和 `\_\_client\_id` 都在。
再手动跑一次 `--cookie cookie.txt P1000` 看提示。
Q：CSV 打开是乱码？
文件本身是 UTF-8 BOM，Excel 直接双击打开正常。
如果你用别的编辑器打开看到乱码，让它按 UTF-8 解码即可。
Q：中途想停怎么办？
Ctrl+C，或者关掉窗口。已抓到的结果不会写 CSV——CSV 是全部跑完才一次性写出的。
想中途拿结果就分批次跑（用 `--offset` / `--limit`）。
Q：能爬题解内容吗？
不能。这个工具只判断「能不能提交」，不抓题解正文。
---
八、重新编译
改了源码之后：
```powershell
g++ -std=c++17 -O2 -Wall -Wextra luogu\_solution\_checker.cpp -o luogu\_check.exe -lwinhttp -lshell32
```
需要 MinGW-w64（本机是 `D:\\mingw64`）。两个库都不能少：`-lwinhttp` 是 HTTP，`-lshell32` 是中文命令行参数解析。
