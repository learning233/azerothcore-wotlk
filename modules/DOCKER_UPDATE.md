# 从 docker 原版更新到带 mod-wob-bootstrap 的本 fork

适用场景：服务器上已经按官方方式装了 docker 版 AzerothCore（`git clone` 官方仓库 +
`docker compose up -d`），现在要把它换成带 `mod-wob-bootstrap` 模块的版本。

**核心认知：官方预编译镜像（Docker Hub 的 `acore/ac-wotlk-*:master`）不含本模块。
模块是 static 方式编译进 worldserver 的，必须本地重新构建镜像。**

## 1. 备份（必做，2 分钟）

```bash
cd /path/to/azerothcore-wotlk        # 服务器上的仓库目录
docker exec ac-database sh -c \
  'mysqldump -uroot -p"$MYSQL_ROOT_PASSWORD" --databases acore_auth acore_characters acore_world' \
  > backup-$(date +%F).sql
docker images | grep ac-wotlk         # 记下当前镜像 ID，回滚要用
```

角色/账号/世界数据都在 `ac-database` 这个 mysql volume 里，重建 worldserver 镜像
**不会**动它；备份只是保险。

## 2. 同步模块（每个模块拷一个目录）

本仓库只保留模块本身（`server/mod-*/`），把它们整个拷进服务器上原有的官方仓库即可。

- `mod-wob-bootstrap`：自动初始化 + 补给（`mod_wob_glyph.sql` 雕文表在模块目录里，
  worldserver 启动时自动应用，无需手动导库）。
- `mod-gear-wardrobe`：衣柜（白名单表默认不启用，可只拷代码）。

### 方式 A：scp（命令行）

```bash
# 在本地 Windows 的项目根目录执行（路径按实际服务器调整）
scp -r server/mod-wob-bootstrap \
    user@server:/path/to/azerothcore-wotlk/modules/
scp -r server/mod-gear-wardrobe \
    user@server:/path/to/azerothcore-wotlk/modules/
```

### 方式 B：MobaXterm 上传 zip（图形界面）

1. 本地把模块文件夹压缩成 zip（右键 → 压缩，得到 `mod-wob-bootstrap.zip`，
   压缩包内保留顶层文件夹 `mod-wob-bootstrap/`）。
2. MobaXterm SSH 连上服务器后，把 zip 拖进左侧的 SFTP 上传面板
   （默认传到当前目录，比如 `~`）。
3. 服务器上解压到 modules 目录（没有 unzip 就先 `apt install -y unzip`）：

   ```bash
   unzip ~/mod-wob-bootstrap.zip -d /path/to/azerothcore-wotlk/modules/
   unzip ~/mod-gear-wardrobe.zip  -d /path/to/azerothcore-wotlk/modules/
   ```

4. 回到项目根目录（`docker-compose.yml` 所在目录）继续第 3 步。

> 本项目的全部服务端改动都在这两个模块目录里，不需要动官方仓库的其他文件。
> 服务器 `modules/` 下如果还有第三方模块，别覆盖它们。

## 3. 重建镜像

```bash
docker compose up -d --build
```

- **必须带 `--build`**：不带的话 compose 继续用旧镜像（或按 `DOCKER_IMAGE_TAG` 拉
  官方预编译镜像），模块不会生效。
- 首次本地构建约 15–40 分钟（看 CPU）。内存建议 ≥4GB、8GB 更稳；不足会 OOM，
  加 swap 再试。`authserver` 会一起重建，正常。
- 构建带 ccache，以后再改模块重新构建只需几分钟。
- `mod_wob_glyph.sql` 由数据库更新器自动应用。
  如果环境关闭了自动更新，在启动 worldserver 之前把新 SQL 导入 `acore_world`。
- 出生霜纹包由建角钩子发放。若已导入过旧版 `mod_wob_starter_bags.sql`，
  部署前删除 `acore_world.playercreateinfo_item` 中 `race=0 AND class=0 AND itemid=41599` 的旧记录，避免重复发包。
- 批量换装协议需要配套更新客户端与 `mod-gear-wardrobe`。

### 构建报错排查：`ninja: error: manifest 'build.ninja' still dirty after 100 tries`

构建死在这一句、还没编译任何代码 = ninja 的时间戳判定死循环，**与模块代码无关**。
宿主机时钟（`date` / `timedatectl status` 确认 NTP 已同步）排除后，按成本从低到高试：

