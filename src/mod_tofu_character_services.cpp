/*
 * mod-tofu-character-services (Tofu Worldview)
 *
 * One NPC, Archivist Ellery Ashby <Character Services>, with five services:
 * - Arrange my characters: reorder the characters on the account (writes characters.order, which the core's
 *   character list query sorts by). Ported from Souicia/mod-character-order (MIT, see NOTICE), which also gives
 *   the player command .charorder.
 * - Change my name, Change my race, Change my faction: set the core's at-login flag (AT_LOGIN_RENAME,
 *   AT_LOGIN_CHANGE_RACE, AT_LOGIN_CHANGE_FACTION), the same flags .character rename, changerace and changefaction
 *   set. The 3.3.5a client then shows its own rename, race change or faction change screen at the character list,
 *   and the core's CharacterHandler does the conversion.
 * - Leave combat: ends the player's combat state outside instances when nothing alive is attacking nearby.
 * Menu style after azerothcore/mod-npc-services (inline icons on each option); no code is copied from it.
 *
 * Bots never get these services: every playerbot session is headless (no socket), so WorldSession::IsHeadless()
 * identifies them without depending on mod-playerbots.
 */

#include "AuctionHouseMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "WorldSession.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // npc_text rows from this module's SQL (registry: npc_text 900018 to 900019).
    constexpr uint32 NPC_TEXT_MAIN = 900018;
    constexpr uint32 NPC_TEXT_ORDER = 900019;

    // Player setting that remembers what was paid for the pending change, so a cancel refunds exactly that.
    constexpr char SETTING_SOURCE[] = "mod-tofu-character-services";
    constexpr uint32 SETTING_PAID = 0;

    // Gossip senders of the main menu. The character order senders keep the upstream values.
    constexpr uint32 SENDER_MAIN = 0x54430001;

    constexpr uint32 ACTION_ORDER = 1;
    constexpr uint32 ACTION_RENAME = 2;
    constexpr uint32 ACTION_RACE = 3;
    constexpr uint32 ACTION_FACTION = 4;
    constexpr uint32 ACTION_LEAVE_COMBAT = 5;
    constexpr uint32 ACTION_CANCEL = 6;
    constexpr uint32 ACTION_GOODBYE = 7;
    constexpr uint32 ACTION_BACK_TO_MAIN = 8;

    constexpr uint32 CHARACTER_ORDER_SENDER_SELECT = 0x434F0001;
    constexpr uint32 CHARACTER_ORDER_SENDER_SELECTED = 0x434F0002;
    constexpr uint32 CHARACTER_ORDER_SENDER_NAVIGATION = 0x434F0003;
    constexpr uint32 CHARACTER_ORDER_SENDER_MOVE_UP = 0x434F0004;
    constexpr uint32 CHARACTER_ORDER_SENDER_MOVE_DOWN = 0x434F0005;
    constexpr uint32 CHARACTER_ORDER_SENDER_MOVE_TOP = 0x434F0006;
    constexpr uint32 CHARACTER_ORDER_SENDER_MOVE_BOTTOM = 0x434F0007;

    constexpr uint32 CHARACTER_ORDER_ACTION_BACK = 1;
    constexpr uint32 CHARACTER_ORDER_ACTION_MOVE_UP = 2;
    constexpr uint32 CHARACTER_ORDER_ACTION_MOVE_DOWN = 3;
    constexpr uint32 CHARACTER_ORDER_ACTION_MOVE_TOP = 4;
    constexpr uint32 CHARACTER_ORDER_ACTION_MOVE_BOTTOM = 5;

    constexpr uint32 PENDING_MASK = AT_LOGIN_RENAME | AT_LOGIN_CHANGE_RACE | AT_LOGIN_CHANGE_FACTION;

    struct ModuleConfig
    {
        bool enable = false;
        bool command = true;
        uint32 renameCost = 0;
        uint32 raceCost = 0;
        uint32 factionCost = 0;
        bool leaveCombat = true;
        float leaveCombatRange = 50.0f;
    };

    ModuleConfig g_Config;

    void LoadConfig()
    {
        g_Config.enable = sConfigMgr->GetOption<bool>("TofuCharServices.Enable", false);
        g_Config.command = sConfigMgr->GetOption<bool>("TofuCharServices.OrderCommand", true);
        g_Config.renameCost = sConfigMgr->GetOption<uint32>("TofuCharServices.RenameCost", 0);
        g_Config.raceCost = sConfigMgr->GetOption<uint32>("TofuCharServices.RaceChangeCost", 0);
        g_Config.factionCost = sConfigMgr->GetOption<uint32>("TofuCharServices.FactionChangeCost", 0);
        g_Config.leaveCombat = sConfigMgr->GetOption<bool>("TofuCharServices.LeaveCombat.Enable", true);
        g_Config.leaveCombatRange = sConfigMgr->GetOption<float>("TofuCharServices.LeaveCombat.Range", 50.0f);
    }

    std::string Icon(char const* name)
    {
        return std::string("|TInterface\\Icons\\") + name + ":30:30:-18:0|t ";
    }

    void Notify(Player* player, std::string const& text)
    {
        ChatHandler handler(player->GetSession());
        handler.SendNotification(text);
        handler.SendSysMessage(text);
    }

    std::string MoneyText(uint32 copper)
    {
        uint32 gold = copper / 10000;
        uint32 silver = (copper % 10000) / 100;
        uint32 rest = copper % 100;
        std::string text;
        if (gold)
            text += std::to_string(gold) + "g ";
        if (silver)
            text += std::to_string(silver) + "s ";
        if (rest || text.empty())
            text += std::to_string(rest) + "c ";
        text.pop_back();
        return text;
    }

    // Where a menu is shown from: the NPC (creature GUID) or the .charorder command (the player's own GUID).
    ObjectGuid MenuSource(Player* player, Creature* creature)
    {
        return creature ? creature->GetGUID() : player->GetGUID();
    }

    // ---------------------------------------------------------------------------------------------------------
    // Character order (ported from Souicia/mod-character-order 2270973)
    // ---------------------------------------------------------------------------------------------------------

    struct CharacterOrderEntry
    {
        uint32 Guid;
        std::string Name;
        uint8 Level;
        uint8 Race;
        uint8 Class;
    };

    char const* GetRaceName(uint8 race)
    {
        switch (race)
        {
            case 1:  return "Human";
            case 2:  return "Orc";
            case 3:  return "Dwarf";
            case 4:  return "Night Elf";
            case 5:  return "Undead";
            case 6:  return "Tauren";
            case 7:  return "Gnome";
            case 8:  return "Troll";
            case 10: return "Blood Elf";
            case 11: return "Draenei";
            default: return "Unknown Race";
        }
    }

    char const* GetClassName(uint8 playerClass)
    {
        switch (playerClass)
        {
            case 1:  return "Warrior";
            case 2:  return "Paladin";
            case 3:  return "Hunter";
            case 4:  return "Rogue";
            case 5:  return "Priest";
            case 6:  return "Death Knight";
            case 7:  return "Shaman";
            case 8:  return "Mage";
            case 9:  return "Warlock";
            case 11: return "Druid";
            default: return "Unknown Class";
        }
    }

    // Class colors darkened so they stay readable on the gossip window's parchment (stock priest white and rogue
    // yellow are not).
    char const* GetClassColor(uint8 playerClass)
    {
        switch (playerClass)
        {
            case 1:  return "7A5230";
            case 2:  return "B0406F";
            case 3:  return "4F6E1F";
            case 4:  return "7D6E00";
            case 5:  return "5A5A5A";
            case 6:  return "A01A30";
            case 7:  return "0058B0";
            case 8:  return "1A6E91";
            case 9:  return "5E4396";
            case 11: return "B35300";
            default: return "000000";
        }
    }

    std::vector<CharacterOrderEntry> LoadAccountCharacters(Player* player)
    {
        std::vector<CharacterOrderEntry> characters;

        if (!player || !player->GetSession())
            return characters;

        uint32 accountId = player->GetSession()->GetAccountId();

        QueryResult result = CharacterDatabase.Query(
            "SELECT guid, name, level, race, class "
            "FROM characters "
            "WHERE account = {} "
            "AND deleteInfos_Name IS NULL "
            "ORDER BY COALESCE(`order`, guid), guid",
            accountId);

        if (!result)
            return characters;

        do
        {
            Field* fields = result->Fetch();

            CharacterOrderEntry entry;
            entry.Guid = fields[0].Get<uint32>();
            entry.Name = fields[1].Get<std::string>();
            entry.Level = fields[2].Get<uint8>();
            entry.Race = fields[3].Get<uint8>();
            entry.Class = fields[4].Get<uint8>();

            characters.push_back(entry);
        }
        while (result->NextRow());

        return characters;
    }

    void SaveCharacterOrder(Player* player, std::vector<CharacterOrderEntry> const& characters)
    {
        if (!player || !player->GetSession() || characters.empty())
            return;

        uint32 accountId = player->GetSession()->GetAccountId();

        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();

        for (std::size_t index = 0; index < characters.size(); ++index)
        {
            transaction->Append(
                "UPDATE characters "
                "SET `order` = {} "
                "WHERE guid = {} "
                "AND account = {} "
                "AND deleteInfos_Name IS NULL",
                static_cast<uint32>(index + 1),
                characters[index].Guid,
                accountId);
        }

        CharacterDatabase.DirectCommitTransaction(transaction);
    }

    std::string BuildCharacterText(CharacterOrderEntry const& entry, std::size_t position)
    {
        return std::to_string(position) + ". |cff" + GetClassColor(entry.Class) + entry.Name + "|r - Level " +
            std::to_string(entry.Level) + " " + GetRaceName(entry.Race) + " |cff" + GetClassColor(entry.Class) +
            GetClassName(entry.Class) + "|r";
    }

    void ShowCharacterList(Player* player, Creature* creature)
    {
        if (!player)
            return;

        ClearGossipMenuFor(player);

        std::vector<CharacterOrderEntry> characters = LoadAccountCharacters(player);

        if (characters.empty())
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "No characters were found on this account.", CHARACTER_ORDER_SENDER_NAVIGATION, CHARACTER_ORDER_ACTION_BACK);
        }
        else
        {
            for (std::size_t index = 0; index < characters.size(); ++index)
            {
                CharacterOrderEntry const& entry = characters[index];
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, BuildCharacterText(entry, index + 1), CHARACTER_ORDER_SENDER_SELECT, entry.Guid);
            }
        }

        if (creature)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to character services", SENDER_MAIN, ACTION_BACK_TO_MAIN);

        SendGossipMenuFor(player, NPC_TEXT_ORDER, MenuSource(player, creature));
    }

    void ShowSelectedCharacter(Player* player, Creature* creature, uint32 selectedGuid)
    {
        if (!player)
            return;

        // Refetch on every selection instead of trusting the gossip action.
        std::vector<CharacterOrderEntry> characters = LoadAccountCharacters(player);

        auto selected = std::find_if(characters.begin(), characters.end(),
            [selectedGuid](CharacterOrderEntry const& entry) { return entry.Guid == selectedGuid; });

        // Invalid or forged GUID: return safely to the authenticated account's character list.
        if (selected == characters.end())
        {
            ShowCharacterList(player, creature);
            return;
        }

        std::size_t position = static_cast<std::size_t>(std::distance(characters.begin(), selected)) + 1;

        ClearGossipMenuFor(player);

        std::string information = "|cff" + std::string(GetClassColor(selected->Class)) + selected->Name + "|r - Level " +
            std::to_string(selected->Level) + " " + GetRaceName(selected->Race) + " " + GetClassName(selected->Class) +
            " - Position " + std::to_string(position) + " of " + std::to_string(characters.size());

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, information, CHARACTER_ORDER_SENDER_SELECTED, selected->Guid);

        if (position > 1)
            AddGossipItemFor(player, GOSSIP_ICON_DOT, "Move up", CHARACTER_ORDER_SENDER_MOVE_UP, selected->Guid);

        if (position < characters.size())
            AddGossipItemFor(player, GOSSIP_ICON_DOT, "Move down", CHARACTER_ORDER_SENDER_MOVE_DOWN, selected->Guid);

        if (position > 1)
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Move to top", CHARACTER_ORDER_SENDER_MOVE_TOP, selected->Guid);

        if (position < characters.size())
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Move to bottom", CHARACTER_ORDER_SENDER_MOVE_BOTTOM, selected->Guid);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Return to character list", CHARACTER_ORDER_SENDER_NAVIGATION, CHARACTER_ORDER_ACTION_BACK);

        SendGossipMenuFor(player, NPC_TEXT_ORDER, MenuSource(player, creature));
    }

    void MoveCharacter(Player* player, Creature* creature, uint32 selectedGuid, uint32 movement)
    {
        if (!player || !player->GetSession())
            return;

        // Refetch before every movement. Never trust the gossip GUID.
        std::vector<CharacterOrderEntry> characters = LoadAccountCharacters(player);

        auto selected = std::find_if(characters.begin(), characters.end(),
            [selectedGuid](CharacterOrderEntry const& entry) { return entry.Guid == selectedGuid; });

        // The GUID is invalid or does not belong to this account.
        if (selected == characters.end())
        {
            ShowCharacterList(player, creature);
            return;
        }

        std::size_t index = static_cast<std::size_t>(std::distance(characters.begin(), selected));

        bool changed = false;

        switch (movement)
        {
            case CHARACTER_ORDER_ACTION_MOVE_UP:
                if (index > 0)
                {
                    std::swap(characters[index], characters[index - 1]);
                    changed = true;
                }
                break;

            case CHARACTER_ORDER_ACTION_MOVE_DOWN:
                if (index + 1 < characters.size())
                {
                    std::swap(characters[index], characters[index + 1]);
                    changed = true;
                }
                break;

            case CHARACTER_ORDER_ACTION_MOVE_TOP:
                if (index > 0)
                {
                    CharacterOrderEntry movedCharacter = characters[index];
                    characters.erase(characters.begin() + index);
                    characters.insert(characters.begin(), movedCharacter);
                    changed = true;
                }
                break;

            case CHARACTER_ORDER_ACTION_MOVE_BOTTOM:
                if (index + 1 < characters.size())
                {
                    CharacterOrderEntry movedCharacter = characters[index];
                    characters.erase(characters.begin() + index);
                    characters.push_back(movedCharacter);
                    changed = true;
                }
                break;

            default:
                break;
        }

        if (changed)
            SaveCharacterOrder(player, characters);

        // Stay on the selected character and display its new position.
        ShowSelectedCharacter(player, creature, selectedGuid);
    }

    // Returns true when the sender belongs to the character order menu.
    bool HandleOrderSelect(Player* player, Creature* creature, uint32 sender, uint32 action)
    {
        switch (sender)
        {
            case CHARACTER_ORDER_SENDER_SELECT:
            case CHARACTER_ORDER_SENDER_SELECTED:
                ShowSelectedCharacter(player, creature, action);
                return true;
            case CHARACTER_ORDER_SENDER_MOVE_UP:
                MoveCharacter(player, creature, action, CHARACTER_ORDER_ACTION_MOVE_UP);
                return true;
            case CHARACTER_ORDER_SENDER_MOVE_DOWN:
                MoveCharacter(player, creature, action, CHARACTER_ORDER_ACTION_MOVE_DOWN);
                return true;
            case CHARACTER_ORDER_SENDER_MOVE_TOP:
                MoveCharacter(player, creature, action, CHARACTER_ORDER_ACTION_MOVE_TOP);
                return true;
            case CHARACTER_ORDER_SENDER_MOVE_BOTTOM:
                MoveCharacter(player, creature, action, CHARACTER_ORDER_ACTION_MOVE_BOTTOM);
                return true;
            case CHARACTER_ORDER_SENDER_NAVIGATION:
                if (action == CHARACTER_ORDER_ACTION_BACK)
                    ShowCharacterList(player, creature);
                return true;
            default:
                return false;
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Name, race, and faction change
    // ---------------------------------------------------------------------------------------------------------

    struct ServiceInfo
    {
        AtLoginFlags flag;
        char const* name;
        uint32 cost;
    };

    ServiceInfo GetService(uint32 action)
    {
        switch (action)
        {
            case ACTION_RENAME:  return { AT_LOGIN_RENAME, "name change", g_Config.renameCost };
            case ACTION_RACE:    return { AT_LOGIN_CHANGE_RACE, "race change", g_Config.raceCost };
            default:             return { AT_LOGIN_CHANGE_FACTION, "faction change", g_Config.factionCost };
        }
    }

    bool HasPendingChange(Player* player)
    {
        return player->HasAtLoginFlag(AtLoginFlags(PENDING_MASK));
    }

    // Same refusals as the core's faction change (CharacterHandler::HandleCharFactionOrRaceChangeCallback): mail in
    // the mailbox, or an auction the character owns or bids on. This server allows cross-faction guilds and arena
    // teams, so those are not checked here; the core still checks them at the character screen.
    bool HasFactionChangeBlocker(Player* player, std::string& reason)
    {
        if (CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(player->GetGUID()))
        {
            if (cache->MailCount)
            {
                reason = "Empty your mailbox first: a character with mail cannot change faction.";
                return true;
            }
        }

        for (AuctionHouseId house : { AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral })
        {
            AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMapByHouseId(house);
            if (!auctionHouse)
                continue;

            for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
            {
                if (auction && (auction->owner == player->GetGUID() || auction->bidder == player->GetGUID()))
                {
                    reason = "Finish or cancel your auctions and bids first: a character with auctions cannot change faction.";
                    return true;
                }
            }
        }

        return false;
    }

    // Common refusals for name, race, and faction change. Returns true and sets reason when the player may not.
    bool IsRefused(Player* player, std::string& reason)
    {
        if (!g_Config.enable)
        {
            reason = "Character services are closed.";
            return true;
        }
        if (player->GetSession()->IsHeadless())
        {
            reason = "Bots cannot use character services.";
            return true;
        }
        if (player->IsInCombat())
        {
            reason = "You cannot do that while in combat.";
            return true;
        }
        if (player->InBattleground() || player->InArena())
        {
            reason = "You cannot do that in a battleground or arena.";
            return true;
        }
        if (HasPendingChange(player))
        {
            reason = "You already have a change waiting. Cancel it first, or log out to finish it.";
            return true;
        }
        return false;
    }

    void BuyService(Player* player, uint32 action)
    {
        ServiceInfo const service = GetService(action);
        CloseGossipMenuFor(player);

        std::string reason;
        if (IsRefused(player, reason) || (service.flag == AT_LOGIN_CHANGE_FACTION && HasFactionChangeBlocker(player, reason)))
        {
            Notify(player, reason);
            return;
        }

        if (service.cost && !player->HasEnoughMoney(service.cost))
        {
            Notify(player, "You need " + MoneyText(service.cost) + " for a " + service.name + ".");
            return;
        }

        if (service.cost)
            player->ModifyMoney(-int32(service.cost));

        player->UpdatePlayerSetting(SETTING_SOURCE, SETTING_PAID, service.cost);
        player->SetAtLoginFlag(service.flag);
        // Save now so the flag, the payment, and the refund record cannot be lost to a crash before logout.
        player->SaveToDB(false, false);

        LOG_INFO("module", "mod-tofu-character-services: {} (guid {}, account {}) bought a {} for {} copper.", player->GetName(), player->GetGUID().GetCounter(), player->GetSession()->GetAccountId(), service.name, service.cost);
        Notify(player, std::string("Your ") + service.name + " is ready. Log out to the character screen to finish it.");
    }

    void CancelService(Player* player)
    {
        CloseGossipMenuFor(player);

        if (!HasPendingChange(player))
        {
            Notify(player, "You have no change waiting.");
            return;
        }

        uint32 const refund = player->GetPlayerSetting(SETTING_SOURCE, SETTING_PAID).value;

        player->RemoveAtLoginFlag(AtLoginFlags(PENDING_MASK), true);
        player->UpdatePlayerSetting(SETTING_SOURCE, SETTING_PAID, 0);
        if (refund)
            player->ModifyMoney(int32(refund));
        player->SaveToDB(false, false);

        LOG_INFO("module", "mod-tofu-character-services: {} (guid {}) cancelled a pending change; refunded {} copper.", player->GetName(), player->GetGUID().GetCounter(), refund);
        Notify(player, refund ? "Your change is cancelled and " + MoneyText(refund) + " was refunded." : std::string("Your change is cancelled."));
    }

    // ---------------------------------------------------------------------------------------------------------
    // Leave combat
    // ---------------------------------------------------------------------------------------------------------

    void LeaveCombat(Player* player)
    {
        CloseGossipMenuFor(player);

        if (!g_Config.enable || !g_Config.leaveCombat)
        {
            Notify(player, "That service is closed.");
            return;
        }
        if (!player->IsInCombat())
        {
            Notify(player, "You are not in combat.");
            return;
        }
        if (player->GetMap()->Instanceable())
        {
            Notify(player, "You cannot leave combat inside a dungeon, raid, battleground, or arena.");
            return;
        }

        for (Unit* attacker : player->getAttackers())
        {
            if (attacker && attacker->IsAlive() && attacker->IsWithinDistInMap(player, g_Config.leaveCombatRange))
            {
                Notify(player, "Something is still attacking you.");
                return;
            }
        }

        player->CombatStopWithPets(true);
        Notify(player, "You are no longer in combat.");
    }

    // ---------------------------------------------------------------------------------------------------------
    // Main menu
    // ---------------------------------------------------------------------------------------------------------

    void AddServiceOption(Player* player, uint32 action, char const* icon, char const* label)
    {
        ServiceInfo const service = GetService(action);
        std::string popup = std::string("Buy a ") + service.name + "? You finish it at the character screen after you log out.";
        if (service.flag == AT_LOGIN_CHANGE_FACTION)
            popup += " Your mailbox must be empty and you must have no auctions or bids.";
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon(icon) + label, SENDER_MAIN, action, popup, service.cost, false);
    }

    void ShowMainMenu(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);

        if (g_Config.enable)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("INV_Scroll_03") + "Arrange my characters", SENDER_MAIN, ACTION_ORDER);

            if (HasPendingChange(player))
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("Spell_ChargeNegative") + "Cancel my pending change", SENDER_MAIN, ACTION_CANCEL, "Cancel your pending change? Anything you paid is refunded.", 0, false);
            }
            else
            {
                AddServiceOption(player, ACTION_RENAME, "INV_Misc_Note_01", "Change my name");
                AddServiceOption(player, ACTION_RACE, "Ability_Rogue_Disguise", "Change my race");
                AddServiceOption(player, ACTION_FACTION, "INV_BannerPVP_01", "Change my faction");
            }

            if (g_Config.leaveCombat && player->IsInCombat())
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("Achievement_BG_AB_defendflags") + "Leave combat", SENDER_MAIN, ACTION_LEAVE_COMBAT);
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Goodbye", SENDER_MAIN, ACTION_GOODBYE);
        SendGossipMenuFor(player, NPC_TEXT_MAIN, creature->GetGUID());
    }
}

