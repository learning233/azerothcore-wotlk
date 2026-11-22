# mod-gear-wardrobe

在线装备页换戒指、饰品、主副手，或需要领取缺件、卸装、多槽换装时，由本模块校验整套目标。
其余普通单槽随身装备替换及宏 `/equipset` 使用 AzerothCore 原版 `CMSG_EQUIPMENT_SET_USE`。
宏缺件时提示并且不创建装备。

## 换装规则

- 保留已经穿在目标槽的物品。其余槽优先复用主背包、已装备的四个袋子或其他装备槽里的实物；每个实例只能占一个槽。
- 装备页所需实例不够才创建。只有创建时使用衣柜武器/护甲、精良及以上、需求等级不高于 80 和可选白名单规则。卖掉或删除后可再领。
- 保留实际物品实例（含附魔、宝石等）。卸下、替换以及被双手武器挤下的物品进入背包。服务端先为所有旧件预留格子，也会利用本次换装腾出的格子；放不下就拒绝这次批量请求。
- 批量请求校验职业、装备槽、战斗/竞技场、双持/泰坦之握及最终装备和宝石的唯一装备限制后才移动物品。

## 批量协议

一条普通 `CMSG_MESSAGECHAT`（Say）携带完整目标：

```text
wob_wardrobe set <request> <entry0> ... <entry18>
```

装备槽为 `EQUIPMENT_SLOT_*` 的 0..18，`entry=0` 表示空槽。命令由 `OnPlayerCanUseChat` 消费，不广播。
回包为 `衣柜：<request> OK` 或 `衣柜：<request> ERR <原因>`。客户端收到确认并看到实际装备后才提示成功。
批量衣柜请求仍受聊天通道的静音限制，但每套只占一条消息。

## 部署

按 [部署说明](../DOCKER_UPDATE.md) 更新客户端及 worldserver 中的本模块。
如启用 `GearWardrobe.EnableWhitelist=1`，在世界库的 `mod_gear_wardrobe_whitelist` 填入允许领取的装备 ID。
客户端 `--dart-define=WOB_WARDROBE_SERVER=false` 可将装备页切到预览模式；原版换装宏不依赖本模块。