```bash
# 方法1：刷新模块文件时间戳（最快，10 秒）
cd /path/to/azerothcore-wotlk

# 1) 刷新 modules 下所有文件/目录的时间戳
find modules -exec touch -h {} +

# 2) 验证：以下命令必须无输出
NOW=$(date -u '+%Y-%m-%d %H:%M:%S')
TZ=UTC find modules -type f -newermt "$NOW" -print

# 3) 先构建 ac-db-import 验证（若仍死循环，约 100 秒内会报同样的错；
#    若通过，就开始正式全量编译，15–40 分钟）
docker compose build --progress=plain ac-db-import

# 方法2：报错一模一样还死在同一位置 → 脏缓存在 BuildKit/ccache 里，清掉重试
#          （会丢 ccache 加成，重编慢一点但更彻底）
docker builder prune -f  【实在不行了 再运行这一句】
docker compose up -d --build



```

判定技巧：touch 后错误**变了**（开始正常编译或报出别的错）= touch 起效；
错误**原样**= 直接跳第 2 步。

## 4. 数据去向（重建镜像后什么还在）

| 数据 | 位置 | 影响 |
| --- | --- | --- |
| 账号/角色/世界库 | `ac-database` volume | 保留 |
| worldserver 配置 | `./env/dist/etc` 挂载 | 保留 |
| 地图 dbc/maps/vmaps/mmaps | `ac-client-data` volume | 保留 |
| 新表 `mod_wob_bootstrap_done` | 角色库 | worldserver 首次启动自动建 |
| 新表 `mod_wob_glyph` | 世界库 | 模块目录内的 SQL 自动执行 |

## 5. 验证

```bash
docker logs ac-worldserver 2>&1 | grep -iE "wob|glyph class"
# 期望看到：[mod-wob-bootstrap] login <角色名>
#          [mod-wob-bootstrap] loaded 349 glyph class masks
```

然后建一个 1 级新角色登录一次，检查：

```sql
SELECT level, specsCount FROM acore_characters.characters WHERE name = '<角色名>';   -- 80, 2
SELECT skill, value, max FROM acore_characters.character_skills
  WHERE guid = <guid> AND skill IN (129, 197, 202);                                   -- 450
SELECT version FROM acore_characters.mod_wob_bootstrap_done WHERE guid = <guid>;      -- 4
-- v3: 武器技能学会并拉满 (43单手剑 44单手斧 54单手锤 136法杖 160双手锤 172双手斧
--      173匕首 226弩 228魔杖 229长柄 473拳套 等, 按职业不同):
SELECT skill, value, max FROM acore_characters.character_skills
  WHERE guid = <guid> AND skill IN (43,44,54,136,160,172,173,176,226,228,229,473);    -- value = max = 400
-- v4（仅 DK）：出生地任务标记完成（全表 59，按种族过滤后本角色 49 行），80 级天赋总点数 71
SELECT COUNT(*) FROM acore_characters.character_queststatus_rewarded r
  JOIN acore_world.quest_template q ON q.ID = r.quest
  WHERE r.guid = <guid> AND r.active = 1 AND q.QuestSortID = -372;                    -- 49
```

背包应有 6 种补给（34721/40771/41509/41166/42545/43523）。
v4 起 DK 还需验证：新建 DK 首次登录天赋总点数 71；v3 时期的老 DK 要**登录两次**
（第一次补任务行，第二次加载任务天赋后变 71）。
v3 起还应验证：技能书里能搜到 7 只坐骑（迅捷祖利安虎/拉扎什迅猛龙/乌鸦之主/
迅捷幽灵虎/黑色其拉作战坦克/两版霜刃豹，点一下即可召唤；
新建角色不再送幽灵虎/旅行者苔原猛犸/米米尔隆的头部/无头骑士坐骑）；施法不再消耗施法材料（如牧师的圣言术：盾
原本要神圣符号）。

## 6. 以后日常更新（改了模块代码后）

```bash
scp -r server/mod-wob-bootstrap server/mod-gear-wardrobe \
    user@server:/path/to/azerothcore-wotlk/modules/
ssh user@server "cd /path/to/azerothcore-wotlk && docker compose up -d --build"
```

## 7. 回滚

- **只关功能**：在 `env/dist/etc/modules/mod_wob_bootstrap.conf` 设
  `WobBootstrap.Enable = 0`（没有该文件就创建，内容抄
  `modules/mod-wob-bootstrap/conf/mod_wob_bootstrap.conf.dist`）重启。
- **换回原镜像**：
  ```bash
  docker tag <第 1 步记下的旧镜像ID> acore/ac-wotlk-worldserver:master
  docker compose up -d --no-build
  ```
- 已被初始化的角色状态（80 级、技能、补给）不会自动回滚，需要的话用 GM 命令
  或第 1 步的备份恢复角色库。

## 8. 关掉耐久度与无限弹药（已由模块全自动接管）

> [!TIP]
> **无需手动修改配置**：
> `mod-wob-bootstrap` 模块已内置 `WobBootstrapWorldScript::OnAfterConfigLoad`，在每次 worldserver 启动或执行 `.reload config` 时，会自动将以下 7 个耐久损耗率覆写为 0，并开启 `InfiniteAmmo.Enabled = 1`（无限弹药）。
> 因此以下第 8.1 ~ 8.2 节的 SSH 手动改配置文件步骤已**自动完成**，仅作为原理参考保留。

