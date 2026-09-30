# 角色固件部署指南

角色功能将屏幕形象、动作动画与语音对话结合：固件负责显示和播放，资源包提供图像，服务端通过提示词赋予角色身份，并通过设备工具请求动作。本文件是角色功能的主文档，负责构建、刷机和服务端配置；各角色文件只提供该角色的配置值、提示词与动作清单。

默认连接小智官方服务器，无需部署本地后端。

## 选择角色

| 角色 | 资源包 | 配置文件 |
|---|---|---|
| 小猪佩奇 | `peppa-v1` | [peppa.md](peppa.md) |

先选定角色，再按下文完成部署。新增角色时，在本表添加入口，并将专属提示词、资源路径及动作清单放入独立角色文件，不复制通用部署步骤。

## 准备环境

当前实现面向 M5Stack CoreS3。以下板型命令、触屏配网方法和分区示例仅适用于 CoreS3，不可直接套用于其他设备。

准备以下环境：

- CoreS3、USB 数据线和可访问互联网的 Wi-Fi。
- ESP-IDF 6.1（最低 6.0.1，不支持 5.x）。
- 包含角色功能的源码：当前开发分支为 `feat/peppa-character`。
- [小智官方控制台](https://xiaozhi.me/console/)账号。

所有构建命令均在源码仓库根目录执行。Windows 可使用 WSL 编译，再在能访问设备串口的 Windows 环境刷机。

## 构建程序和资源

激活 SDK，使用标准入口生成 CoreS3 配置并构建：

```sh
source /path/to/esp-idf/export.sh
idf.py --version
python3 scripts/build.py m5stack/core-s3 --name m5stack-core-s3 --language zh-CN
```

将 SDK 路径替换为实际安装位置。角色显示默认关闭，随后通过配置界面启用：

```sh
idf.py menuconfig
```

在 `Xiaozhi Assistant` 中设置下列选项，也可按 `/` 搜索配置符号：

| 配置 | 设置 |
|---|---|
| `ENABLE_CHARACTER_DISPLAY` | 启用 |
| `FLASH_DEFAULT_ASSETS` | 选择默认资源构建 |
| `CHARACTER_PACK_DIR` | 填写角色文件中的资源路径 |
| `CHARACTER_DEBUG_ACTIONS` | 关闭，避免待机自动轮播测试动作 |
| `OTA_URL` | 保持 `https://api.tenclass.net/xiaozhi/ota/` |

唤醒词与服务端角色名独立配置。首次部署可保留原有唤醒方式；角色自定义唤醒参数见对应角色文件。保存后重新构建：

```sh
idf.py build
```

程序与资源需要配套：主要产物为 `build/xiaozhi.bin`、`build/generated_assets.bin`。检查构建日志中的角色包打包结果及分区容量；资源镜像还应保留中文字体和所需语音模型。

不要手工编辑生成的 `sdkconfig`、`build/` 或第三方组件。再次运行板型构建脚本可能重置配置，需要重新核对角色开关。不要为了角色功能改变板型身份或引脚。

## 刷机

### 首次刷入

在可访问 USB 串口的 ESP-IDF 环境，使用本次构建的完整烧录配置：

```sh
idf.py -p COM5 flash monitor
```

`COM5` 是示例，替换为实际端口；Linux 通常使用 `/dev/ttyACM*` 或 `/dev/ttyUSB*`。按 `Ctrl+]` 退出监视器。完整烧录文件和地址以 `build/flasher_args.json` 为准。

### WSL 编译、Windows 首次刷机

如果 WSL 无法访问设备串口，可在 Windows 使用 Python 和 esptool 烧录，无需在 Windows 重新编译：

1. 在 WSL 完成上述构建。
2. 将本次 `build/flasher_args.json` 复制到 Windows 的烧录目录，并复制其中 `flash_files` 列出的**全部文件**，保留相对于构建目录的子目录结构。通常包括 `bootloader/bootloader.bin`、`partition_table/partition-table.bin`、`ota_data_initial.bin`、`xiaozhi.bin` 和 `generated_assets.bin`；以本次清单为准，不从旧构建补文件。
3. Windows 安装 esptool：`python -m pip install esptool`。连接设备并确认串口，关闭占用该串口的监视器。
4. 在烧录目录打开 PowerShell，运行以下脚本。脚本读取本次构建的地址和 Flash 参数，并在烧录前检查全部文件是否存在：

```powershell
$flashManifest = Get-Content -Raw ./flasher_args.json | ConvertFrom-Json
$flashChip = $flashManifest.extra_esptool_args.chip
if ($flashChip -ne 'esp32s3') { throw '当前清单不是 ESP32-S3 构建，请核对板型。' }
$flashArgs = @('-m', 'esptool', '--chip', $flashChip,
    '-p', 'COM5', '-b', '460800', 'write-flash')
$flashArgs += @($flashManifest.write_flash_args)
foreach ($entry in $flashManifest.flash_files.PSObject.Properties) {
    if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) {
        throw "缺少烧录文件：$($entry.Value)"
    }
    $flashArgs += @($entry.Name, [string]$entry.Value)
}
python @flashArgs
if ($LASTEXITCODE -ne 0) { throw '烧录失败，请检查上方输出。' }
```

将 `COM5` 替换为实际端口。首次刷机使用完整清单，不能只复制程序和角色资源两个镜像。烧录后重启设备并继续配网；不要把其他版本的 `flasher_args.json` 与本次产物混用。

### 更新已有设备

更新前保存旧程序、资源及分区记录。分区布局不变时，可以只更新程序与资源，保留 NVS 中的网络配置。

以下命令仅适用于 16MB CoreS3、程序地址 `0x20000`、assets 地址 `0x800000` 的布局。先核对目标设备和本次分区表；布局不同则按实际烧录配置操作：

```powershell
python -m esptool --chip esp32s3 -p COM5 -b 460800 write-flash 0x20000 build/xiaozhi.bin 0x800000 build/generated_assets.bin
```

跨 WSL 与 Windows 复制产物时，使用同一次构建的程序和资源。不要把整片擦除作为常规更新步骤，也不要用上游通用固件覆盖角色功能。

## 配网与设备绑定

1. 重启 CoreS3，在刚开机的启动阶段轻点屏幕进入配网。
2. 手机连接屏幕显示的 Xiaozhi 热点，按提示打开配网页面，通常为 `http://192.168.4.1`。
3. 填写可上网的 Wi-Fi，使用官方 OTA 地址 `https://api.tenclass.net/xiaozhi/ota/`。
4. 保存并等待设备联网。
5. 登录[官方控制台](https://xiaozhi.me/console/)，创建或选择智能体，按设备提示完成绑定和激活。

如果设备曾连接其他服务器，需在配网页面的高级设置中修改自定义 OTA 地址。已保存的地址优先于固件默认值，重新编译默认地址不一定会切换后端。

官方服务模式下，开发电脑无需保持开机。

## 官方服务端配置

在控制台找到绑定设备的智能体，进入“配置角色”。界面名称和模型列表以实际账号页面为准。

### 提示词

从对应角色文件复制昵称和提示词，填写角色介绍，设置对话语言并保存。开始新会话，询问角色身份以确认配置生效。

角色提示词决定对话身份和动作调用意图，不改变设备离线唤醒词，也不会生成或下载 GIF。屏幕资源与服务端角色应匹配。

### 模型与语音

| 设置 | 要求 |
|---|---|
| 语言模型 | 支持所需语言及设备工具调用，具体型号从平台当前提供的选项中选择 |
| 对话语言 | 与角色提示词一致；中文角色选择普通话/中文 |
| 音色 | 选择对应语言的音色并试听；音色与角色画面分别配置 |
| 动作工具 | 服务链路与模型能调用设备上报的 `self.character.play_action` |
| 回复长度 | 通过提示词限制为适合语音交流的简短回复 |

页面未开放温度、输出 token 上限或独立 ASR 设置时，使用平台默认值。不要将自建后端的 `function_call`、`nointent` 字段当作官方控制台必有选项。使用官方托管模型不需要部署本地模型或填写本地服务密钥。

当前尚未完成官方服务器下自定义动作工具的完整验证。能聊天或口头答应表演，不等于已执行工具；切换模型后应重新测试动画调用。

参考：[小智官方常见问题](https://xiaozhi.me/xz-docs/docs/help-doc/xiaozhi-ai-chatbot-q%26a/)、[M5Stack CoreS3 小智指南](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/static/pdf/static/zh_CN/guide/realtime/xiaozhi/m5cores3.pdf)。

## 验收与排查

依次检查以下项目：

1. 设备启动联网，显示所选角色的基础画面。
2. 待机时轻点屏幕开始对话，确认中文识别、回复和角色身份。
3. 按角色动作清单明确请求表演，确认实际画面变化；再测试拒绝表演的语句。
4. 在设备回复时轻点屏幕打断，确认语音、画面和后续对话正常。
5. 连续对话并切换动作，观察卡顿、重启及内存异常。

动作可能在次数耗尽、达到时限、回复结束或状态切换时停止，不保证播满设定次数。当前动画不提供精确口型或逐句音画同步。

| 问题 | 检查方向 |
|---|---|
| 显示原版表情 | 角色开关、资源包是否打包并刷入、程序与资源是否配套 |
| 仍连接旧后端 | 配网页面中保存的自定义 OTA 地址 |
| 联网但无法聊天 | 互联网连接、设备激活和智能体绑定 |
| 角色身份或回复语言错误 | 是否修改了正确智能体、提示词和语言是否保存、是否开始新会话 |
| 口头答应动作但画面不动 | 模型的工具能力及实际调用结果 |
| 自定义唤醒无响应 | 检查唤醒模型与命令配置；先用触屏排除后端聊天问题 |

## 可选后端：xiaozhi-esp32-server

[xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) 是为小智设备提供服务的开源后端，可自行配置语音识别、对话模型、语音合成和工具能力，提供最简化安装及带管理后台的全模块安装。

默认使用官方服务器时无需部署。需要自建时，请按该项目的[部署文档](https://github.com/xinnan-tech/xiaozhi-esp32-server#部署文档)和 [docs 目录](https://github.com/xinnan-tech/xiaozhi-esp32-server/tree/main/docs)选择部署方式及配置模型。

角色文件中的提示词可复用；设备动作仍依赖工具调用。连接地址、认证和模型配置以所用服务端版本文档为准。官方与自建后端的智能体配置分别管理，不自动同步。
