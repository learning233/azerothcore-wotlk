# 部署验收清单（mod-wob-bootstrap + mod-gear-wardrobe）

配合 `DOCKER_UPDATE.md` 使用：模块拷入、`docker compose up -d --build` 完成后，
按顺序过这张表。每项都有预期结果；不符合的把日志/SQL 输出贴回来排查。

测试用的 SQL 一律通过数据库容器执行：

```bash
docker exec -it ac-database mysql -uroot -p acore_characters
# 密码见 docker-compose 的 MYSQL_ROOT_PASSWORD（默认 password）
```

---

## A. 启动检查（容器起来后立刻做）

| # | 操作 | 预期 |
| --- | --- | --- |
| A1 | `docker ps` | `ac-worldserver` / `ac-authserver` / `ac-database` 都是 Up（worldserver 启动要几十秒） |
| A2 | `docker logs ac-worldserver 2>&1 \| grep -iE "wob\|glyph"` | 出现 `[mod-wob-bootstrap] loaded 349 glyph class masks`。**没有这条 = 模块 SQL 没应用或模块没编进去，先停下排查** |
| A3 | 查库：`SHOW TABLES IN acore_characters LIKE 'mod_wob%';` | 有 `mod_wob_bootstrap_done` |
| A4 | `SELECT COUNT(*) FROM acore_world.mod_wob_glyph;` | 349 |
| A5 | `SHOW TABLES IN acore_world LIKE 'mod_gear%';` | 有 `mod_gear_wardrobe_whitelist`（空表，正常） |

---

## B. 新角色初始化（bootstrap）

任意账号建一个 1 级新角色登录：

| # | 操作 | 预期 |
| --- | --- | --- |
| B1 | 建完角色看角色列表 | 直接显示 80 级 |
| B2 | 进入世界 | 等级 80；本职业技能学全；背包有 6 种补给（绷带/手雷/投网/治疗注射器/法力注射器/魔法酪饼 x80） |
| B3 | 天赋页 | 双天赋可切换（两个 spec）；天赋点 0 已花（`character_talent` 无行）——不自动点天赋是设计行为 |
| B4 | 骑任意坐骑 / 诺森德飞行 | 280%/310% 坐骑可骑、诺森德可飞（骑术 300 + 寒冬飞行已自动学） |
| B5 | 穿着鞋子登录 | 鞋子自动附魔 Nitro Boosts（火箭靴）；没穿鞋聊天框提示跳过 |
| B6 | 建一个死亡骑士，看天赋页 | 天赋总点数 71（0 已花 + 71 可用）；出生地任务已自动标记完成，`character_talent` 仍无行 |
| B7 | 普通职业和死亡骑士各建一角，检查背包槽并重登 | 四个槽都装备霜纹包 `41599`，每个 20 格，旧包不在背包里，重登仍在 |
| B8 | 检查两个新角色的装备栏和背包 | 主手、副手和远程／圣物三槽保持原始装备（原先为空则仍为空）；其余装备槽为空，被销毁的初始装备不进背包 |

B 组对应的 SQL 验证（`<guid>` 换成角色 guid）：

```sql
SELECT level, specsCount FROM characters WHERE guid = <guid>;          -- 80, 2
SELECT skill, value, max FROM character_skills WHERE guid = <guid>
  AND skill IN (129, 197, 202, 762);                                   -- 三专业 450/450，骑术 300/300
SELECT COUNT(*) FROM character_talent WHERE guid = <guid>;             -- 0
SELECT spell FROM character_spell WHERE guid = <guid>
  AND spell IN (33388, 33391, 34090, 34091, 54197);                    -- 5 行（骑术四档+寒冬飞行）
SELECT itemEntry, count FROM character_inventory ci
  JOIN item_instance ii ON ii.guid = ci.item
  WHERE ci.guid = <guid> AND ci.itemEntry IN (34721,40771,41509,41166,42545,43523); -- 6 种都在
SELECT version FROM mod_wob_bootstrap_done WHERE guid = <guid>;        -- 4
-- 仅 DK：出生地任务应为 49 行（全表 59，扣掉 9 个其它种族 A Special Surprise 和 1 个敌对阵营最终任务）
SELECT COUNT(*) FROM character_queststatus_rewarded r
  JOIN acore_world.quest_template q ON q.ID = r.quest
  WHERE r.guid = <guid> AND r.active = 1 AND q.QuestSortID = -372;     -- 49
```

