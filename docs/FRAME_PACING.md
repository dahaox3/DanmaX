# Windows 弹幕帧节奏

## 实现

- `maxFps=0` 保留 Qt 的显示刷新驱动。透明覆盖层通过公开的 `QSurfaceFormat::setSwapInterval(0)` 提交，避免 Windows 的更新请求已经跟随 DXGI 垂直同步后，D3D11 在开始帧时再次等待交换链。主面板和工具栏保留原设置。
- 限帧使用单调时钟、固定相位的纳秒截止时间，迟到时跳过过期帧，不逐帧累积唤醒误差。Windows 高精度 waitable timer 通过公开的 `QWinEventNotifier` 唤醒 GUI；不可用时回退到精确 `QChronoTimer`。不忙等，不更改系统计时器精度或进程优先级。
- 场景同步时仅修正滚动文字从快照计算到同步之间的位移，最多修正 100 毫秒；固定字幕不移动。暂停发布零速度快照，跳转仍由引擎重置，入场、避让和寿命不在渲染线程执行。
- mpv 生命周期监视保留已经验证身份的进程句柄，避免每 100 毫秒重新解析可执行文件路径。

限帧部分参考 [Magpie StepTimer（固定版本）](https://github.com/Blinue/Magpie/blob/9e8e90274ba07f555bf87f09321dabdac97b1b4f/src/Magpie.Core/StepTimer.cpp) 的固定相位和高精度定时思路，但不移植其短时忙等。Magpie 的最低帧率用于没有新捕获帧时强制重绘，不能保证 GPU 负载下的 1% low，因此没有增加“最低帧率保证”设置。

Qt 行为依据锁定版本 6.11.0 的 `qwindowswindow.cpp::requestUpdate`、`qsgthreadedrenderloop.cpp::polishAndSync` 和 `qrhid3d11.cpp::beginFrame`；产品只调用公开 API。其他图形驱动、全屏组合及机器仍需单独验证。诊断时可用进程环境变量 `DANMAX_OVERLAY_SWAP_INTERVAL=1` 对比原交换间隔。

## 日志口径

`--report` 中的 `presentMeanMs`、`presentP95Ms`、`presentP99Ms`、`presentMaxMs` 来自渲染线程 `frameSwapped` 的单调时间戳，仅纳入内容序号变化的帧，保留最近 600 个间隔。`presentationIntervalsMs` 保留这些原始间隔供复算。不同报告的滑动窗口会重叠，不能拼接当成独立样本。

`onePercentLowFps` 在至少 100 个间隔后计算：1000 除以最慢 1% 间隔的平均毫秒数，样本数向上取整。它不是 P99 的倒数。`renderWaitP99Ms` 覆盖开始帧到场景同步的等待，`renderCpuP99Ms` 覆盖同步和命令准备，`renderSubmitP99Ms` 覆盖结束帧／提交，`guiDeliveryP99Ms` 是提交回调排队到 GUI 的延迟。它们不是 GPU 时间戳查询结果。

这些数据表示 Qt 帧提交时间，不能等同于显示器光学扫描输出或保证 DWM 显示了每个提交帧。`presentedPerSecond` 仍包含重复画面提交；`p95Ms/p99Ms` 仍为含维护路径的 GUI tick 间隔。报告不再写入可能包含流凭据的 SMTC 标题和标签。

## 2026-10-24 实机记录

Windows 11，Release 动态部署，Qt 6.11.0；i5-14600KF（20 逻辑处理器）、RTX 4070 Ti、驱动 617.42。显示器 160 Hz，DPR 2.25，窗口客户区 970×546 逻辑像素（约 2183×1229 物理像素）。mpv 同时运行 VapourSynth 超分和 `d3d11vpp`，输出 3840×2160、BT.2020/PQ，片源约 23.976 FPS。DanmaX 的 `maxFps` 始终为 0。

以下取每秒报告中的滑动窗口统计中位数；过滤启动前 5 秒、隐藏／暂停以及超过 2 秒未刷新的报告。不是相同片段的严格对照试验。

| 项目 | 原交换间隔 1 | 覆盖层交换间隔 0 |
| --- | ---: | ---: |
| 有效报告数 | 45 | 55 |
| 弹幕更新率中位数 | 123.5 FPS | 159.9 FPS |
| 窗口平均提交间隔中位数 | 8.08 ms | 6.25 ms |
| 窗口 P99 中位数 | 18.89 ms | 9.54 ms |
| 窗口 1% low 中位数 | 49.8 FPS | 92.1 FPS |
| 窗口开始帧等待 P99 中位数 | 18.62 ms | 0.026 ms |
| GUI 接收延迟 P99 中位数 | 19.43 ms | 0.88 ms |
| GPU 占用范围 | 48～72% | 47～66% |
| 活动弹幕数 | 3～18 | 2～8 |
| mpv 新增丢帧 | 0 | 0 |

原始文件位于 `out/validation/pacing-stress/uncapped-202138.jsonl`、`final-uncapped-202753.jsonl` 和 `summary.json`。另一次仅切换交换间隔的 30 秒观测得到约 159.8 FPS、P99 中位数 9.25 ms、1% low 中位数 98.2 FPS。改后的 60 秒观测期间后台存在回归测试负载；不同播放片段和弹幕密度限制了直接比较的精度。

原交换间隔下还记录到快照同步延迟约 6 ms、中途达约 25 ms；改后这项单次采样通常小于 0.1 ms。中途有跳转的记录 `reprojection-swap1-202548.jsonl` 不用于稳定性能结论。

没有宣称已经验证持续 99～100% GPU 饱和、全屏、跨屏 DPI 或光学无撕裂输出。没有修改 mpv 的视频同步、滤镜或用户保存的弹幕帧率上限。

回归：相关 CTest 20/20 通过（99.04 秒），包括 XML 小数字号、分类避让、限帧、时钟、渲染缓存、隐藏播放、外部会话和应用 smoke。新增渲染用例模拟同步前延迟，覆盖滚动、倍速、暂停和固定字幕。帧率测试的局部回调上下文在用例结束时销毁，避免断开连接后仍排队的回调引用失效栈变量；该套件修复后连续 5 次通过。界面自动化与已知证书夹具相关的在线测试不在这次 20 项范围内。