class npc_tofu_character_services : public CreatureScript
{
public:
    npc_tofu_character_services() : CreatureScript("npc_tofu_character_services") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!player || !creature)
            return true;

        ShowMainMenu(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (!player || !creature)
            return true;

        if (sender != SENDER_MAIN)
        {
            if (!g_Config.enable || !HandleOrderSelect(player, creature, sender, action))
                CloseGossipMenuFor(player);
            return true;
        }

        switch (action)
        {
            case ACTION_ORDER:
                if (g_Config.enable)
                    ShowCharacterList(player, creature);
                else
                    CloseGossipMenuFor(player);
                break;
            case ACTION_RENAME:
            case ACTION_RACE:
            case ACTION_FACTION:
                BuyService(player, action);
                break;
            case ACTION_CANCEL:
                CancelService(player);
                break;
            case ACTION_LEAVE_COMBAT:
                LeaveCombat(player);
                break;
            case ACTION_BACK_TO_MAIN:
                ShowMainMenu(player, creature);
                break;
            default:
                CloseGossipMenuFor(player);
                break;
        }

        return true;
    }
};

class TofuCharServicesCommandScript : public CommandScript
{
public:
    TofuCharServicesCommandScript() : CommandScript("TofuCharServicesCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "charorder", HandleCharacterOrderCommand, SEC_PLAYER, Console::No }
        };

