# 整包聊天记录：只检查顶层

不调用 `get_forward_msg`，不展开 Node/Nodes，不读取包内正文、图片或嵌套转发。用户明确要求整包作为一个对象，因此本功能只能判断顶层引用及可见卡片是否重复。

## 结果含义

| 状态 | 依据 | 自动提醒 |
|---|---|---|
| `same_resource` | 同群、同命名空间的顶层资源 ID 相同 | 提示同一资源再次出现 |
| `possible_same_card` | 资源 ID 不同或缺失，但可见卡片摘要相同 | 默认不提醒，仅供诊断 |
| `new_resource` | 首次见到此资源 ID，且无相同预览 | 不提醒；不能证明内容全新 |
| `insufficient_evidence` | 没有可比资源 ID | 不提醒；不能判定内容 |

同一卡片预览可能对应不同聊天记录，绝不能仅凭标题、条数或预览判定整包内容相同。资源 ID 是平台引用标识，不是内容哈希。重新打包/某些转发路径可能产生新 ID；即使内容未变，也可能无法命中。

支持外层 OneBot `forward.data.id` 和 QQ `com.tencent.multimsg` JSON 卡片的 `meta.detail.resid`。两种标识使用不同命名空间，不假设它们等价。JSON 只对白名单中的可见标题、摘要、最多四条预览计算摘要；发送时间、外层消息 ID、uniseq、extra 不参与匹配。XML 卡片、其他适配器字段缺失时跳过，不猜测。

## 验证转发后是否变化

在测试群发送同一包，分别用 QQ 的再次转发、重新合并打包两种方式发送。只收集外层 `type`、`forward.data.id` 或卡片 `resid` 以及返回状态；不要调用下载整包接口。比较两次 ID：若相同则 `same_resource`，若变化则最多 `possible_same_card`。这需要真实 QQ 和所选适配器版本验证，项目不预先声称资源 ID 一定稳定。

两类消息现在共用 `Detector.detect(Message(...))` 和带依据的 SHA-256 指纹；旧 `ForwardDetector` 入口保留。见 [统一架构与哈希说明](ARCHITECTURE.md)。哈希存在不代表顶层元数据足以识别内部内容，卡片指纹仍然只是弱证据。

参考：[OneBot 消息段](https://github.com/botuniverse/onebot-11/blob/master/message/segment.md)、[公开 API](https://github.com/botuniverse/onebot-11/blob/master/api/public.md)、[NapCat 顶层多转发元素类型](https://github.com/NapNeko/NapCatQQ/blob/main/packages/napcat-core/types/msg.ts)。
