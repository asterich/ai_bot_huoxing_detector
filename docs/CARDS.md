# 消息分享卡片查重

AstrBot 与 MaiBot 在群内识别最外层 QQ `com.tencent.structmsg` 的 `meta.news.jumpUrl`、`com.tencent.miniapp_01` 的 `meta.detail_1.qqdocurl`，以及 `serviceID="1"` XML 卡片的根 `url` 属性。不会扫描标题、预览图、来源链接、普通文本、引用消息或合并转发内部节点。

匹配规则：

- B 站 `bilibili.com`、`www.bilibili.com`、`m.bilibili.com` 的 `/video/BV…`、`/video/av…`：按视频 ID 匹配，忽略分享参数；分 P 视为同一视频。AV 和 BV 暂不互转。
- 微信公众号 `mp.weixin.qq.com/s/<短标识>`：按路径标识匹配；`/s?__biz=…&mid=…&idx=…&sn=…`：保留这四个参数，忽略分享来源参数。重复或缺失必需参数时跳过。两种链接形式不跨形式合并。
- 其他 HTTP/HTTPS 链接（包括 `b23.tv`）：仅规范化主机大小写、去掉 fragment，其余路径、协议及查询参数保留。同一短链接可命中，但不会联网展开，也不会自动关联视频长链接。

提示仅表示同群出现过相同目标标识，不能说明网页内容未更新。相同标题或封面不构成重复依据。不同群互不匹配；重复投递同一消息不重复回复。只有新的消息 ID 才会产生再次分享提示。

身份复用现有原生检测器的资源引用通道，使用独立的 `share:*:v1` 命名空间；标识先计算 SHA-256，数据库不保存原始 URL、标题、完整卡片。沿用已有 SQLite 事务、消息幂等、首次记录和 `delete_scope` 清理，无需修改原生库或数据库结构。外层分段序号保持不变，与合并转发分段不会混淆。

XML 最多 64 KiB，禁止 DTD/实体声明；URL 最多 4096 字符，拒绝非 HTTP(S)、带认证信息或显式端口的 URL。未知卡片格式跳过。AstrBot 的 OneBot 入口读取保留的原始外层数组，避免 XML 在宿主转换中被丢弃。

验证使用生成的卡片样例、真实 SQLite/原生扩展和宿主事件替身；未在真实 QQ 群发送卡片进行端到端验证。

网易云音乐支持 music.163.com / m.music.163.com / y.music.163.com 的 song、album、playlist ID，包含桌面 #/song?id= 和移动 /m/song?id= 路径。忽略分享跟踪参数，歌曲、专辑和歌单各用独立命名空间。QQ 音乐 JSON 卡片读取 meta.music.jumpUrl；音乐 XML 卡片读取 serviceID=2 根 URL。无明确目标 ID 的网易云地址不匹配，短链不联网展开。
