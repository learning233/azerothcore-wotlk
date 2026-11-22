#include "Bag.h"
#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <memory>
#include <sstream>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::string_view CommandPrefix = "wob_wardrobe";
    using Entries = std::array<uint32, EQUIPMENT_SLOT_END>;
    using Equipment = std::array<Item*, EQUIPMENT_SLOT_END>;

    uint16 Position(uint8 bag, uint8 slot)
    {
        return uint16(bag) << 8 | slot;
    }

    bool ParseNumber(std::string const& text, uint32& value)
    {
        auto const end = text.data() + text.size();
        auto const result = std::from_chars(text.data(), end, value);
        return result.ec == std::errc{} && result.ptr == end;
    }

    void Reply(Player* player, uint32 request, std::string const& error)
    {
        ChatHandler(player->GetSession()).SendSysMessage(
            "衣柜：" + std::to_string(request) + (error.empty() ? " OK" : " ERR " + error));
    }

    bool CanGrant(ItemTemplate const* proto)
    {
        if (!proto || (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR) ||
            proto->Quality < ITEM_QUALITY_RARE || proto->RequiredLevel > 80)
            return false;

        return !sConfigMgr->GetOption<bool>("GearWardrobe.EnableWhitelist", false) ||
            WorldDatabase.Query("SELECT 1 FROM `mod_gear_wardrobe_whitelist` WHERE `entry` = {}", proto->ItemId) != nullptr;
    }

    bool BlocksOffhand(ItemTemplate const* proto, bool titanGrip)
    {
        return proto && proto->Class == ITEM_CLASS_WEAPON &&
            (proto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM || proto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF ||
             proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE || (proto->InventoryType == INVTYPE_2HWEAPON && !titanGrip));
    }

    struct BagSlot
    {
        uint16 position;
        ItemTemplate const* bag;
        Item* item;
    };

    std::vector<BagSlot> BagSlots(Player* player)
    {
        std::vector<BagSlot> slots;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            slots.push_back({Position(INVENTORY_SLOT_BAG_0, slot), nullptr, player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)});
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* container = player->GetBagByPos(bag))
                for (uint8 slot = 0; slot < container->GetBagSize(); ++slot)
                    slots.push_back({Position(bag, slot), container->GetTemplate(), container->GetItemByPos(slot)});
        return slots;
    }

    bool IsMovable(Player* player, Item* item)
    {
        return !item->IsInTrade() && !item->m_lootGenerated && item->GetGUID() != player->GetLootGUID() &&
            !item->IsBindedNotWith(player);
    }

    bool ValidateUniqueEquipment(Equipment const& target)
    {
        std::map<uint32, uint32> items;
        std::map<uint32, uint32> categories;
        auto add = [&](ItemTemplate const* proto)
        {
            if (proto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE) && ++items[proto->ItemId] > 1)
                return false;
            if (uint32 id = proto->ItemLimitCategory)
            {
                auto const* limit = sItemLimitCategoryStore.LookupEntry(id);
                if (!limit || ++categories[id] > limit->maxCount)
                    return false;
            }
            return true;
        };

        for (Item* item : target)
        {
            if (!item)
                continue;
            if (!add(item->GetTemplate()))
                return false;
            for (uint32 slot = SOCK_ENCHANTMENT_SLOT; slot <= PRISMATIC_ENCHANTMENT_SLOT; ++slot)
            {
                if (slot == BONUS_ENCHANTMENT_SLOT)
                    continue;
                uint32 enchant = item->GetEnchantmentId(EnchantmentSlot(slot));
                if (auto const* record = sSpellItemEnchantmentStore.LookupEntry(enchant))
                    if (auto const* gem = sObjectMgr->GetItemTemplate(record->GemID); gem && !add(gem))
                        return false;
            }
        }
        return true;
    }

    InventoryResult CheckEquip(Player* player, uint8 slot, Item* item, Equipment const& current, Equipment const& target)
    {
        uint16 dest;
        auto const* proto = item->GetTemplate();
        auto const* main = target[EQUIPMENT_SLOT_MAINHAND];
        bool const titanGrip = player->CanTitanGrip();
        if (slot == EQUIPMENT_SLOT_OFFHAND)
        {
            if (BlocksOffhand(main ? main->GetTemplate() : nullptr, titanGrip))
                return EQUIP_ERR_CANT_EQUIP_WITH_TWOHANDED;
            auto const type = proto->InventoryType;
            if ((type == INVTYPE_WEAPON || type == INVTYPE_WEAPONOFFHAND || type == INVTYPE_2HWEAPON) && !player->CanDualWield())
                return EQUIP_ERR_CANT_DUAL_WIELD;
            if ((type == INVTYPE_WEAPON || type == INVTYPE_2HWEAPON) && BlocksOffhand(proto, true))
                return EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
            if (type == INVTYPE_2HWEAPON && (!titanGrip || !main))
                return EQUIP_ERR_CANT_DUAL_WIELD;
        }

        // Titan's Grip FindEquipSlot checks the *current* main hand. For a valid final
        // pair, probe the same weapon as main hand if the old main hand blocks that slot.
        uint8 probeSlot = slot;
        if (slot == EQUIPMENT_SLOT_OFFHAND && proto->InventoryType == INVTYPE_2HWEAPON &&
            player->FindEquipSlot(proto, slot, true) != slot)
        {
            if (!player->CanUseAttackType(Player::GetAttackBySlot(slot)))
                return EQUIP_ERR_NOT_WHILE_DISARMED;
            probeSlot = EQUIPMENT_SLOT_MAINHAND;
        }

        auto result = player->CanEquipItem(probeSlot, dest, item, true);
        switch (result)
        {
            // The combined final equipment (including gems) is checked independently.
            case EQUIP_ERR_ITEM_UNIQUE_EQUIPABLE:
            case EQUIP_ERR_ITEM_MAX_COUNT_EQUIPPED_SOCKETED:
            case EQUIP_ERR_ITEM_MAX_LIMIT_CATEGORY_EQUIPPED_EXCEEDED:
                return EQUIP_ERR_OK;
            case EQUIP_ERR_CANT_EQUIP_WITH_TWOHANDED:
                if (slot == EQUIPMENT_SLOT_OFFHAND && current[EQUIPMENT_SLOT_MAINHAND] != main)
                    return EQUIP_ERR_OK;
                break;
            case EQUIP_ERR_ITEMS_CANT_BE_SWAPPED:
                // CanEquipItem checks the currently occupied bags for the displaced
                // offhand. This plan reserves space against the final bag layout.
                if (slot == EQUIPMENT_SLOT_MAINHAND && proto->InventoryType == INVTYPE_2HWEAPON &&
                    BlocksOffhand(proto, titanGrip) && current[EQUIPMENT_SLOT_OFFHAND] && !target[EQUIPMENT_SLOT_OFFHAND] &&
                    player->FindEquipSlot(proto, slot, true) == slot)
                    return EQUIP_ERR_OK;
                break;
            default:
                break;
        }
        return result;
    }

    std::string ApplyEquipment(Player* player, Entries const& desired)
    {
        if (player->GetLevel() < 80 || !player->IsAlive() || player->HasUnitState(UNIT_STATE_STUNNED) ||
            player->IsNonMeleeSpellCast(false))
            return "当前状态不能换装。";

        auto bags = BagSlots(player);
        Equipment current{}, target{};
        auto used = [&](Item* item) { return std::find(target.begin(), target.end(), item) != target.end(); };
        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            current[slot] = item;
            if (item && item->GetEntry() == desired[slot])
                target[slot] = item;
        }

        auto discard = [player](Item* item)
        {
            if (item)
            {
                item->RemoveFromUpdateQueueOf(player);
                delete item;
            }
        };
        using NewItem = std::unique_ptr<Item, decltype(discard)>;
        std::vector<NewItem> created;
        auto findGrant = [&](Item* item)
        {
            return std::find_if(created.begin(), created.end(), [&](NewItem const& pending) { return pending.get() == item; });
        };
        std::map<uint32, uint32> grants;
        std::map<uint32, uint32> grantedCategories;
        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            uint32 entry = desired[slot];
            if (!entry || target[slot])
                continue;

            auto matches = [&](Item* item) { return item && item->GetEntry() == entry && !used(item); };
            auto inBag = std::find_if(bags.begin(), bags.end(), [&](BagSlot const& cell) { return matches(cell.item); });
            auto worn = std::find_if(current.begin(), current.end(), matches);
            if (inBag != bags.end())
                target[slot] = inBag->item;
            else if (worn != current.end())
                target[slot] = *worn;
            else
            {
                auto const* proto = sObjectMgr->GetItemTemplate(entry);
                if (!CanGrant(proto) || player->CanTakeMoreSimilarItems(entry, ++grants[entry]) != EQUIP_ERR_OK)
                    return "该装备不能从衣柜领取（物品 " + std::to_string(entry) + "）。";
                if (uint32 id = proto->ItemLimitCategory)
                    if (auto const* limit = sItemLimitCategoryStore.LookupEntry(id);
                        limit && limit->mode == ITEM_LIMIT_CATEGORY_MODE_HAVE &&
                        player->GetItemCountWithLimitCategory(id) + ++grantedCategories[id] > limit->maxCount)
                        return "已达到同类物品持有限制。";

                NewItem item(Item::CreateItem(entry, 1, player), discard);
                if (!item)
                    return "创建装备失败。";
                target[slot] = item.get();
                created.push_back(std::move(item));
            }
        }

        if (auto* main = target[EQUIPMENT_SLOT_MAINHAND]; target[EQUIPMENT_SLOT_OFFHAND] &&
            BlocksOffhand(main ? main->GetTemplate() : nullptr, player->CanTitanGrip()))
            return "主手武器不能搭配副手装备。";
        if (!ValidateUniqueEquipment(target))
            return "目标配装中的装备或宝石违反唯一装备限制。";

        std::vector<std::pair<Item*, ItemPosCountVec>> toBag;
        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (current[slot] == target[slot])
                continue;
            if (Item* old = current[slot])
            {
                auto result = player->CanUnequipItem(Position(INVENTORY_SLOT_BAG_0, slot), false);
                if (!IsMovable(player, old) || result != EQUIP_ERR_OK)
                    return "当前不能卸下槽位 " + std::to_string(slot + 1) + " 的装备。";
                if (!used(old))
                {
                    // Reserve slots freed by incoming items as well as empty slots.
                    // Prefer compatible specialty bags, preserving general slots for other gear.
                    auto available = [&](BagSlot const& cell)
                    {
                        return !cell.item || used(cell.item);
                    };
                    auto place = std::find_if(bags.begin(), bags.end(), [&](BagSlot const& cell)
                    {
                        return available(cell) && cell.bag && cell.bag->SubClass != ITEM_SUBCLASS_CONTAINER &&
                            ItemCanGoIntoBag(old->GetTemplate(), cell.bag);
                    });
                    if (place == bags.end())
                        place = std::find_if(bags.begin(), bags.end(), [&](BagSlot const& cell)
                        {
                            return available(cell) && (!cell.bag || ItemCanGoIntoBag(old->GetTemplate(), cell.bag));
                        });
                    if (place == bags.end())
                        return "背包空间不足，未更改装备。";

                    toBag.emplace_back(old, ItemPosCountVec{ItemPosCount(place->position, old->GetCount())});
                    // Remove the reserved cell from the planner, not from the player's bags.
                    bags.erase(place);
                }
            }
            if (Item* item = target[slot])
            {
                auto result = CheckEquip(player, slot, item, current, target);
                if (!IsMovable(player, item) || result != EQUIP_ERR_OK)
                    return "当前不能穿上槽位 " + std::to_string(slot + 1) + " 的装备（错误 " +
                        std::to_string(result) + "）。";
            }
        }

        // All items, positions and rules have been checked. Detach moved instances
        // first so cycles (ring swaps, etc.) don't need a spare inventory slot.
        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
            if (current[slot] && current[slot] != target[slot])
                player->RemoveItem(INVENTORY_SLOT_BAG_0, slot, false);
        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = target[slot]; item && item != current[slot])
                if (item->GetSlot() != NULL_SLOT && findGrant(item) == created.end())
                    player->RemoveItem(item->GetBagSlot(), item->GetSlot(), false);

        for (auto const& [item, destination] : toBag)
            player->StoreItem(destination, item, true);
        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = target[slot]; item && item != current[slot])
            {
                uint16 pos = Position(INVENTORY_SLOT_BAG_0, slot);
                auto grant = findGrant(item);
                if (grant != created.end())
                {
                    grant->release();
                    if (sScriptMgr->OnPlayerCanSaveEquipNewItem(player, item, pos, true))
                    {
                        player->AdditionalSavingAddMask(ADDITIONAL_SAVING_INVENTORY_AND_GOLD);
                        player->ItemAddedQuestCheck(item->GetEntry(), 1);
                        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_RECEIVE_EPIC_ITEM, item->GetEntry(), 1);
                    }
                }
                player->EquipItem(pos, item, true);
            }
        for (auto const& move : toBag)
            sScriptMgr->OnPlayerUnequip(player, move.first);
        player->RemoveItemDependentAurasAndCasts(nullptr);
        player->UpdateTitansGrip();
        return {};
    }

    class GearWardrobePlayerScript : public PlayerScript
    {
    public:
        GearWardrobePlayerScript() : PlayerScript("GearWardrobePlayerScript") {}

        bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*language*/, std::string& msg) override
        {
            if (!msg.starts_with(CommandPrefix))
                return true;
            std::istringstream input(msg);
            std::string command, op, field;
            input >> command >> op >> field;
            if (command != CommandPrefix)
                return true;

            uint32 request = 0;
            Entries desired{};
            if (op != "set" || !ParseNumber(field, request) || !request)
            {
                Reply(player, request, "需要支持批量换装的客户端。");
                return false;
            }
            for (uint32& entry : desired)
                if (!(input >> field) || !ParseNumber(field, entry) || entry > 0xFFFFFF)
                {
                    Reply(player, request, "无效的配装数据。");
                    return false;
                }
            if (input >> field)
                Reply(player, request, "无效的配装数据。");
            else
                Reply(player, request, ApplyEquipment(player, desired));
            return false;
        }
    };
}

void AddSC_mod_gear_wardrobe()
{
    new GearWardrobePlayerScript();
}