耐久损失全部由 worldserver 的 7 个配置键控制（战斗磨损/格挡/招架/死亡/摔死/
灵魂医者复活都读它们），全设 0 = 耐久永不下降。

要改的值：

```ini
DurabilityLoss.InPvP = 0
DurabilityLoss.OnDeath = 0
DurabilityLoss.OnSpiritResurrect = 0
DurabilityLossChance.Damage = 0
DurabilityLossChance.Absorb = 0
DurabilityLossChance.Parry = 0
DurabilityLossChance.Block = 0
```

### 8.1 conf 在哪（docker 部署的文件位置原理）

官方 compose 是**宿主机目录绑定挂载**（docker-compose.yml 里
`${DOCKER_VOL_ETC:-./env/dist/etc}:/azerothcore/env/dist/etc`）：
容器里的 `/azerothcore/env/dist/etc/worldserver.conf` 就是宿主机上
AC 仓库目录（docker-compose.yml 所在目录）下的 `env/dist/etc/worldserver.conf`，
**改宿主机文件 = 改容器配置**，不需要进容器，也不需要重建镜像。
（entrypoint 启动时只用 `cp -n` 补缺失的 conf，不会覆盖你改过的文件。）

### 8.2 详细步骤（SSH 到服务器上执行）

```bash
# 1. 进入 AC 仓库目录（docker-compose.yml 所在目录，路径按实际改）
cd /path/to/azerothcore-wotlk

# 2. 确认 etc 的实际挂载点（默认 ./env/dist/etc；
#    如果有人用 .env 覆盖过 DOCKER_VOL_ETC，这里会显示真实路径）
docker inspect ac-worldserver --format '{{range .Mounts}}{{.Source}} -> {{.Destination}}{{"\n"}}{{end}}' | grep etc

# 3. 一把梭改 7 个键（sed 只改以这些键开头的行，空格容忍）
cd env/dist/etc
sed -i -E \
  -e 's/^[# ]*DurabilityLoss\.InPvP[[:space:]]*=.*/DurabilityLoss.InPvP = 0/' \
  -e 's/^[# ]*DurabilityLoss\.OnDeath[[:space:]]*=.*/DurabilityLoss.OnDeath = 0/' \
  -e 's/^[# ]*DurabilityLoss\.OnSpiritResurrect[[:space:]]*=.*/DurabilityLoss.OnSpiritResurrect = 0/' \
  -e 's/^[# ]*DurabilityLossChance\.Damage[[:space:]]*=.*/DurabilityLossChance.Damage = 0/' \
  -e 's/^[# ]*DurabilityLossChance\.Absorb[[:space:]]*=.*/DurabilityLossChance.Absorb = 0/' \
  -e 's/^[# ]*DurabilityLossChance\.Parry[[:space:]]*=.*/DurabilityLossChance.Parry = 0/' \
  -e 's/^[# ]*DurabilityLossChance\.Block[[:space:]]*=.*/DurabilityLossChance.Block = 0/' \
  worldserver.conf

#    （不想用 sed 就 vi/nano worldserver.conf 手改这 7 行；MobaXterm 用户
#     也可以把文件拖下来改完再传回去——注意别改变文件名）

# 4. 核对结果（应输出 7 行，值全为 0）
grep -E '^DurabilityLoss' worldserver.conf

# 5. 让它生效（二选一）：
#    a) 热重载，不断线：GM 号在游戏里执行  .reload config
#       （这 7 个键全部标记为可热重载）
#    b) 重启容器（约几十秒）：
docker restart ac-worldserver
```

两个常见坑：

- **找不到 `worldserver.conf`（只有 `.dist` 结尾的文件）**：说明容器还没完整
  跑过首次生成。先 `docker compose up -d` 等容器起来一次（entrypoint 会把
  默认 conf 补进 etc 目录），再回来执行第 3 步。
- **sed 改了没生效**：确认改的是第 2 步查出来的**真实挂载路径**里的文件，
  而不是仓库里另一份；改完记得第 5 步二选一。

### 8.3 验证耐久确实关了

游戏里让角色挨打/死几次，再看装备耐久是否纹丝不动；或者对照法：衣柜
（`wob_wardrobe`）重穿一件 = 满耐久基线，打完一场战场后耐久应仍然满。

- 模块侧配套：每次登录免费修满存量损伤（`DurabilityRepairAll`），之后不再掉。
- 回滚：把 7 个键改回默认（10 / 25 / 0.5 / 0.5 / 0.05 / 0.05，InPvP=0），
  再 `.reload config` 或重启容器。