---

## C. 补货与老角色

| # | 操作 | 预期 |
| --- | --- | --- |
| C1 | 用掉/卖掉一半补给，重新登录 | 数量补回目标（绷带回 20、松饼回 80） |
| C2 | 多拿几组松饼再登录（topup 默认） | 多出来的**不被删** |
| C3 | **之前初始化过的老角色**重新登录一次 | 自动补学骑术（`character_spell` 多出 5 行、skill 762 变 300）——VERSION=2 的重跑机制，不用删 marker |
| C4 | **v3 时期已存在的老 DK** 登录 | 出生地任务被标记完成（B 组 SQL 49 行）；本次登录天赋仍是旧点数，**再登录一次变 71** |

---

## D. 猎人宠物

| # | 操作 | 预期 |
| --- | --- | --- |
| D1 | 带出战宠的猎人登出后再登录 | 身边恢复上次退出时的同一只宠物（pet_number/entry/name 不变），不会出现默认 704 狼 |
| D2 | `SELECT id, entry, level, name, slot FROM character_pet WHERE owner = <guid> ORDER BY slot;` | `slot=0` 的宠物登出前后不变且为 80 级；新角色没有自动创建的 704 |
| D3 | 猎人点击宠物天赋面板或兽栏面板【重置天赋】 | 服务端只重置出战宠物的已分配天赋点数，全额返还（兽栏中的不受影响），客户端天赋树刷新，播放洗点音效 |
| D4 | 宠物快乐度随时间消耗或死亡扣减后等待 | 服务端 30 秒心跳检测自动拉回 100 万（满快乐度），锁定 125% 伤害档位 |
| D5 | 制造 2 条以上 `character_pet.slot=100` 的猎人宠物后执行 `wob_pet_adopt <entry>` | 被拒绝并提示“你还有未归栏的宠物”；打开兽栏后自动归栏，再驯服成功 |
| D6 | 新建/驯服宠物后查 `pet_spell` 和动作条 | 不含 `24394/75446/75447/75593`；正常家族主动/被动技能仍在 |
| D7 | 骑乘/临时收起当前宠后打开兽栏 | 骑乘状态下模块下马并恢复当前宠，取出/交换成功；载具/传送中未恢复时明确提示并回 6 |
| D8 | `WobBootstrap.StablePanel = 0` 后分别发 `wob_stable` / `wob_stable_refresh` | 都不加 62757 aura、不重发兽栏列表 |
| D9 | 宠物死亡且尸体消失（`slot=100, Health=0`）后打开兽栏并施放复活宠物（982） | 死宠不被自动塞进兽栏槽位，982 可正常复活；死宠未复活前存/换兽栏与驯服均被明确拦截 |
| D10 | 战斗中或角色死亡状态下尝试打开兽栏 / 存 / 取 / 换 / 放生 | 客户端本地即拦截提示；绕过 UI 直接发包也会被服务端拦截并返回错误提示 |

---

## E. 衣柜（mod-gear-wardrobe）

前提：客户端衣柜开关已默认开启（`wardrobeServerEnabled` 默认 `true`，
见 `lib/src/app_settings.dart`；如需临时关闭用 `--dart-define=WOB_WARDROBE_SERVER=false`）。

> 装备页按回执和同步后的装备确认结果，衣柜回执不进战场信息条。

