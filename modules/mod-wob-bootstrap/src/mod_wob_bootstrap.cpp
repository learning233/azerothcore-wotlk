#include "Bag.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DatabaseScript.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Optional.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include "World.h"
#include "WorldScript.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    struct WobBootstrapConfig
    {
        bool enabled = true;
        std::string refreshMode = "topup";
        bool glyphLibrary = true;
        bool stablePanel = true;
    };

    WobBootstrapConfig LoadConfig()
    {
        WobBootstrapConfig c;
        c.enabled = sConfigMgr->GetOption<bool>("WobBootstrap.Enable", true);
        c.refreshMode = sConfigMgr->GetOption<std::string>("WobBootstrap.RefreshMode", "topup");
        c.glyphLibrary = sConfigMgr->GetOption<bool>("WobBootstrap.GlyphLibrary", true);
        c.stablePanel = sConfigMgr->GetOption<bool>("WobBootstrap.StablePanel", true);
        return c;
    }

    struct SupplyItem
    {
        uint32 entry;
        uint32 count;
    };

    constexpr std::array<SupplyItem, 7> SUPPLIES = {{
        { 34721, 20 }, // 霜纹绷带
        { 40771, 20 }, // 钴制破片手雷
        { 41509, 20 }, // 霜纹投网
        { 41166, 20 }, // 符文治疗注射器
        { 42545, 20 }, // 符文法力注射器
        { 43523, 80 }, // 魔法酪饼，4 组
        { 8529, 40 },  // 诺格弗格药剂，2 组
    }};

    void EnsureLevel(Player* player, bool save = true)
    {
        if (player->GetLevel() >= 80)
            return;

        player->GiveLevel(80);
        player->InitTalentForLevel();
        if (save)
            player->SaveToDB(false, false);
        LOG_INFO("module", "[mod-wob-bootstrap] {} -> level 80", player->GetName());
    }

    // 黑锋要塞内的 DK 天赋点由核心特别计算（Player::CalculateTalentsPoints）：
    // (等级-55) + 出生地任务 RewardTalents 之和，且不超过 等级-9。
    // bootstrap 直接给 80 级时一个出生地任务都没完成，80-55 = 25 点；
    // 出生地任务对每个种族的奖励天赋点合计正好 46（全表 55 点，扣掉 9 个
    // 其它种族的 "A Special Surprise" 分身），25 + 46 = 71 = 满级上限。
    // 这里只把任务标记为已完成（写入 character_queststatus_rewarded），
    // 不发物品/金币/声望；死亡战马/黑锋之门等任务奖励法术已由
    // LearnClassSkills 通过 quest_template_addon.RequiredClasses 学到。
    void EnsureDeathKnightStartQuests(Player* player, bool save = true)
    {
        if (player->getClass() != CLASS_DEATH_KNIGHT)
            return;

        uint32 raceMask = player->getRaceMask();
        uint32 marked = 0;

        for (auto const& questPair : sObjectMgr->GetQuestTemplates())
        {
            Quest const* quest = questPair.second;
            // -372 = QuestSort.dbc「死亡骑士」；出生地全部任务都在这个分类下。
            if (quest->GetZoneOrSort() != -372)
                continue;
            // 10 个 "A Special Surprise" 按种族互斥，只标本种族的那个。
            if (quest->GetAllowableRaces() && !(quest->GetAllowableRaces() & raceMask))
                continue;

            uint32 questId = questPair.first;
            if (player->IsQuestRewarded(questId))
                continue;

            // 已在任务日志里的先清掉，和 RewardQuest 的处理保持一致。
            if (player->GetQuestStatus(questId) != QUEST_STATUS_NONE)
            {
                uint16 slot = player->FindQuestSlot(questId);
                if (slot < MAX_QUEST_LOG_SIZE)
                    player->SetQuestSlot(slot, 0);
                player->RemoveActiveQuest(questId, false);
            }

            player->SetRewardedQuest(questId);
            ++marked;
        }

        if (!marked)
            return;

        if (save)
            player->SaveToDB(false, false);
        LOG_INFO("module", "[mod-wob-bootstrap] {} marked {} DK start-zone quests rewarded",
                 player->GetName(), marked);
    }

    void PrepareStartingEquipment(Player* player)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (slot != EQUIPMENT_SLOT_MAINHAND && slot != EQUIPMENT_SLOT_OFFHAND && slot != EQUIPMENT_SLOT_RANGED)
                player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, false);

        constexpr uint32 StarterBag = 41599;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(StarterBag);
        if (!proto || proto->InventoryType != INVTYPE_BAG || proto->ContainerSlots != 20)
        {
            LOG_ERROR("module", "[mod-wob-bootstrap] Invalid starter bag template {}", StarterBag);
            return;
        }

        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        {
            std::vector<Item*> contents;
            if (Bag* old = player->GetBagByPos(slot))
            {
                if (old->GetEntry() == StarterBag)
                    continue;
                for (uint8 cell = 0; cell < old->GetBagSize(); ++cell)
                    if (Item* item = old->GetItemByPos(cell))
                        contents.push_back(item);
            }

            // DestroyItem on a bag also destroys its contents. Detach the items first.
            if (contents.size() > proto->ContainerSlots)
            {
                LOG_ERROR("module", "[mod-wob-bootstrap] {} starter bag slot {} has too many items",
                    player->GetName(), slot);
                continue;
            }
            Item* replacement = Item::CreateItem(StarterBag, 1, player);
            if (!replacement)
            {
                LOG_ERROR("module", "[mod-wob-bootstrap] Could not create starter bag for {}", player->GetName());
                continue;
            }

            for (Item* item : contents)
                player->RemoveItem(slot, item->GetSlot(), false);
            player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, false);
            player->EquipItem(uint16(INVENTORY_SLOT_BAG_0) << 8 | slot, replacement, false);
            for (uint8 cell = 0; cell < contents.size(); ++cell)
            {
                uint16 pos = uint16(slot) << 8 | cell;
                player->StoreItem(ItemPosCountVec{ItemPosCount(pos, contents[cell]->GetCount())}, contents[cell], false);
            }
        }
    }

    void LearnClassSkills(Player* player)
    {
        player->LearnDefaultSkills();

        std::vector<Trainer::Trainer const*> trainers;
        bool trainersOk = true;
        try
        {
            trainers = sObjectMgr->GetClassTrainers(player->getClass());
        }
        catch (std::out_of_range const&)
        {
            LOG_ERROR("module", "[mod-wob-bootstrap] no class trainers for class {}",
                uint32(player->getClass()));
            trainersOk = false;
        }

        if (trainersOk)
        {
            bool hadNew = false;
            do
            {
                hadNew = false;
                for (Trainer::Trainer const* trainer : trainers)
                {
                    if (!trainer->IsTrainerValidForPlayer(player))
                        continue;

                    for (Trainer::Spell const& trainerSpell : trainer->GetSpells())
                    {
                        if (!trainer->CanTeachSpell(player, &trainerSpell))
                            continue;

                        if (trainerSpell.IsCastable())
                            player->CastSpell(player, trainerSpell.SpellId, true);
                        else
                            player->learnSpell(trainerSpell.SpellId, false);

                        if (!trainer->CanTeachSpell(player, &trainerSpell))
                            hadNew = true;
                    }
                }
            } while (hadNew);
        }

        for (auto const& questPair : sObjectMgr->GetQuestTemplates())
        {
            Quest const* quest = questPair.second;
            if (quest->GetRequiredClasses() && player->SatisfyQuestClass(quest, false))
                player->learnQuestRewardedSpells(quest);
        }
    }

    // 专业只学阶位法术（学徒/大师级等）+ 拉满数值，不学制造配方：
    // 成品补给的使用只查技能数值（CanUseItem 看 RequiredSkill/RequiredSkillRank，
    // 6 种补给的 requiredspell 全是 0），配方无人消费；客户端也没有制造入口。
    void LearnProfessionRanks(Player* player, uint32 skillId)
    {
        for (uint32 rankSpell : sSpellMgr->GetSkillRankSpells(skillId))
            player->learnSpell(rankSpell);
    }

    void LearnProfessions(Player* player)
    {
        constexpr std::array<uint32, 3> skills = { 129, 197, 202 };
        for (uint32 skillId : skills)
        {
            LearnProfessionRanks(player, skillId);
            uint16 max = player->GetPureMaxSkillValue(skillId);
            player->SetSkill(skillId, player->GetSkillStep(skillId), max, max);
        }
    }

    void UnlockDualSpec(Player* player)
    {
        if (player->GetSpecsCount() < 2)
            player->UpdateSpecCount(2);

        if (!player->HasSpell(63644))
            player->learnSpell(63644);
        if (!player->HasSpell(63645))
            player->learnSpell(63645);
    }

    void EnsureRiding(Player* player)
    {
        // 骑术四档（trainer_spell 实际教学内容，不分阵营）：
        // 33388=初级75 / 33391=中级150 / 34090=高级225 / 34091=专家300。
        // 这些法术的 effect 是 SPELL_EFFECT_SKILL(118)，生效走 learnSpell -> AddSpell
        // 的 spellLearnSkill 分支（SetSkill(762, step, step*75, step*75)）；
        // 不能 CastSpell —— AC 的 Spell::EffectSkill 是空实现。
        // AddSpell 不检查前置档位，四档全学与真实训练师学满后的状态一致。
        for (uint32 spellId : { 33388u, 33391u, 34090u, 34091u })
            if (!player->HasSpell(spellId))
                player->learnSpell(spellId);

        // 寒冷天气飞行（诺森德飞行被动，68 级可学）
        if (!player->HasSpell(54197))
            player->learnSpell(54197);
    }

    // 武器技能全集（SkillLine 类别 6，id 以 SkillLine.dbc 为准）：
    // 43单手剑 44单手斧 45弓 46枪械 54单手锤 55双手剑 136法杖 160双手锤
    // 172双手斧 173匕首 176投掷武器 226弩 228魔杖 229长柄 473拳套。
    // LearnDefaultSkills 只给 playercreateinfo_skills 的种族/职业默认值（如猎人默认
    // 学不到剑）；武器大师可学的组合不在其中。不学会的话 CanUseItem 对带 RequiredSkill
    // 的武器直接拒装（NO_REQUIRED_PROFICIENCY）。LearnDefaultSkill 内部会用
    // SkillRaceClassInfo 按 种族×职业 过滤，学不了的自动跳过。
    constexpr std::array<uint32, 15> WEAPON_SKILLS = {{
        43u, 44u, 45u, 46u, 54u, 55u, 136u, 160u, 172u, 173u, 176u, 226u, 228u, 229u, 473u,
    }};

    void EnsureWeaponSkills(Player* player)
    {
        for (uint32 skillId : WEAPON_SKILLS)
            if (!player->HasSkill(skillId))
                player->LearnDefaultSkill(skillId, 0);
    }

    // 直送的坐骑（学会召唤法术，不进包）。声望/阵营/购买门槛都长在「物品」上，
    // 直接 learnSpell 法术不经过物品校验，法术本身只要求骑术（762），已学满 300。
    // 清单 = tools/export_mount_models.py 规划的 12 只；display→法术反查链见该脚本
    // 头注释（Spell.dbc aura78 MiscValue=creature entry → creature_template_model.sql
    // → CreatureDisplayInfo.dbc）。
    // 注意：其拉坦克只送**黑色**（26656，无区域限制）。红/蓝/黄/绿
    // （25953/26054/26055/26056）都带 AreaGroup=38 → 仅限安其拉区域 3428
    // 内召唤，野外骑不了，送了也没用。
    // 已移除：42776 幽灵虎（地面仅 60%）、61425/61447 旅行者的苔原猛犸象、
    // 63796 米米尔隆的头部、48025 无头骑士的坐骑（召唤 buff 图标与坐骑面板
    // 图标对不上，用户 2026-11-08 决定整只下架）。已学过的角色不做遗忘处理。
    constexpr std::array<uint32, 7> GRANTED_MOUNTS = {{
        24252u, // 迅捷祖利安猛虎（祖尔格拉布）
        24242u, // 拉扎什迅猛龙（祖尔格拉布）
        41252u, // 乌鸦之神（塞泰克大厅）
        42777u, // 迅捷幽灵虎（TCG 卡牌）
        26656u, // 黑色其拉作战坦克（全地图可骑；其余颜色仅限安其拉内）
        17229u, // 冬泉霜刃豹（声望）
        23221u, // 迅捷霜刃豹（声望）
    }};

    void EnsureMounts(Player* player)
    {
        for (uint32 spellId : GRANTED_MOUNTS)
            if (!player->HasSpell(spellId))
                player->learnSpell(spellId);
    }

    // 施法免材料：PLAYER_NO_REAGENT_COST_1..3 是 96 位法术家族掩码（竞技场准备阶段
    // 用的同一机制）。校验侧 Spell::CheckItems 与消耗侧 TakeReagents 都先走
    // CanNoReagentCast，命中即免查免扣。全 1 = 所有带家族标志的法术免材料
    // （家族标志为 0 的主要是制造业配方，本客户端无制造业入口）。字段不落库，
    // 必须每次登录都设。
    void EnsureNoReagent(Player* player)
    {
        // 萨满复生：AC Player::GetResurrectionSpellId 硬编码要求
        // HasAura(58059) || HasItemCount(17030)，不走 CanNoReagentCast。
        // 58059（新生雕文）属性 0xC0 = 被动且不显示在 Buff 栏、不占雕文槽。
        // 必须先挂 58059 再设 PLAYER_NO_REAGENT_COST_1..3，否则 AddAura 触发
        // HandleNoReagentUseAura 会覆写这 3 个字段。
        if (player->getClass() == CLASS_SHAMAN && !player->HasAura(58059))
            player->AddAura(58059, player);

        for (uint8 i = 0; i < 3; ++i)
        {
            if (player->GetUInt32Value(PLAYER_NO_REAGENT_COST_1 + i) != 0xFFFFFFFFu)
                player->SetUInt32Value(PLAYER_NO_REAGENT_COST_1 + i, 0xFFFFFFFFu);
        }
        // 清理旧版本可能残留的 46699 光环，防止锁死弹药栏并抹掉 91.5 伤害
        if (player->HasAura(46699))
            player->RemoveAura(46699);
    }

    // 补齐 PLAYER_NO_REAGENT_COST_1..3 掩码打不中的职业法术：
    //   1. SpellFamilyFlags == [0,0,0] 的 5 条职业法术（0 & 0xFFFFFFFF == 0）：
    //      1122 地狱火、29858 灵魂碎裂、19752 神圣干涉、43987/58659 召唤餐桌；
    //   2. 萨满 4 系图腾的 TotemCategory（2 大地 / 3 空气 / 4 火焰 / 5 水）：
    //      不走 CanNoReagentCast，没带圣物或图腾时无法插图腾。
    void PatchNoReagentSpellInfos()
    {
        constexpr std::array<uint32, 5> ZERO_FLAG_REAGENT_SPELLS = {{
            1122u,  // 地狱火（术士）
            29858u, // 灵魂碎裂（术士）
            19752u, // 神圣干涉（圣骑士）
            43987u, // 召唤餐桌 等级 1（法师）
            58659u, // 召唤餐桌 等级 2（法师）
        }};

        uint32 patchedReagents = 0;
        for (uint32 spellId : ZERO_FLAG_REAGENT_SPELLS)
        {
            if (SpellInfo* info = const_cast<SpellInfo*>(sSpellMgr->GetSpellInfo(spellId)))
            {
                info->Reagent.fill(0);
                info->ReagentCount.fill(0);
                ++patchedReagents;
            }
        }

        uint32 patchedTotems = 0;
        uint32 const storeSize = sSpellMgr->GetSpellInfoStoreSize();
        for (uint32 spellId = 1; spellId < storeSize; ++spellId)
        {
            SpellInfo const* cinfo = sSpellMgr->GetSpellInfo(spellId);
            if (!cinfo || cinfo->SpellFamilyName != SPELLFAMILY_SHAMAN)
                continue;

            bool changed = false;
            SpellInfo* info = const_cast<SpellInfo*>(cinfo);
            for (uint8 i = 0; i < 2; ++i)
            {
                if (info->TotemCategory[i] >= 2 && info->TotemCategory[i] <= 5)
                {
                    info->TotemCategory[i] = 0;
                    changed = true;
                }
            }
            if (changed)
                ++patchedTotems;
        }

        LOG_INFO("module",
                 "[mod-wob-bootstrap] 已免除 {} 条零家族掩码法术材料、{} 条萨满图腾道具门槛",
                 patchedReagents, patchedTotems);
    }

    // 登录免费修满存量耐久损伤（耐久损失已在服务端 conf 全关，之后不再掉）
    void EnsureDurabilityFull(Player* player)
    {
        player->DurabilityRepairAll(false, 0.0f, false);
    }

    // 弹药自适应：持有弓/弩自动给并绑定冰刃箭（+91.5 DPS），持有枪械自动给并绑定碎星弹（+91.5 DPS）
    // 配合全局开启的 InfiniteAmmo.Enabled = 1，背包只需 1 发代表弹药且永不消耗。
    void EnsureAmmo(Player* player)
    {
        Item* ranged = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (!ranged)
        {
            if (player->GetUInt32Value(PLAYER_AMMO_ID) != 0)
                player->RemoveAmmo();
            return;
        }

        ItemTemplate const* proto = ranged->GetTemplate();
        if (!proto || proto->Class != ITEM_CLASS_WEAPON)
        {
            if (player->GetUInt32Value(PLAYER_AMMO_ID) != 0)
                player->RemoveAmmo();
            return;
        }

        uint32 ammoEntry = 0;
        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                ammoEntry = 52021; // 冰刃箭 (80级顶级箭矢, +91.5 DPS)
                break;
            case ITEM_SUBCLASS_WEAPON_GUN:
                ammoEntry = 52020; // 碎星弹 (80级顶级枪弹, +91.5 DPS)
                break;
            default:
                if (player->GetUInt32Value(PLAYER_AMMO_ID) != 0)
                    player->RemoveAmmo();
                return;
        }

        // 确保背包里至少有 1 发该弹药（配合 InfiniteAmmo 永远不扣减）
        // 这样 Spell::CheckCast 的 HasItemCount 检查永远通过
        if (player->GetItemCount(ammoEntry, true) == 0)
        {
            uint32 count = 1;
            player->StoreNewItemInBestSlots(ammoEntry, count);
        }

        // 激活对应弹药（赋予 +91.5 DPS 并切换射击弹道特效）
        if (player->GetUInt32Value(PLAYER_AMMO_ID) != ammoEntry)
        {
            player->SetAmmo(ammoEntry);
        }
    }

    // v2: 补骑术四档 + 寒冷天气飞行
    // v3: 学会并拉满该职业全部武器技能；直送 12 只坐骑法术
    // v4: DK 出生地任务全部标记完成，补足 46 点任务天赋（80 级共 71 点）
    constexpr uint32 BOOTSTRAP_VERSION = 4;

    std::unordered_set<ObjectGuid::LowType> g_coreBootstrappedThisSession;
    std::unordered_map<ObjectGuid::LowType, uint32> g_petHappinessCheckTimer;
    // 脏天赋单次清洗标记：宠物异步加载可能晚于登录钩子，登录那一下可能扑空，
    // 用每 tick 轮询补一次，洗完即记，下线清除。
    std::unordered_set<ObjectGuid::LowType> g_petTalentCleaned;

    bool IsCoreBootstrapped(Player* player)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT version FROM mod_wob_bootstrap_done WHERE guid = {}",
            player->GetGUID().GetCounter());
        return result && result->Fetch()[0].Get<uint32>() >= BOOTSTRAP_VERSION;
    }

    void MarkCoreBootstrapped(Player* player)
    {
        CharacterDatabase.Execute(
            "INSERT INTO mod_wob_bootstrap_done (guid, version) VALUES ({}, {}) "
            "ON DUPLICATE KEY UPDATE version = VALUES(version)",
            player->GetGUID().GetCounter(), BOOTSTRAP_VERSION);
    }

    void EnsureCoreBootstrap(Player* player, bool force = false)
    {
        ObjectGuid::LowType guid = player->GetGUID().GetCounter();
        if (!force && g_coreBootstrappedThisSession.contains(guid))
            return;

        if (!force && IsCoreBootstrapped(player))
        {
            g_coreBootstrappedThisSession.insert(guid);
            return;
        }

        EnsureLevel(player);
        EnsureDeathKnightStartQuests(player);
        // 骑术要在职业训练师法术之前：圣骑 23214 / 术士 23161 的职业坐骑
        // trainer 前置是中级骑术 33391，先学骑术首次初始化才能学到
        EnsureRiding(player);
        EnsureMounts(player);
        LearnClassSkills(player);
        LearnProfessions(player);
        UnlockDualSpec(player);
        EnsureWeaponSkills(player);
        // 拉满全部已学的非专业/非骑术技能：武器熟练度 1/400 -> 400/400（专业已由
        // LearnProfessions 自己拉满，骑术保持 300/300）
        player->UpdateSkillsToMaxSkillsForLevel();
        player->SaveToDB(false, false);
        MarkCoreBootstrapped(player);
        g_coreBootstrappedThisSession.insert(guid);
    }

    uint32 CountInBackpack(Player* player, uint32 entry)
    {
        uint32 count = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (item->GetEntry() == entry)
                    count += item->GetCount();
        }
        return count;
    }

    uint32 CountBackpackSpace(Player* player, uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto)
            return 0;

        uint32 maxStack = proto->GetMaxStackSize();
        uint32 space = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                space += maxStack;
            else if (item->GetEntry() == entry && item->GetCount() < maxStack)
                space += maxStack - item->GetCount();
        }
        return space;
    }

    void TopUpSupplies(Player* player)
    {
        WobBootstrapConfig cfg = LoadConfig();
        bool exact = cfg.refreshMode == "exact";

        for (SupplyItem const& supply : SUPPLIES)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(supply.entry);
            uint32 total = player->GetItemCount(supply.entry, true);

            if (exact && total > supply.count)
            {
                player->DestroyItemCount(supply.entry, total - supply.count, true, false);
                total = supply.count;
            }

            // 只看主背包：自定义客户端的 ConsumableBar 只读 INVENTORY_SLOT_BAG_0 的 16 格。
            uint32 carried = CountInBackpack(player, supply.entry);
            if (carried >= supply.count)
                continue;

            uint32 missing = supply.count - carried;

            // MaxCount（如 43523=80）按“背包+银行”总量限制；不能把超出上限的部分
            // 反复邮寄。allowed = 还能真实增加的数量。
            uint32 allowed = missing;
            if (proto && proto->MaxCount > 0)
            {
                uint32 maxCount = uint32(proto->MaxCount);
                allowed = total >= maxCount ? 0 : (maxCount - total);
                if (allowed > missing)
                    allowed = missing;
            }
            if (allowed == 0)
                continue;

            uint32 storable = CountBackpackSpace(player, supply.entry);
            if (storable > allowed)
                storable = allowed;

            if (storable)
            {
                ItemPosCountVec dest;
                InventoryResult msg = player->CanStoreNewItem(
                    INVENTORY_SLOT_BAG_0, NULL_SLOT, dest, supply.entry, storable);

                if (msg == EQUIP_ERR_OK)
                    player->StoreNewItem(dest, supply.entry, true);
                else
                {
                    player->SendItemRetrievalMail(supply.entry, allowed);
                    LOG_INFO("module", "[mod-wob-bootstrap] {} main bag full, mailed {} x{}",
                             player->GetName(), supply.entry, allowed);
                    continue;
                }
            }

            uint32 mailed = missing - storable;
            if (mailed > allowed - storable)
                mailed = allowed - storable;
            if (mailed)
            {
                player->SendItemRetrievalMail(supply.entry, mailed);
                LOG_INFO("module", "[mod-wob-bootstrap] {} main bag full, mailed {} x{}",
                         player->GetName(), supply.entry, mailed);
            }
        }
    }

    void ApplyBootEnchant(Player* player)
    {
        Item* boots = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_FEET);
        if (!boots)
            return;

        SpellInfo const* nitro = sSpellMgr->GetSpellInfo(55016);
        if (!nitro || !nitro->Effects[0].MiscValue)
            return;

        uint32 enchantId = nitro->Effects[0].MiscValue;
        if (boots->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) == enchantId)
            return;

        boots->SetEnchantment(PERM_ENCHANTMENT_SLOT, enchantId, 0, 0, player->GetGUID());
        player->ApplyEnchantment(boots, PERM_ENCHANTMENT_SLOT, true);
        boots->SetState(ITEM_CHANGED, player);
        LOG_INFO("module", "[mod-wob-bootstrap] applied Nitro Boosts to {}", player->GetName());
    }

    bool HasAnyUnslottedHunterPet(PetStable const& stable)
    {
        for (PetStable::PetInfo const& info : stable.UnslottedPets)
            if (info.Type == HUNTER_PET)
                return true;
        return false;
    }

    // 官方口径：家族被动 + 按等级解锁的家族/默认技能。
    // 不再遍历原始 SkillLineAbility，避免把玩家天赋 aura 触发的宠物技能
    // （24394/75446/75447/75593）当家族技能白送。
    void LearnPetSkills(Pet* pet)
    {
        if (!pet)
            return;

        pet->LearnPetPassives();
        pet->InitLevelupSpellsForLevel();
    }

    // 洗掉历史上被误授的天赋：官方学天赋只认本系树（LearnPetTalent 防作弊校验），
    // 跨系/玩家系的一律是脏数据。删完调 InitTalentForLevel 对账（超标自动全额退款）。
    void CleanIllegitimatePetTalents(Pet* pet)
    {
        if (!pet || pet->getPetType() != HUNTER_PET || pet->isBeingLoaded())
            return;

        CreatureTemplate const* ci = pet->GetCreatureTemplate();
        if (!ci)
            return;

        CreatureFamilyEntry const* family = sCreatureFamilyStore.LookupEntry(ci->family);
        if (!family || family->petTalentType < 0)
            return;

        std::vector<uint32> illegitimate;
        for (auto const& spellPair : pet->m_spells)
        {
            uint32 spellId = spellPair.first;
            if (spellPair.second.state == PETSPELL_REMOVED)
                continue;
            if (GetTalentSpellCost(spellId) == 0)
                continue;

            bool legitimate = false;
            if (TalentSpellPos const* pos = GetTalentSpellPos(spellId))
            {
                if (TalentEntry const* talentInfo = sTalentStore.LookupEntry(pos->talent_id))
                {
                    if (TalentTabEntry const* tabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab))
                        legitimate = ((1 << family->petTalentType) & tabInfo->petTalentMask) != 0;
                }
            }
            if (!legitimate)
                illegitimate.push_back(spellId);
        }

        if (!illegitimate.empty())
        {
            for (uint32 spellId : illegitimate)
                pet->unlearnSpell(spellId, false);
            LOG_INFO("module", "[mod-wob-bootstrap] removed {} illegitimate pet talents for {}",
                     illegitimate.size(), pet->GetName());
        }

        pet->InitTalentForLevel();
    }

    // 兽栏槽位：原生 characters.stable_slots 默认 0。没有槽位时
    // CMSG_STABLE_PET 的 for (freeSlot < MaxStabledPets) 一次都不进，
    // 服务端直接回 STABLE_ERR_STABLE(6) —— 客户端表现为「存放到兽栏」
    // 必失败。本服固定免费给满 4 格并落库，内存/DB 保持一致。
    void EnsureStableSlots(Player* player, PetStable& stable)
    {
        if (stable.MaxStabledPets >= MAX_PET_STABLES)
            return;

        LOG_INFO("module",
                 "[mod-wob-bootstrap] {} stable slots {} -> {}",
                 player->GetName(), stable.MaxStabledPets, MAX_PET_STABLES);
        stable.MaxStabledPets = MAX_PET_STABLES;
        CharacterDatabase.Execute(
            "UPDATE characters SET stable_slots = {} WHERE guid = {}",
            MAX_PET_STABLES, player->GetGUID().GetCounter());
    }

    bool HasFreeStableSlot(PetStable const& stable)
    {
        for (Optional<PetStable::PetInfo> const& slot : stable.StabledPets)
            if (!slot)
                return true;
        return false;
    }

    // healHealth：登录/驯服时补满生命；开兽栏/刷新只补快乐度，不免费回血。
    void EnsurePetLevel(Player* player, bool healHealth = false)
    {
        if (player->getClass() != CLASS_HUNTER)
            return;

        PetStable& stable = player->GetOrInitPetStable();
        EnsureStableSlots(player, stable);

        // 排障：把服务端 PetStable 里实际装了几只宠物打出来。
        // stabled 只在 slot 1..4 里；unslotted 是 DB slot=100 的那些
        // （原生列表最多只显示 1 只 unslotted）。
        uint32 stabled = 0;
        for (Optional<PetStable::PetInfo> const& slot : stable.StabledPets)
            if (slot)
                ++stabled;
        uint32 unslotted = 0;
        for (PetStable::PetInfo const& info : stable.UnslottedPets)
            if (info.Type == HUNTER_PET)
                ++unslotted;
        LOG_INFO("module",
                 "[mod-wob-bootstrap] hunter {} pets: current={} stabled={}/4 "
                 "unslotted={} stableSlots={}",
                 player->GetName(), stable.CurrentPet ? 1 : 0, stabled,
                 unslotted, stable.MaxStabledPets);

        // 外部模块塞进来的多只宠物通常落在 slot=100（UnslottedPets）。
        // 原生 SendStablePet 只显示 1 只 unslotted，其余在兽栏里永远看不见；
        // 这里把它们依次放进空槽（最多 4 格）并同步 DB 的 slot。
        // 关键：只移除真正搬走的条目；槽满时没搬走的必须留在 UnslottedPets，
        // 否则宠物会从内存态消失（DB 仍是 slot=100，重登后又被同样逻辑藏起来）。
        std::vector<PetStable::PetInfo> keptUnslotted;
        keptUnslotted.reserve(stable.UnslottedPets.size());
        uint8 freeSlot = 1;
        for (PetStable::PetInfo& info : stable.UnslottedPets)
        {
            if (info.Type != HUNTER_PET)
            {
                keptUnslotted.push_back(std::move(info));
                continue;
            }
            while (freeSlot <= MAX_PET_STABLES && stable.StabledPets[freeSlot - 1])
                ++freeSlot;
            if (freeSlot > MAX_PET_STABLES)
            {
                // 没有空槽：保留，等玩家腾出位置后下次开栏再收
                keptUnslotted.push_back(std::move(info));
                continue;
            }
            LOG_INFO("module",
                     "[mod-wob-bootstrap] move pet {} (entry {}) into stable slot {}",
                     info.PetNumber, info.CreatureId, freeSlot);
            CharacterDatabase.Execute(
                "UPDATE character_pet SET slot = {} WHERE owner = {} AND id = {}",
                static_cast<uint32>(freeSlot), player->GetGUID().GetCounter(),
                info.PetNumber);
            stable.StabledPets[freeSlot - 1] = std::move(info);
            ++freeSlot;
        }
        stable.UnslottedPets = std::move(keptUnslotted);

        // 先把这只猎人的所有宠物 DB 等级拉到 80（包括兽栏里未召唤的）
        CharacterDatabase.Execute(
            "UPDATE character_pet SET level = 80 WHERE owner = {} AND PetType = 1",
            player->GetGUID().GetCounter());

        // 同步内存里的 PetStable，兽栏面板直接读它，不重登也要显示 80
        if (stable.CurrentPet)
            stable.CurrentPet->Level = 80;

        for (Optional<PetStable::PetInfo>& slot : stable.StabledPets)
            if (slot)
                slot->Level = 80;

        for (PetStable::PetInfo& info : stable.UnslottedPets)
            if (info.Type == HUNTER_PET)
                info.Level = 80;

        Pet* pet = player->GetPet();
        if (!pet || pet->getPetType() != HUNTER_PET)
            return;

        if (pet->GetLevel() < 80)
        {
            pet->SetLevel(80);
            pet->InitStatsForLevel(80);
            pet->InitTalentForLevel();
        }

        LearnPetSkills(pet);
        CleanIllegitimatePetTalents(pet);
        // LearnPetPassives 走 addSpell，不发包；补一次全量，避免客户端漏掉新被动。
        if (!pet->isBeingLoaded())
            player->PetSpellInitialize();
        if (healHealth)
            pet->SetFullHealth();
        // 快乐度始终补满：客户端没有补快乐值的系统，服务端 30 秒轮询是主路径，
        // 这里作为登录/开栏兜底。
        pet->SetPower(POWER_HAPPINESS, pet->GetMaxPower(POWER_HAPPINESS));
        pet->SavePetToDB(PET_SAVE_AS_CURRENT);
    }

    // 不创建默认宠：登录时 AC 已由 Player::LoadPet 按 character_pet.slot=0
    // 恢复上次出战宠物；这里只做等级/兽栏/技能/天赋/快乐度维护。
    void MaintainHunterPet(Player* player)
    {
        if (player->getClass() != CLASS_HUNTER)
            return;

        EnsurePetLevel(player, true);
    }

    std::unordered_map<uint32, uint32> g_glyphClassMask;

    void LoadGlyphClassMask()
    {
        g_glyphClassMask.clear();
        QueryResult result = WorldDatabase.Query(
            "SELECT glyph_id, class_mask FROM mod_wob_glyph");

        if (!result)
        {
            LOG_WARN("module", "[mod-wob-bootstrap] mod_wob_glyph is empty; class check disabled");
            return;
        }

        do
        {
            Field* fields = result->Fetch();
            g_glyphClassMask[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
        } while (result->NextRow());

        LOG_INFO("module", "[mod-wob-bootstrap] loaded {} glyph class masks",
                 g_glyphClassMask.size());
    }

    void ApplyGlyph(Player* player, uint8 slot, uint32 glyphId)
    {
        if (slot >= MAX_GLYPH_SLOT_INDEX)
        {
            ChatHandler(player->GetSession()).SendSysMessage("雕文槽位必须是 0..5。");
            return;
        }

        GlyphPropertiesEntry const* glyph = sGlyphPropertiesStore.LookupEntry(glyphId);
        if (!glyph)
        {
            ChatHandler(player->GetSession()).SendSysMessage("雕文不存在。");
            return;
        }

        GlyphSlotEntry const* slotEntry = sGlyphSlotStore.LookupEntry(player->GetGlyphSlot(slot));
        if (!slotEntry || glyph->TypeFlags != slotEntry->TypeFlags)
        {
            ChatHandler(player->GetSession()).SendSysMessage("该雕文与槽位类型不匹配。");
            return;
        }

        auto itr = g_glyphClassMask.find(glyphId);
        if (itr != g_glyphClassMask.end() && itr->second &&
            (itr->second & player->getClassMask()) == 0)
        {
            ChatHandler(player->GetSession()).SendSysMessage("该雕文不属于你的职业。");
            return;
        }

        if (uint32 oldGlyph = player->GetGlyph(slot))
        {
            if (GlyphPropertiesEntry const* old = sGlyphPropertiesStore.LookupEntry(oldGlyph))
            {
                player->RemoveAurasDueToSpell(old->SpellId);

                // 和 Spell::EffectApplyGlyph 一样：清掉由旧雕文触发的衍生光环
                Unit::AuraMap& ownedAuras = player->GetOwnedAuras();
                for (auto iter = ownedAuras.begin(); iter != ownedAuras.end();)
                {
                    if (SpellInfo const* triggered = iter->second->GetTriggeredByAuraSpellInfo())
                    {
                        if (triggered->Id == old->SpellId)
                        {
                            player->RemoveOwnedAura(iter);
                            continue;
                        }
                    }
                    ++iter;
                }

                player->SendLearnPacket(old->SpellId, false);
            }
        }

        player->SetGlyph(slot, glyphId, true);
        player->CastSpell(player, glyph->SpellId, true);
        player->SendLearnPacket(glyph->SpellId, true);
        player->SendTalentsInfoData(false);
        // 装卸免材料小雕文会触发 HandleNoReagentUseAura 覆写 PLAYER_NO_REAGENT_COST_1..3
        // （或摘掉新生雕文 58059），这里立即补回。
        EnsureNoReagent(player);
        ChatHandler(player->GetSession()).SendSysMessage("雕文已更新。");
    }

    // 官方驯服逻辑：新宠直接成为出战宠物（EffectTameCreature / Player::CreatePet）。
    // 驯服时不允许有出战宠物或未归栏宠物；有出战宠物先提示存入兽栏（存栏需要空位）。
    bool TamePet(Player* player, uint32 entry)
    {
        if (player->getClass() != CLASS_HUNTER)
        {
            ChatHandler(player->GetSession()).SendSysMessage("只有猎人才能驯服宠物。");
            return false;
        }

        if (!player->IsAlive())
        {
            ChatHandler(player->GetSession()).SendSysMessage("死亡状态不能驯服宠物。");
            return false;
        }

        if (player->IsInCombat())
        {
            ChatHandler(player->GetSession()).SendSysMessage("战斗中不能驯服宠物。");
            return false;
        }

        CreatureTemplate const* ci = sObjectMgr->GetCreatureTemplate(entry);
        if (!ci || !ci->IsTameable(true))
        {
            ChatHandler(player->GetSession()).SendSysMessage("无法驯服该生物（必须是可驯服的野兽）。");
            return false;
        }

        if (ci->IsExotic() && !player->CanTameExoticPets())
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "这是特殊野兽，需要点出兽王系终极天赋【野兽控制大师】才能驯服。");
            return false;
        }

        if (!ci->GetFirstValidModel())
        {
            ChatHandler(player->GetSession()).SendSysMessage("该生物没有可用模型，无法驯服。");
            return false;
        }

        PetStable& stable = player->GetOrInitPetStable();
        EnsureStableSlots(player, stable);

        bool const stableFull = !HasFreeStableSlot(stable);
        if (player->GetPet() || stable.CurrentPet)
        {
            if (stableFull)
                ChatHandler(player->GetSession()).SendSysMessage(
                    "兽栏已满（4/4）。请先放生/取出一只兽栏宠物腾出空位，或放生当前宠物，再来驯服。");
            else
                ChatHandler(player->GetSession()).SendSysMessage(
                    "请先把当前宠物存入兽栏（兽栏有空位），再来驯服。");
            return false;
        }

        if (HasAnyUnslottedHunterPet(stable))
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "你还有未归栏的宠物，请先打开兽栏存入空位（兽栏满则先放生/取出一只），再驯服。");
            return false;
        }

        if (player->IsMounted())
            player->RemoveAurasByType(SPELL_AURA_MOUNTED);

        Pet* pet = player->CreatePet(entry);
        if (!pet)
        {
            ChatHandler(player->GetSession()).SendSysMessage("驯服失败，请稍后重试。");
            return false;
        }

        pet->SetLevel(80);
        pet->InitStatsForLevel(80);
        pet->InitTalentForLevel();
        pet->SetReactState(REACT_DEFENSIVE);
        LearnPetSkills(pet);
        CleanIllegitimatePetTalents(pet);
        pet->SetFullHealth();
        pet->SetPower(POWER_HAPPINESS, pet->GetMaxPower(POWER_HAPPINESS));
        pet->SavePetToDB(PET_SAVE_AS_CURRENT);
        player->PetSpellInitialize();
        player->GetSession()->SendStablePet(player->GetGUID());

        ChatHandler(player->GetSession()).SendSysMessage(
            Acore::StringFormat("已驯服【{}】作为出战宠物。", pet->GetName()));
        LOG_INFO("module", "[mod-wob-bootstrap] tamed pet {} (entry {}) for {}",
                 pet->GetName(), entry, player->GetName());
        return true;
    }

    // 只放生出战宠物。官方协议同样只支持放生世界中的宠物，
    // 兽栏里的必须先取出（兽栏放生入口已删除）。
    bool AbandonPet(Player* player, uint32 petNumber)
    {
        if (player->getClass() != CLASS_HUNTER)
            return false;

        Pet* currentPet = player->GetPet();
        if (currentPet && currentPet->getPetType() == HUNTER_PET &&
            currentPet->GetCharmInfo()->GetPetNumber() == petNumber)
        {
            std::string name = currentPet->GetName();
            player->RemovePet(currentPet, PET_SAVE_AS_DELETED);
            player->GetSession()->SendStablePet(player->GetGUID());
            ChatHandler(player->GetSession()).SendSysMessage(
                Acore::StringFormat("出战宠物【{}】已放生。", name));
            return true;
        }

        ChatHandler(player->GetSession()).SendSysMessage("只能放生出战宠物，兽栏宠物请先取出。");
        return false;
    }

    // 与 AC HandleListStabledPetsOpcode 一致：先清假死/坐骑，避免带马取宠删宠，
    // 然后补兽栏槽位/等级，并每次重上 aura 62757 续 2 分钟窗口。
    void SendStableList(Player* player)
    {
        if (player->HasUnitState(UNIT_STATE_DIED))
            player->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);
        if (player->IsMounted())
            player->RemoveAurasByType(SPELL_AURA_MOUNTED);

        // 临时收起（坐骑/载具/传送等）后如果当前宠还没恢复，开兽栏时先尝试恢复；
        // 载具/传送期间 Resummon 会自己 return，只能等状态解除。
        if (!player->GetPet() && player->GetTemporaryUnsummonedPetNumber())
            player->ResummonPetTemporaryUnSummonedIfAny();

        EnsurePetLevel(player);

        // 62757 = 召唤兽栏宠物，aura 292 = SPELL_AURA_OPEN_STABLE（2 分钟）。
        // 只在没有 aura 时才加会导致面板开久后窗口过期、列表请求静默失败；
        // 这里每次开/刷新都移除重上，续满时长。
        player->RemoveAurasDueToSpell(62757);
        if (!player->AddAura(62757, player))
            LOG_WARN("module", "[mod-wob-bootstrap] AddAura(62757) failed for {}",
                     player->GetName());

        // aura 的 apply handler 已经会 SendStablePet；这里再发一次保证 UI 拿到列表
        player->GetSession()->SendStablePet(player->GetGUID());
    }

    void OpenStable(Player* player)
    {
        SendStableList(player);
    }

    void RefreshStable(Player* player)
    {
        SendStableList(player);
    }

    bool HandleWobCommand(Player* player, std::string const& msg)
    {
        WobBootstrapConfig cfg = LoadConfig();
        std::istringstream iss(msg);
        std::string cmd;
        iss >> cmd;

        if (cmd == "wob_reset_talents")
        {
            if (player->IsInCombat())
            {
                ChatHandler(player->GetSession()).SendSysMessage("战斗中不能重置天赋。");
                return true;
            }
            if (player->HasUnitState(UNIT_STATE_DIED))
                player->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

            if (player->resetTalents(true))
            {
                player->SendTalentsInfoData(false);
                player->CastSpell(player, 14867, true); // spell: "Untalent Visual Effect"
                LOG_INFO("module", "[mod-wob-bootstrap] reset talents for {}", player->GetName());
                ChatHandler(player->GetSession()).SendSysMessage("天赋已重置，点数已全部返还。");
            }
            else
            {
                ChatHandler(player->GetSession()).SendSysMessage("当前方案没有已分配的天赋点。");
            }
            return true;
        }
        if (cmd == "wob_reset_pet_talents")
        {
            if (player->getClass() != CLASS_HUNTER)
            {
                ChatHandler(player->GetSession()).SendSysMessage("只有猎人可以重置宠物天赋。");
                return true;
            }
            if (player->IsInCombat())
            {
                ChatHandler(player->GetSession()).SendSysMessage("战斗中不能重置宠物天赋。");
                return true;
            }
            Pet* pet = player->GetPet();
            if (!pet || pet->getPetType() != HUNTER_PET)
            {
                ChatHandler(player->GetSession()).SendSysMessage("当前没有出战宠物，请先召唤要重置的宠物（只重置出战宠物，兽栏中的不受影响）。");
                return true;
            }
            if (pet->resetTalents())
            {
                player->SendTalentsInfoData(true);
                player->CastSpell(player, 14867, true); // spell: "Untalent Visual Effect"
                LOG_INFO("module", "[mod-wob-bootstrap] reset talents for hunter {} current pet", player->GetName());
                ChatHandler(player->GetSession()).SendSysMessage("出战宠物的天赋已重置，点数已全部返还（兽栏中的宠物不受影响）。");
            }
            else
            {
                ChatHandler(player->GetSession()).SendSysMessage("出战宠物当前没有已分配的天赋点。");
            }
            return true;
        }
        if (cmd == "wob_poison")
        {
            if (player->getClass() != CLASS_ROGUE)
            {
                ChatHandler(player->GetSession()).SendSysMessage("只有盗贼可以涂抹毒药。");
                return true;
            }
            if (player->IsInCombat())
            {
                ChatHandler(player->GetSession()).SendSysMessage("战斗中无法涂抹毒药。");
                return true;
            }

            uint32 slot = 0;
            std::string poisonType;
            if (!(iss >> slot >> poisonType) || (slot != EQUIPMENT_SLOT_MAINHAND && slot != EQUIPMENT_SLOT_OFFHAND))
            {
                ChatHandler(player->GetSession()).SendSysMessage("用法：wob_poison <15|16> <wound|deadly|instant|crippling|mind|clear>");
                return true;
            }

            Item* weapon = player->GetItemByPos(INVENTORY_SLOT_BAG_0, static_cast<uint8>(slot));
            if (!weapon || !weapon->GetTemplate() || weapon->GetTemplate()->Class != ITEM_CLASS_WEAPON)
            {
                ChatHandler(player->GetSession()).SendSysMessage("目标槽位没有装备可用武器。");
                return true;
            }

            uint32 enchantId = 0;
            std::string poisonName;
            if (poisonType == "wound")
            {
                enchantId = 3773; // 致伤药膏 VII
                poisonName = "致伤药膏 VII";
            }
            else if (poisonType == "deadly")
            {
                enchantId = 3771; // 致命药膏 IX
                poisonName = "致命药膏 IX";
            }
            else if (poisonType == "instant")
            {
                enchantId = 3769; // 速效药膏 IX
                poisonName = "速效药膏 IX";
            }
            else if (poisonType == "crippling")
            {
                enchantId = 22;   // 减速药膏
                poisonName = "减速药膏";
            }
            else if (poisonType == "mind")
            {
                enchantId = 35;   // 麻痹药膏
                poisonName = "麻痹药膏";
            }
            else if (poisonType == "clear")
            {
                enchantId = 0;
            }
            else
            {
                ChatHandler(player->GetSession()).SendSysMessage("未知毒药类型，可用：wound / deadly / instant / crippling / mind / clear");
                return true;
            }

            player->ApplyEnchantment(weapon, TEMP_ENCHANTMENT_SLOT, false);
            std::string const slotDesc = (slot == EQUIPMENT_SLOT_MAINHAND) ? "主手" : "副手";
            if (enchantId > 0)
            {
                weapon->SetEnchantment(TEMP_ENCHANTMENT_SLOT, enchantId, 3600 * 1000, 0, player->GetGUID());
                player->ApplyEnchantment(weapon, TEMP_ENCHANTMENT_SLOT, true);
                ChatHandler(player->GetSession()).SendSysMessage(
                    Acore::StringFormat("已为{}武器涂抹【{}】（持续 1 小时）。", slotDesc, poisonName));
            }
            else
            {
                weapon->ClearEnchantment(TEMP_ENCHANTMENT_SLOT);
                ChatHandler(player->GetSession()).SendSysMessage(
                    Acore::StringFormat("已清除{}武器的临时附魔/涂毒。", slotDesc));
            }
            return true;
        }
        if (cmd == "wob_stable" && cfg.stablePanel)
        {
            OpenStable(player);
            return true;
        }
        if (cmd == "wob_stable_refresh" && cfg.stablePanel)
        {
            RefreshStable(player);
            return true;
        }
        if (cmd == "wob_pet_adopt")
        {
            uint32 entry = 0;
            if (!(iss >> entry) || !entry)
            {
                ChatHandler(player->GetSession()).SendSysMessage("用法：wob_pet_adopt <entry>");
                return true;
            }
            TamePet(player, entry);
            return true;
        }
        if (cmd == "wob_pet_abandon")
        {
            uint32 petNumber = 0;
            if (!(iss >> petNumber) || !petNumber)
            {
                ChatHandler(player->GetSession()).SendSysMessage("用法：wob_pet_abandon <petNumber>");
                return true;
            }
            AbandonPet(player, petNumber);
            return true;
        }
        if (cmd == "wob_boots")
        {
            ApplyBootEnchant(player);
            return true;
        }
        if (cmd == "wob_bootstrap")
        {
            if (player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
                return true;

            // 强制重跑：清掉本进程 guard，并让 EnsureCoreBootstrap 跳过 DB marker 检查。
            g_coreBootstrappedThisSession.erase(player->GetGUID().GetCounter());
            EnsureCoreBootstrap(player, true);
            TopUpSupplies(player);
            ApplyBootEnchant(player);
            MaintainHunterPet(player);
            EnsureNoReagent(player);
            EnsureDurabilityFull(player);
            EnsureAmmo(player);
            return true;
        }
        if (cmd == "wob_glyph" && cfg.glyphLibrary)
        {
            uint32 slot = 0;
            uint32 glyphId = 0;
            if (!(iss >> slot >> glyphId))
            {
                ChatHandler(player->GetSession()).SendSysMessage("用法：wob_glyph <0..5> <glyphId>");
                return true;
            }
            ApplyGlyph(player, static_cast<uint8>(slot), glyphId);
            return true;
        }

        return false;
    }

    class WobBootstrapWorldScript : public WorldScript
    {
    public:
        WobBootstrapWorldScript() : WorldScript("WobBootstrapWorldScript") {}

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            // 1. 关掉全部 7 项耐久损失（全设 0 = 装备永不磨损，无需手动改 worldserver.conf）
            sWorld->setRate(RATE_DURABILITY_LOSS_ON_DEATH, 0.0f);
            sWorld->setRate(RATE_DURABILITY_LOSS_ON_SPIRIT_RESURRECT, 0.0f);
            sWorld->setRate(RATE_DURABILITY_LOSS_DAMAGE, 0.0f);
            sWorld->setRate(RATE_DURABILITY_LOSS_ABSORB, 0.0f);
            sWorld->setRate(RATE_DURABILITY_LOSS_PARRY, 0.0f);
            sWorld->setRate(RATE_DURABILITY_LOSS_BLOCK, 0.0f);
            sWorld->setBoolConfig(CONFIG_DURABILITY_LOSS_IN_PVP, false);

            // 2. 开启无限弹药（射击不扣减数量）
            sWorld->setBoolConfig(CONFIG_ENABLE_INFINITEAMMO, true);

            LOG_INFO("module", "[mod-wob-bootstrap] 覆写全局配置：耐久磨损=0 (7项全部置0)，无限弹药=开");
        }

        void OnStartup() override
        {
            if (!LoadConfig().enabled)
                return;
            PatchNoReagentSpellInfos();
        }
    };

    class WobBootstrapPlayerScript : public PlayerScript
    {
    public:
        WobBootstrapPlayerScript() : PlayerScript("WobBootstrapPlayerScript") {}

        void OnPlayerCreate(Player* player) override
        {
            if (!LoadConfig().enabled) return;
            LOG_INFO("module", "[mod-wob-bootstrap] OnPlayerCreate {}", player->GetName());
            EnsureLevel(player, false);
            EnsureDeathKnightStartQuests(player, false);
            PrepareStartingEquipment(player);
            player->SetFullHealth();

            // The core invokes this hook after its initial save. Wait for the new
            // inventory to persist before the core acknowledges character creation.
            auto transaction = CharacterDatabase.BeginTransaction();
            player->SaveToDB(transaction, false, false);
            auto result = CharacterDatabase.AsyncCommitTransaction(transaction);
            if (!result.m_future.get())
                LOG_ERROR("module", "[mod-wob-bootstrap] Could not save starting equipment for {}", player->GetName());
        }

        void OnPlayerFirstLogin(Player* player) override
        {
            if (!LoadConfig().enabled) return;
            LOG_INFO("module", "[mod-wob-bootstrap] first login {}", player->GetName());
            EnsureCoreBootstrap(player);
            MaintainHunterPet(player);
            TopUpSupplies(player);
            ApplyBootEnchant(player);
            EnsureNoReagent(player);
            EnsureDurabilityFull(player);
            EnsureAmmo(player);
        }

        void OnPlayerLogin(Player* player) override
        {
            if (!LoadConfig().enabled) return;
            LOG_INFO("module", "[mod-wob-bootstrap] login {}", player->GetName());
            EnsureCoreBootstrap(player);
            MaintainHunterPet(player);
            TopUpSupplies(player);
            ApplyBootEnchant(player);
            EnsureNoReagent(player);
            EnsureDurabilityFull(player);
            EnsureAmmo(player);
        }

        void OnPlayerEquip(Player* player, Item* /*it*/, uint8 /*bag*/, uint8 slot, bool /*update*/) override
        {
            if (slot == EQUIPMENT_SLOT_RANGED)
                EnsureAmmo(player);
        }

        void OnPlayerUnequip(Player* player, Item* /*it*/) override
        {
            EnsureAmmo(player);
        }

        void OnPlayerLogout(Player* player) override
        {
            g_petHappinessCheckTimer.erase(player->GetGUID().GetCounter());
            g_petTalentCleaned.erase(player->GetGUID().GetCounter());
        }

        void OnPlayerUpdate(Player* player, uint32 diff) override
        {
            // 兜底：防止任意光环/天赋切换触发 HandleNoReagentUseAura 把免材料掩码冲回 0，
            // 或萨满新生雕文被动意外丢失。正常情况下仅比较 3 个 u32，零发包。
            bool needReagentRestore =
                player->GetUInt32Value(PLAYER_NO_REAGENT_COST_1) != 0xFFFFFFFFu ||
                player->GetUInt32Value(PLAYER_NO_REAGENT_COST_1 + 1) != 0xFFFFFFFFu ||
                player->GetUInt32Value(PLAYER_NO_REAGENT_COST_1 + 2) != 0xFFFFFFFFu ||
                (player->getClass() == CLASS_SHAMAN && !player->HasAura(58059));
            if (needReagentRestore)
                EnsureNoReagent(player);

            if (player->getClass() != CLASS_HUNTER)
                return;

            // 脏天赋单次兜底：宠物加载晚于登录钩子时，登录那次清洗会扑空，
            // 这里等宠进世界后补洗一次。
            if (!g_petTalentCleaned.contains(player->GetGUID().GetCounter()))
            {
                if (Pet* pet = player->GetPet())
                {
                    if (pet->getPetType() == HUNTER_PET)
                    {
                        CleanIllegitimatePetTalents(pet);
                        g_petTalentCleaned.insert(player->GetGUID().GetCounter());
                    }
                }
            }

            uint32& timer = g_petHappinessCheckTimer[player->GetGUID().GetCounter()];
            timer += diff;
            if (timer >= 30000) // 每 30 秒轮询一次
            {
                timer = 0;
                if (Pet* pet = player->GetPet())
                {
                    // 仅当宠物掉出 HAPPY（125% 伤害档位）时才拉回，平时 0 开销零发包
                    if (pet->getPetType() == HUNTER_PET && pet->GetHappinessState() != HAPPY)
                    {
                        pet->SetPower(POWER_HAPPINESS, pet->GetMaxPower(POWER_HAPPINESS));
                    }
                }
            }
        }

        bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*language*/, std::string& msg) override
        {
            if (!LoadConfig().enabled) return true;
            // 前缀不能带点：`.` 开头的消息会被 AC 内置命令系统（Chat.cpp 的
            // ParseCommands）先行消费，AllowPlayerCommands=1 或 GM 账号下
            // 未知点命令直接被吃，到不了本钩子。
            if (msg.rfind("wob_", 0) != 0) return true;
            if (msg.rfind("wob_wardrobe", 0) == 0) return true;
            HandleWobCommand(player, msg);
            return false; // 所有 wob_* 都被本模块吞掉，不广播
        }
    };

    class WobBootstrapDatabaseScript : public DatabaseScript
    {
    public:
        WobBootstrapDatabaseScript() : DatabaseScript("WobBootstrapDatabaseScript") {}

        bool OnModuleDatabasesLoading() override
        {
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS mod_wob_bootstrap_done ("
                "guid int unsigned NOT NULL PRIMARY KEY, "
                "version int unsigned NOT NULL DEFAULT 0, "
                "updated_at timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP "
                "ON UPDATE CURRENT_TIMESTAMP) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

            // 此时 world DB 已经 open；如果 mod_wob_glyph.sql 还没执行，
            // 这里会返回 null 并打印 warning，职业校验暂时禁用。
            LoadGlyphClassMask();
            return true;
        }
    };
}

void AddSC_mod_wob_bootstrap()
{
    new WobBootstrapWorldScript();
    new WobBootstrapDatabaseScript();
    new WobBootstrapPlayerScript();
}
