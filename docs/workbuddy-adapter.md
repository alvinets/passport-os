# WorkBuddy 接入与协议适配

本固件用 `components/passport_workbuddy/` 替代了原 Muse 应用。设备不保存 OAuth
`client_secret`，也不直接连接 WorkBuddy 账户 API；它只保存中继地址和该中继签发
的设备令牌，通过 HTTPS 与中继通信。原 `components/passport_muse/` 只保留通用的
HTTP CONNECT 传输（代理与证书校验），不再包含配对、Noise 会话或聊天协议。

## 设备协议

所有 `/v1/*` 请求使用 `Authorization: Bearer <device_token>`。

| 请求 | 响应 | 用途 |
| --- | --- | --- |
| `GET /v1/status` | `{"ok":true,"agent":"WorkBuddy"}` | 检查中继与 WorkBuddy 授权状态 |
| `POST /v1/turn/begin` | `{"turn_id":"..."}` | 开始一轮语音 |
| `POST /v1/turn/{id}/audio` | `{"ok":true}` | 上传 base64 WAV 分片 |
| `POST /v1/turn/{id}/end` | `{"state":"working"}` | 结束上传，开始处理 |
| `GET /v1/turn/{id}` | `{"state":"working\|done\|error","text":"..."}` | 轮询回复，500 ms 间隔，60 s 超时 |
| `POST /v1/turn/{id}/cancel` | `{"ok":true}` | 取消等待 |

设备端约束：16 kHz / 16 bit 单声道 PCM，8 KiB staging，base64 后逐片上传；
回复截断为 1023 UTF-8 字节并按 5 行 × 26 列分页。按压 30 秒自动结束。

## 中继

参考实现 `tools/workbuddy_relay.py`（Python 标准库）：

- OAuth 2.1：`/oauth/start` 跳转授权页，`/oauth/callback` 用 `client_secret`
  在服务端换取 token，可选持久化到权限 0600 的文件。
- 任务与 ACP：`POST /openapi/v2/tasks` 创建任务，取得 `link`/`token` 后经
  SSE + JSON-RPC 依次调用 `initialize`、`session/load`、`session/prompt`，
  从 `session/update` 聚合文本回复。
- 语音转写：ACP 没有公开的音频输入端点，因此中继需要配置
  `WORKBUDDY_ASR_URL`（OpenAI 兼容转写接口）。未配置时设备会收到明确错误。

启动前至少设置 `WORKBUDDY_CLIENT_ID`、`WORKBUDDY_CLIENT_SECRET`、
`WORKBUDDY_REDIRECT_URI`、`WORKBUDDY_DEVICE_TOKEN`。生产环境须置于 TLS
反向代理之后，不要直接用默认 `127.0.0.1` 绑定对外服务。

## 构建与烧录注意

- 项目基线是 ESP-IDF 5.5.3。若用 ESP-IDF 6.1 构建，需要已加入的版本条件：
  `esp_crt_bundle` 并入 `mbedtls`、`json` 改为托管组件 `cjson`、`esp_driver_ledc`
  与 `esp_ringbuf` 显式依赖、深度睡眠唤醒 API 变更、Netease AES 使用 mbedTLS 4
  内部头文件。已无引用者的 `noise_core` 从构建中排除。
- 应用镜像约 3.62 MiB。设备若仍使用旧的 3 MiB `factory` 分区表，仅写
  `0x10000` 会导致 'Image length ... doesn't fit in partition length' 且反复重启。
  需先写本仓库 `partition-table.bin` 到 `0x8000`（会废弃旧设备上的私有数据分区），
  再写 app；无需从 `0x0` 写完整镜像，bootloader 与 NVS 可保留。

## 验证边界

主机测试覆盖设备配置校验、文本分页/base64、中继 HTTP 协议、ACP 消息映射。
2026-11-24 已在 ESP-IDF 6.1 下完成固件构建、布局/SRAM 校验与 COM14 真机烧录；
启动日志确认可以进入手机设置与 WorkBuddy 应用注册，未出现崩溃。
尚未验证：WorkBuddy 真实 OAuth 审核、`client_secret` 换取 token、真实 ACP
长连接保活、端侧麦克风/PTT、屏幕上的回复分页与真机网络切换。
这些项目必须在取得凭据后由使用者实际按压设备验收，不能以编译或启动日志代替。