| # | 操作 | 预期 |
| --- | --- | --- |
| E1 | 打开装备页 | 顶部“服务端当前装备”= 19 格真实装备，点击槽位可直接换装（不再只是本地预览） |
| E2 | 从目录选一件尚未持有的本职业紫装 | 服务端创建到指定槽；装备页/模型自动同步 |
| E3 | 同槽 A→B→A，其中 A 有附魔或宝石 | 旧件回包；换回同一 GUID 的 A，附魔和宝石仍在 |
| E4 | 非泰坦之握角色从单手+盾换双手 | 副手原实例回包；背包无处容纳时主手和副手都保持原状 |
| E5 | 申请其他职业的护甲（如法师选板甲） | 服务端拒绝，原装备保留 |
| E6 | 点装备槽的「卸下」 | 该槽清空，物品进背包；没有空位时保持装备 |
| E7 | 双持职业主手 1H + 副手选另一把 1H | 服务端 `PLAYER_VISIBLE_ITEM` 15/16 两格都有；不再把副手 1H 塞进主手 |
| E8 | 换装时身上有项链/戒指/饰品 | 这些槽不被误删；只动用户点选的格 |
| E9 | 两个客户端同屏，A 换一件装备 | B 看 A 只更新该槽，其他 18 格不消失（增量 visible items 合并） |
| E10 | 服务端拒绝（槽位/类型/职业不符） | 客户端回退到服务端最后一次确认的 19 格，不显示假装备 |
| E11 | 玩家处于战斗中（`IsInCombat`）尝试换装 | 防具及饰品等受原版规则限制；有槽未完成时不提示整套成功 |
| E12 | 两套配装之间反复一键切换 | 每次只发一个套装包或一个批量衣柜请求；不逐格发 Say |
| E13 | 用缺件的本地配装执行 `/equipset`，再从装备页选同一配装 | 宏提示缺件且不发换装包；装备页可领取合规缺件 |
| E14 | 背包满，换随身包中已有的一件装备 | 旧装备可放进新件腾出的格子，保留两个物品 GUID |
| E15 | 两个戒指槽都已装备；从装备页用包里的戒指替换戒指 2，新旧戒指镶了同一颗唯一装备宝石 | 只替换戒指 2，旧戒指及其宝石原样回包 |

---

## F. 两模块交互

| # | 操作 | 预期 |
| --- | --- | --- |
| F1 | 穿装备（wob_wardrobe）和插雕文（wob_glyph，客户端面板未实现前可用官方 3.3.5 客户端在说话框直接输入）互不影响 | 各自生效，聊天里都不广播命令明文 |

---

## H. 盗贼武器涂毒（mod-wob-bootstrap）

| # | 操作 | 预期 |
| --- | --- | --- |
| H1 | 盗贼在换装界面点选主手/副手武器，点击毒药技能条（致伤/致命/速效/减速/麻痹） | 服务端为武器施加 1 小时对应临时附魔（绿色字），客户端弹 Toast 提示，战斗攻击触发对应毒药特效 |
| H2 | 点击【清除涂毒】 | 武器临时附魔被清除，提示已清除涂毒 |
| H3 | 处于战斗状态（`IsInCombat`）尝试涂毒 | 被服务端拦截，提示："战斗中无法涂抹毒药。" |

---

## G. 已知限制（不是 bug，别报）

- 未领取的补货邮件不计入背包数量：背包长期满且不取邮件，每次登录会重复邮寄。
- `RefreshMode=topup`（默认）只补不删；多出来的物品保留。
- 雕文面板 / 消耗品栏 / 兽栏面板的**客户端 UI 还没做**（plan Task 6–10），服务端命令已就绪，
  目前可用官方客户端直连服务器验证 `wob_glyph` / `wob_stable`。

---

装备页与宏的协议和持有规则见 [mod-gear-wardrobe](mod-gear-wardrobe/README.md)。