        return commandTable;
    }

    static bool HandleCharacterOrderCommand(ChatHandler* handler, Optional<Tail>)
    {
        Player* player = handler->GetPlayer();

        if (!player || !player->GetSession())
            return false;

        if (!g_Config.enable || !g_Config.command)
        {
            handler->SendSysMessage("Character ordering by command is turned off. Visit the Archivist instead.");
            return true;
        }

        ShowCharacterList(player, nullptr);
        return true;
    }
};

// Handles the .charorder menu, whose gossip source is the player (the NPC's menu is handled by its CreatureScript).
class TofuCharServicesPlayerScript : public PlayerScript
{
public:
    TofuCharServicesPlayerScript() : PlayerScript("TofuCharServicesPlayerScript", { PLAYERHOOK_ON_GOSSIP_SELECT }) { }

    void OnPlayerGossipSelect(Player* player, uint32 /*menuId*/, uint32 sender, uint32 action) override
    {
        if (!player || !player->GetSession() || !g_Config.enable || !g_Config.command)
            return;

        HandleOrderSelect(player, nullptr, sender, action);
    }
};

class TofuCharServicesWorldScript : public WorldScript
{
public:
    TofuCharServicesWorldScript() : WorldScript("TofuCharServicesWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
        LOG_INFO("module", "mod-tofu-character-services: {} (name {}c, race {}c, faction {}c, leave combat {}, .charorder {}).", g_Config.enable ? "enabled" : "disabled", g_Config.renameCost, g_Config.raceCost, g_Config.factionCost, g_Config.leaveCombat ? "on" : "off", g_Config.command ? "on" : "off");
    }
};

void AddSC_mod_tofu_character_services()
{
    new npc_tofu_character_services();
    new TofuCharServicesCommandScript();
    new TofuCharServicesPlayerScript();
    new TofuCharServicesWorldScript();
}
