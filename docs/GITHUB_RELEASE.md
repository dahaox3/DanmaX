# GitHub Actions Windows 编译与发布

工作流为 `.github/workflows/windows-release.yml`。在仓库 Actions 中运行 **Windows mpv release** 可生成 Windows x64 ZIP 和依赖源码材料，保留 30 天；推送 `legacy-mpv-v*` 标签会在编译、21 项相关 CTest、MPV 配置界面测试及部署目录 smoke 均通过后自动创建 GitHub Release，附运行包、对应源码包、SHA-256 与构建来源信息。

发布示例：在已确认提交上创建 `legacy-mpv-v0.2.5` 标签并推送。工作流的手动入口不发布 Release，不覆盖已有标签或发布资产。

云端使用 `windows-2022` 镜像，明确声明镜像提供的 Visual Studio 2022 Enterprise，严格校验锁定的 MSVC 14.44.35207 与 SDK 10.0.26100.0。Qt 6.11.0、CMake 和 Ninja 使用仓库锁定下载与 SHA-256；没有调用全局 Qt。镜像工具版本缺失时失败，不静默改用另一编译器。

QWindowKit 原 ZIP 仅支持本机缓存，云端显式通过 `prepare-qwindowkit.ps1 -FromGit` 获取同一锁定 commit 及该 commit 的子模块 gitlink。联网准备与 CMake 编译分开。缓存仅保存 SDK、已验证下载和 Anitomy 归档，不缓存用户数据或编译目录。

运行包是动态 Qt 目录版，包含 Qt/QML、官方 MSVC x64 CRT 可再分发 DLL、许可及使用说明。网页 MPV 启动器和协议绑定已集成在应用内，无需 Python；旧 Emby 脚本仅为兼容保留。完整解压后运行 DanmaX.exe，不能只移动 EXE。打包从全新 staging 收集依赖，拒绝个人设置、Token 配置、协议备份、关联、日志、缓存和调试符号；不会访问开发机的 mpv 或服务器配置。

源码材料包含对应提交的应用源码、Qt 官方源码归档及校验清单、Anitomy 和固定 QWindowKit/子模块源码。二进制与材料由同一次云端任务生成。

云端测试不覆盖真实 GPU 压力、穿透、焦点和跨屏 DPI。在线测试中已知的自签名证书夹具、在线元数据及完整界面套件不在 21 项 CTest 发布门槛内；新增 MPV 配置界面另以 offscreen 模式验证。不关闭生产 TLS 校验。协议与 Emby 回传测试使用隔离注册表及模拟服务器，不接管真实默认协议或修改个人观看记录。
