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
 * - Menus for other modules' player commands: Autonomous alts and Alt parties (.autoalt, mod-autonomous-altbots),
 *   AoE loot (.aoeloot, mod-aoe-loot), XP rate (.xp, mod-individual-xp), and the per-character Realm First opt-out
 *   (.realmfirst optout, Tofu fork of mod-realm-first-titles). Each option runs the real command for
 *   the player through ChatHandler::ParseCommands, exactly as if it were typed, so every check, permission, lock, and
 *   reply of those modules applies unchanged and this module does not link against them. Typed input (a party name,
 *   an XP rate) comes from the gossip text box and is validated before it is put into a command. State shown in the
 *   menus (which characters are autonomous alts, which parties exist) is read with SELECTs only, and the AoE loot and
 *   Realm First toggles read the player settings those modules keep (mod-aoe-loot needs the Tofu fork for that).
 * Menu style after azerothcore/mod-npc-services (inline icons on each option); no code is copied from it.
 *
 * Bots never get these services: every playerbot session is headless (no socket), so WorldSession::IsHeadless()
 * identifies them without depending on mod-playerbots.
 */

#include "AccountMgr.h"
#include "AuctionHouseMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "WorldSession.h"

#include <algorithm>
#include <map>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // npc_text rows from this module's SQL (registry: npc_text 900018 to 900019 and 900064 to 900066).
    constexpr uint32 NPC_TEXT_MAIN = 900018;
    constexpr uint32 NPC_TEXT_ORDER = 900019;
    constexpr uint32 NPC_TEXT_ALTS = 900064;
    constexpr uint32 NPC_TEXT_PARTIES = 900065;
    constexpr uint32 NPC_TEXT_XP = 900066;

    // mod-autonomous-altbots stores each character's on/off choice in this player setting (read only here).
    constexpr char AUTOALT_SETTING_SOURCE[] = "mod_autonomous_altbots";

    // The Tofu realm-first fork stores the character's opt-out in this player setting (1 = opted out; read only here).
    constexpr char REALM_FIRST_SETTING_SOURCE[] = "mod-realm-first-titles";
    constexpr uint8 REALM_FIRST_SETTING_INDEX = 0;
    // The Tofu mod-aoe-loot fork stores the character's .aoeloot choice in this player setting (1 = off; read only here).
    constexpr char AOE_LOOT_SETTING_SOURCE[] = "mod-aoe-loot";
    constexpr uint8 AOE_LOOT_SETTING_INDEX = 0;
    constexpr std::size_t PARTY_NAME_MAX_LENGTH = 32;
    constexpr std::size_t MAX_LIST_OPTIONS = 30;

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
    constexpr uint32 ACTION_ALTS = 9;
    constexpr uint32 ACTION_PARTIES = 10;
    constexpr uint32 ACTION_AOE_ON = 11;
    constexpr uint32 ACTION_AOE_OFF = 12;
    constexpr uint32 ACTION_XP = 13;
    constexpr uint32 ACTION_REALM_FIRST_OFF = 14;
    constexpr uint32 ACTION_REALM_FIRST_ON = 15;

    // Autonomous alts (action = character GUID unless noted).
    constexpr uint32 SENDER_ALT_SELECT = 0x54430010;
    constexpr uint32 SENDER_ALT_ON = 0x54430011;
    constexpr uint32 SENDER_ALT_OFF = 0x54430012;
    constexpr uint32 SENDER_ALT_STATUS = 0x54430013;
    constexpr uint32 SENDER_ALT_LIST = 0x54430014;      // action 0: .autoalt list
    constexpr uint32 SENDER_ALT_GLOBAL = 0x54430015;    // read-only list of every autonomous alt on the realm

    // Alt parties (action = party id unless noted).
    constexpr uint32 SENDER_PARTY_SELECT = 0x54430020;
    constexpr uint32 SENDER_PARTY_CREATE = 0x54430021;  // coded: the typed name
    constexpr uint32 SENDER_PARTY_JOIN = 0x54430022;
    constexpr uint32 SENDER_PARTY_ADD_MENU = 0x54430023;
    constexpr uint32 SENDER_PARTY_ADD = 0x54430024;     // action = character GUID; party from the open add menu
    constexpr uint32 SENDER_PARTY_REMOVE_MENU = 0x54430025;
    constexpr uint32 SENDER_PARTY_REMOVE = 0x54430026;  // action = character GUID
    constexpr uint32 SENDER_PARTY_DISBAND = 0x54430027;
    constexpr uint32 SENDER_PARTY_LIST = 0x54430028;    // action 0: every party; otherwise that party
    constexpr uint32 SENDER_PARTY_BACK = 0x54430029;    // action 0: back to the party list

    // XP rate.
    constexpr uint32 SENDER_XP = 0x54430030;
    constexpr uint32 SENDER_XP_CUSTOM = 0x54430032;     // coded: the typed rate
    constexpr uint32 XP_VIEW = 1;
    constexpr uint32 XP_DEFAULT = 2;
    constexpr float XP_RATE_MIN = 1.0f;
    constexpr float XP_RATE_MAX = 20.0f;

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
        bool autoAlt = false;
        bool autoAltParties = false;
        bool aoeLoot = false;
        bool xp = false;
        bool realmFirst = false;
    };

    ModuleConfig g_Config;

    // Party whose "Add a member" menu each player has open (player GUID counter -> party id). Gossip runs on map
    // update threads, so access is locked.
    std::mutex g_AddMenuMutex;
    std::unordered_map<uint32, uint32> g_AddMenuParty;

    void LoadConfig()
    {
        g_Config.enable = sConfigMgr->GetOption<bool>("TofuCharServices.Enable", false);
        g_Config.command = sConfigMgr->GetOption<bool>("TofuCharServices.OrderCommand", true);
        g_Config.renameCost = sConfigMgr->GetOption<uint32>("TofuCharServices.RenameCost", 0);
        g_Config.raceCost = sConfigMgr->GetOption<uint32>("TofuCharServices.RaceChangeCost", 0);
        g_Config.factionCost = sConfigMgr->GetOption<uint32>("TofuCharServices.FactionChangeCost", 0);
        g_Config.leaveCombat = sConfigMgr->GetOption<bool>("TofuCharServices.LeaveCombat.Enable", true);
        g_Config.leaveCombatRange = sConfigMgr->GetOption<float>("TofuCharServices.LeaveCombat.Range", 50.0f);
        g_Config.autoAlt = sConfigMgr->GetOption<bool>("TofuCharServices.AutoAlt.Enable", false);
        g_Config.autoAltParties = sConfigMgr->GetOption<bool>("TofuCharServices.AutoAlt.Parties", false);
        g_Config.aoeLoot = sConfigMgr->GetOption<bool>("TofuCharServices.AoeLoot.Enable", false);
        g_Config.xp = sConfigMgr->GetOption<bool>("TofuCharServices.Xp.Enable", false);
        g_Config.realmFirst = sConfigMgr->GetOption<bool>("TofuCharServices.RealmFirst.Enable", false);
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
    // Other modules' player commands
    // ---------------------------------------------------------------------------------------------------------

    // AoE loot is on unless the character turned it off (mod-aoe-loot's own default with no row).
    bool AoeLootOn(Player* player)
    {
        return player->GetPlayerSetting(AOE_LOOT_SETTING_SOURCE, AOE_LOOT_SETTING_INDEX).value == 0;
    }

    // Eligible for Realm Firsts unless the character opted out.
    bool RealmFirstEligible(Player* player)
    {
        return player->GetPlayerSetting(REALM_FIRST_SETTING_SOURCE, REALM_FIRST_SETTING_INDEX).value == 0;
    }

    // Runs a player command exactly as if the player had typed it (".xp view" and so on). The command's own
    // module checks permissions and replies in the player's chat.
    void RunCommand(Player* player, std::string const& command)
    {
        CloseGossipMenuFor(player);
        ChatHandler handler(player->GetSession());
        if (!handler.ParseCommands(command))
            Notify(player, "That command is not available on this server right now.");
    }

    bool IsValidPartyName(std::string const& name)
    {
        if (name.empty() || name.size() > PARTY_NAME_MAX_LENGTH)
            return false;
        return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
    }

    // Character names are stored as letters only; anything else is not put into a command.
    bool IsSafeCharacterName(std::string const& name)
    {
        return !name.empty() && name.size() <= 12 && std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 0x80 || std::isalpha(c); });
    }

    // Character settings are stored as space-separated values; returns the value at index, or 0.
    uint32 SettingValue(std::string const& data, uint32 index)
    {
        std::istringstream stream(data);
        std::string token;
        for (uint32 i = 0; stream >> token; ++i)
            if (i == index)
                return uint32(std::strtoul(token.c_str(), nullptr, 10));
        return 0;
    }

    struct AltEntry
    {
        uint32 Guid;
        std::string Name;
        uint8 Level;
        uint8 Class;
        bool Enabled;
        uint32 PartyId;
    };

    // The account's characters except the one the player is on, with their autonomous alt state and party.
    std::vector<AltEntry> LoadAccountAlts(Player* player, bool withParty)
    {
        std::vector<AltEntry> alts;
        std::string const partyColumn = withParty ? "COALESCE(m.party_id, 0)" : "0";
        std::string const partyJoin = withParty ? "LEFT JOIN tofu_autoalt_party_member m ON m.guid = c.guid " : "";

        QueryResult result = CharacterDatabase.Query(
            "SELECT c.guid, c.name, c.level, c.class, COALESCE(s.data, ''), " + partyColumn + " "
            "FROM characters c "
            "LEFT JOIN character_settings s ON s.guid = c.guid AND s.source = '" + std::string(AUTOALT_SETTING_SOURCE) + "' " +
            partyJoin +
            "WHERE c.account = {} AND c.guid <> {} AND c.deleteInfos_Name IS NULL "
            "ORDER BY COALESCE(c.`order`, c.guid), c.guid",
            player->GetSession()->GetAccountId(), player->GetGUID().GetCounter());

        if (!result)
            return alts;

        do
        {
            Field* fields = result->Fetch();
            AltEntry entry;
            entry.Guid = fields[0].Get<uint32>();
            entry.Name = fields[1].Get<std::string>();
            entry.Level = fields[2].Get<uint8>();
            entry.Class = fields[3].Get<uint8>();
            entry.Enabled = SettingValue(fields[4].Get<std::string>(), 0) != 0;
            entry.PartyId = fields[5].Get<uint32>();
            alts.push_back(entry);
        }
        while (result->NextRow() && alts.size() < MAX_LIST_OPTIONS);

        return alts;
    }

    bool FindAccountAlt(Player* player, uint32 guid, AltEntry& found)
    {
        for (AltEntry const& entry : LoadAccountAlts(player, false))
        {
            if (entry.Guid == guid)
            {
                found = entry;
                return true;
            }
        }
        return false;
    }

    std::string AltLabel(AltEntry const& entry)
    {
        return "|cff" + std::string(GetClassColor(entry.Class)) + entry.Name + "|r - Level " + std::to_string(entry.Level) +
            " " + GetClassName(entry.Class) + (entry.Enabled ? " - |cff1E7A1Eon|r" : " - |cff7A1E1Eoff|r");
    }

    void ShowAltList(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);

        std::vector<AltEntry> const alts = LoadAccountAlts(player, false);
        if (alts.empty())
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "You have no other characters on this account.", SENDER_MAIN, ACTION_BACK_TO_MAIN);

        for (AltEntry const& entry : alts)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, AltLabel(entry), SENDER_ALT_SELECT, entry.Guid);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("INV_Scroll_03") + "Show all my alts in chat", SENDER_ALT_LIST, 0);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("INV_Misc_Book_09") + "Show every autonomous alt on the realm", SENDER_ALT_GLOBAL, 0);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to character services", SENDER_MAIN, ACTION_BACK_TO_MAIN);
        SendGossipMenuFor(player, NPC_TEXT_ALTS, creature->GetGUID());
    }

    // Read-only: every character on any account whose autonomous alt setting is on (the set .autoalt list shows a GM),
    // with whether it is in the world now and the account it belongs to. Nothing here can change an alt.
    void ShowGlobalAltList(Player* player, Creature* creature)
    {
        struct GlobalAlt
        {
            uint32 Guid;
            std::string Name;
            uint8 Level;
            uint8 Class;
            uint32 Account;
        };

        std::vector<GlobalAlt> alts;
        if (QueryResult result = CharacterDatabase.Query(
            "SELECT c.guid, c.name, c.level, c.class, c.account, s.data FROM character_settings s "
            "JOIN characters c ON c.guid = s.guid "
            "WHERE s.source = '" + std::string(AUTOALT_SETTING_SOURCE) + "' AND c.deleteInfos_Name IS NULL "
            "ORDER BY c.name"))
        {
            do
            {
                Field* fields = result->Fetch();
                if (SettingValue(fields[5].Get<std::string>(), 0) == 0)
                    continue;
                alts.push_back({ fields[0].Get<uint32>(), fields[1].Get<std::string>(), fields[2].Get<uint8>(), fields[3].Get<uint8>(), fields[4].Get<uint32>() });
            }
            while (result->NextRow());
        }

        ClearGossipMenuFor(player);
        uint32 online = 0;
        for (GlobalAlt const& alt : alts)
            if (Player* bot = ObjectAccessor::FindPlayerByLowGUID(alt.Guid))
                if (bot->IsInWorld())
                    ++online;

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Autonomous alts on the realm: " + std::to_string(alts.size()) + " on, " + std::to_string(online) + " in the world now", SENDER_ALT_GLOBAL, 0);

        // The client shows at most 32 options: the header, up to MAX_LIST_OPTIONS - 2 alts, an overflow line, and Back.
        std::size_t const shownMax = MAX_LIST_OPTIONS - 2;
        std::map<uint32, std::string> accountNames;
        for (std::size_t i = 0; i < alts.size() && i < shownMax; ++i)
        {
            GlobalAlt const& alt = alts[i];
            auto it = accountNames.find(alt.Account);
            if (it == accountNames.end())
            {
                std::string name;
                if (!AccountMgr::GetName(alt.Account, name))
                    name = "unknown account";
                it = accountNames.emplace(alt.Account, name).first;
            }

            Player* bot = ObjectAccessor::FindPlayerByLowGUID(alt.Guid);
            bool const inWorld = bot && bot->IsInWorld();
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|cff" + std::string(GetClassColor(alt.Class)) + alt.Name + "|r - Level " + std::to_string(alt.Level) + " " + GetClassName(alt.Class) + (inWorld ? " - |cff1E7A1Eonline|r" : " - |cff7A1E1Eoffline|r") + " - " + it->second, SENDER_ALT_GLOBAL, 0);
        }
        if (alts.size() > shownMax)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "...and " + std::to_string(alts.size() - shownMax) + " more", SENDER_ALT_GLOBAL, 0);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to my alts", SENDER_MAIN, ACTION_ALTS);
        SendGossipMenuFor(player, NPC_TEXT_MAIN, creature->GetGUID());
    }

    void ShowAltPage(Player* player, Creature* creature, uint32 guid)
    {
        AltEntry entry;
        if (!FindAccountAlt(player, guid, entry))
        {
            ShowAltList(player, creature);
            return;
        }

        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, AltLabel(entry), SENDER_ALT_SELECT, entry.Guid);
        if (entry.Enabled)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("Spell_ChargeNegative") + "Turn off: log it out and keep it offline", SENDER_ALT_OFF, entry.Guid);
        else
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("Spell_ChargePositive") + "Turn on: let it play on its own", SENDER_ALT_ON, entry.Guid);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("INV_Misc_Spyglass_02") + "Show its status in chat", SENDER_ALT_STATUS, entry.Guid);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to my alts", SENDER_MAIN, ACTION_ALTS);
        SendGossipMenuFor(player, NPC_TEXT_ALTS, creature->GetGUID());
    }

    struct PartyEntry
    {
        uint32 Id;
        std::string Name;
        uint32 Members;
    };

    std::vector<PartyEntry> LoadParties()
    {
        std::vector<PartyEntry> parties;
        QueryResult result = CharacterDatabase.Query(
            "SELECT p.id, p.name, COUNT(m.guid) FROM tofu_autoalt_party p "
            "LEFT JOIN tofu_autoalt_party_member m ON m.party_id = p.id GROUP BY p.id, p.name ORDER BY p.name");

        if (!result)
            return parties;

        do
        {
            Field* fields = result->Fetch();
            PartyEntry entry;
            entry.Id = fields[0].Get<uint32>();
            entry.Name = fields[1].Get<std::string>();
            entry.Members = uint32(fields[2].Get<uint64>());
            if (IsValidPartyName(entry.Name))
                parties.push_back(entry);
        }
        while (result->NextRow() && parties.size() < MAX_LIST_OPTIONS);

        return parties;
    }

    bool FindParty(uint32 id, PartyEntry& found)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT p.id, p.name, (SELECT COUNT(*) FROM tofu_autoalt_party_member m WHERE m.party_id = p.id) FROM tofu_autoalt_party p WHERE p.id = {}", id);
        if (!result)
            return false;

        Field* fields = result->Fetch();
        found.Id = fields[0].Get<uint32>();
        found.Name = fields[1].Get<std::string>();
        found.Members = uint32(fields[2].Get<uint64>());
        return IsValidPartyName(found.Name);
    }

    void ShowPartyList(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);

        for (PartyEntry const& party : LoadParties())
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("INV_Misc_GroupNeedMore") + party.Name + " (" + std::to_string(party.Members) + " of 5)", SENDER_PARTY_SELECT, party.Id);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("Spell_ChargePositive") + "Create a party...", SENDER_PARTY_CREATE, 0, "Type a name for the new party: 1 to 32 letters, digits, - or _.", 0, true);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("INV_Scroll_03") + "Show all parties in chat", SENDER_PARTY_LIST, 0);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to character services", SENDER_MAIN, ACTION_BACK_TO_MAIN);
        SendGossipMenuFor(player, NPC_TEXT_PARTIES, creature->GetGUID());
    }

    void ShowPartyPage(Player* player, Creature* creature, uint32 id)
    {
        PartyEntry party;
        if (!FindParty(id, party))
        {
            Notify(player, "That party no longer exists.");
            ShowPartyList(player, creature);
            return;
        }

        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, party.Name + " - " + std::to_string(party.Members) + " of 5 members", SENDER_PARTY_SELECT, party.Id);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("Ability_Warrior_BattleShout") + "Join and lead this party", SENDER_PARTY_JOIN, party.Id);
        if (party.Members < 5)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("Spell_ChargePositive") + "Add one of my alts", SENDER_PARTY_ADD_MENU, party.Id);
        if (party.Members > 0)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("Spell_ChargeNegative") + "Remove a member", SENDER_PARTY_REMOVE_MENU, party.Id);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("INV_Scroll_03") + "Show its members in chat", SENDER_PARTY_LIST, party.Id);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("Ability_Rogue_FeignDeath") + "Disband this party", SENDER_PARTY_DISBAND, party.Id, "Disband " + party.Name + "? Its members stay autonomous alts.", 0, false);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to all parties", SENDER_PARTY_BACK, 0);
        SendGossipMenuFor(player, NPC_TEXT_PARTIES, creature->GetGUID());
    }

    void ShowPartyAddMenu(Player* player, Creature* creature, uint32 id)
    {
        PartyEntry party;
        if (!FindParty(id, party))
        {
            ShowPartyList(player, creature);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_AddMenuMutex);
            g_AddMenuParty[player->GetGUID().GetCounter()] = id;
        }

        ClearGossipMenuFor(player);
        bool any = false;
        for (AltEntry const& entry : LoadAccountAlts(player, true))
        {
            if (!entry.Enabled || entry.PartyId)
                continue;
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, AltLabel(entry), SENDER_PARTY_ADD, entry.Guid);
            any = true;
        }
        if (!any)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "None of your alts can join: turn one on first, and it must not be in a party.", SENDER_PARTY_SELECT, id);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to " + party.Name, SENDER_PARTY_SELECT, id);
        SendGossipMenuFor(player, NPC_TEXT_PARTIES, creature->GetGUID());
    }

    void ShowPartyRemoveMenu(Player* player, Creature* creature, uint32 id)
    {
        PartyEntry party;
        if (!FindParty(id, party))
        {
            ShowPartyList(player, creature);
            return;
        }

        ClearGossipMenuFor(player);
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT c.guid, c.name, c.level, c.class FROM tofu_autoalt_party_member m JOIN characters c ON c.guid = m.guid WHERE m.party_id = {} ORDER BY c.name", id))
        {
            do
            {
                Field* fields = result->Fetch();
                std::string const name = fields[1].Get<std::string>();
                uint8 const cls = fields[3].Get<uint8>();
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|cff" + std::string(GetClassColor(cls)) + name + "|r - Level " + std::to_string(fields[2].Get<uint8>()) + " " + GetClassName(cls), SENDER_PARTY_REMOVE, fields[0].Get<uint32>(), "Take " + name + " out of " + party.Name + "?", 0, false);
            }
            while (result->NextRow());
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to " + party.Name, SENDER_PARTY_SELECT, id);
        SendGossipMenuFor(player, NPC_TEXT_PARTIES, creature->GetGUID());
    }

    std::string CharacterNameByGuid(uint32 guid)
    {
        std::string name;
        if (!sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(guid), name) || !IsSafeCharacterName(name))
            return "";
        return name;
    }

    void ShowXpMenu(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("INV_Misc_Spyglass_02") + "Show my XP rate", SENDER_XP, XP_VIEW);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("INV_Misc_Note_01") + "Set a custom XP rate (1-20)...", SENDER_XP_CUSTOM, 0, "Type your XP rate from 1 to 20, for example 4 or 2.5.", 0, true);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("Spell_Holy_Renew") + "Reset my XP rate back to 1", SENDER_XP, XP_DEFAULT);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to character services", SENDER_MAIN, ACTION_BACK_TO_MAIN);
        SendGossipMenuFor(player, NPC_TEXT_XP, creature->GetGUID());
    }

    // Digits with at most one decimal point.
    bool IsRateText(std::string const& text)
    {
        bool dot = false;
        bool digit = false;
        for (unsigned char c : text)
        {
            if (c == '.')
            {
                if (dot)
                    return false;
                dot = true;
            }
            else if (std::isdigit(c))
                digit = true;
            else
                return false;
        }
        return digit;
    }

    // A typed XP rate: digits with at most one decimal point, at most 6 characters, from XP_RATE_MIN to XP_RATE_MAX.
    bool IsValidRate(std::string const& text)
    {
        if (text.empty() || text.size() > 6)
            return false;
        if (!IsRateText(text))
            return false;
        float const rate = std::strtof(text.c_str(), nullptr);
        return rate >= XP_RATE_MIN && rate <= XP_RATE_MAX;
    }

    std::string Trim(char const* code)
    {
        std::string text = code ? code : "";
        auto const notSpace = [](unsigned char c) { return !std::isspace(c); };
        text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
        text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
        return text;
    }

    // Returns true when the sender belongs to one of the command menus.
    bool HandleCommandSelect(Player* player, Creature* creature, uint32 sender, uint32 action)
    {
        switch (sender)
        {
            case SENDER_ALT_SELECT:
            case SENDER_ALT_ON:
            case SENDER_ALT_OFF:
            case SENDER_ALT_STATUS:
            case SENDER_ALT_LIST:
            case SENDER_ALT_GLOBAL:
            {
                if (!g_Config.autoAlt)
                {
                    CloseGossipMenuFor(player);
                    return true;
                }
                if (sender == SENDER_ALT_GLOBAL)
                {
                    ShowGlobalAltList(player, creature);
                    return true;
                }
                if (sender == SENDER_ALT_LIST)
                {
                    RunCommand(player, ".autoalt list");
                    return true;
                }
                if (sender == SENDER_ALT_SELECT)
                {
                    ShowAltPage(player, creature, action);
                    return true;
                }
                AltEntry entry;
                if (!FindAccountAlt(player, action, entry) || !IsSafeCharacterName(entry.Name))
                {
                    ShowAltList(player, creature);
                    return true;
                }
                char const* sub = sender == SENDER_ALT_ON ? "start" : sender == SENDER_ALT_OFF ? "stop" : "status";
                RunCommand(player, std::string(".autoalt ") + sub + " " + entry.Name);
                return true;
            }

            case SENDER_PARTY_SELECT:
            case SENDER_PARTY_JOIN:
            case SENDER_PARTY_ADD_MENU:
            case SENDER_PARTY_ADD:
            case SENDER_PARTY_REMOVE_MENU:
            case SENDER_PARTY_REMOVE:
            case SENDER_PARTY_DISBAND:
            case SENDER_PARTY_LIST:
            case SENDER_PARTY_BACK:
            {
                if (!g_Config.autoAlt || !g_Config.autoAltParties)
                {
                    CloseGossipMenuFor(player);
                    return true;
                }

                PartyEntry party;
                switch (sender)
                {
                    case SENDER_PARTY_SELECT:
                        ShowPartyPage(player, creature, action);
                        break;
                    case SENDER_PARTY_BACK:
                        ShowPartyList(player, creature);
                        break;
                    case SENDER_PARTY_ADD_MENU:
                        ShowPartyAddMenu(player, creature, action);
                        break;
                    case SENDER_PARTY_REMOVE_MENU:
                        ShowPartyRemoveMenu(player, creature, action);
                        break;
                    case SENDER_PARTY_LIST:
                        if (!action)
                            RunCommand(player, ".autoalt party list");
                        else if (FindParty(action, party))
                            RunCommand(player, ".autoalt party list " + party.Name);
                        else
                            ShowPartyList(player, creature);
                        break;
                    case SENDER_PARTY_JOIN:
                    case SENDER_PARTY_DISBAND:
                        if (FindParty(action, party))
                            RunCommand(player, std::string(".autoalt party ") + (sender == SENDER_PARTY_JOIN ? "join " : "disband ") + party.Name);
                        else
                            ShowPartyList(player, creature);
                        break;
                    case SENDER_PARTY_ADD:
                    {
                        uint32 partyId = 0;
                        {
                            std::lock_guard<std::mutex> lock(g_AddMenuMutex);
                            auto it = g_AddMenuParty.find(player->GetGUID().GetCounter());
                            if (it != g_AddMenuParty.end())
                            {
                                partyId = it->second;
                                g_AddMenuParty.erase(it);
                            }
                        }
                        AltEntry entry;
                        if (partyId && FindParty(partyId, party) && FindAccountAlt(player, action, entry) && IsSafeCharacterName(entry.Name))
                            RunCommand(player, ".autoalt party add " + party.Name + " " + entry.Name);
                        else
                            ShowPartyList(player, creature);
                        break;
                    }
                    case SENDER_PARTY_REMOVE:
                    {
                        std::string const name = CharacterNameByGuid(action);
                        if (!name.empty())
                            RunCommand(player, ".autoalt party remove " + name);
                        else
                            ShowPartyList(player, creature);
                        break;
                    }
                    default:
                        break;
                }
                return true;
            }

            case SENDER_XP:
            {
                if (!g_Config.xp)
                {
                    CloseGossipMenuFor(player);
                    return true;
                }
                switch (action)
                {
                    case XP_VIEW:    RunCommand(player, ".xp view"); break;
                    case XP_DEFAULT: RunCommand(player, ".xp default"); break;
                    default:         ShowXpMenu(player, creature); break;
                }
                return true;
            }

            default:
                return false;
        }
    }

    // Options with a text box (party name, custom XP rate).
    void HandleCodedSelect(Player* player, Creature* creature, uint32 sender, char const* code)
    {
        std::string const text = Trim(code);

        if (sender == SENDER_PARTY_CREATE && g_Config.autoAlt && g_Config.autoAltParties)
        {
            if (!IsValidPartyName(text))
            {
                Notify(player, "A party name is 1 to 32 letters, digits, - or _.");
                ShowPartyList(player, creature);
                return;
            }
            RunCommand(player, ".autoalt party create " + text);
            return;
        }

        if (sender == SENDER_XP_CUSTOM && g_Config.xp)
        {
            if (!IsValidRate(text))
            {
                Notify(player, "Type a number from 1 to 20, for example 4 or 2.5.");
                ShowXpMenu(player, creature);
                return;
            }
            RunCommand(player, ".xp set " + text);
            return;
        }

        CloseGossipMenuFor(player);
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

            if (g_Config.autoAlt)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("Spell_Holy_PrayerOfSpirit") + "Autonomous alts", SENDER_MAIN, ACTION_ALTS);
            if (g_Config.autoAlt && g_Config.autoAltParties)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("INV_Misc_GroupNeedMore") + "Alt parties", SENDER_MAIN, ACTION_PARTIES);
            if (g_Config.aoeLoot)
            {
                if (AoeLootOn(player))
                    AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("INV_Misc_Bag_10") + "Toggle AoE loot: |cff1E7A1EON|r", SENDER_MAIN, ACTION_AOE_OFF);
                else
                    AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("INV_Misc_Bag_10_Black") + "Toggle AoE loot: |cff7A1E1EOFF|r", SENDER_MAIN, ACTION_AOE_ON);
            }
            if (g_Config.xp)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("Spell_Holy_SurgeOfLight") + "My XP rate", SENDER_MAIN, ACTION_XP);

            if (g_Config.realmFirst)
            {
                if (!RealmFirstEligible(player))
                    AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("Achievement_General") + "Toggle Realm First achievement eligibility: |cff7A1E1EOFF|r", SENDER_MAIN, ACTION_REALM_FIRST_ON, "Let this character earn Realm First achievements again? Only new progress counts: a Realm First it would have earned while they were off is not granted.", 0, false);
                else
                    AddGossipItemFor(player, GOSSIP_ICON_CHAT, Icon("Achievement_General") + "Toggle Realm First achievement eligibility: |cff1E7A1EON|r", SENDER_MAIN, ACTION_REALM_FIRST_OFF, "Stop this character from earning Realm First achievements, so they stay open for others? Realm Firsts it already holds are kept.", 0, false);
            }
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

        // Bots never get a menu.
        if (player->GetSession()->IsHeadless())
        {
            CloseGossipMenuFor(player);
            return true;
        }

        ShowMainMenu(player, creature);
        return true;
    }

    bool OnGossipSelectCode(Player* player, Creature* creature, uint32 sender, uint32 /*action*/, char const* code) override
    {
        if (!player || !creature)
            return true;

        if (!g_Config.enable || player->GetSession()->IsHeadless())
        {
            CloseGossipMenuFor(player);
            return true;
        }

        HandleCodedSelect(player, creature, sender, code);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (!player || !creature)
            return true;

        if (player->GetSession()->IsHeadless())
        {
            CloseGossipMenuFor(player);
            return true;
        }

        if (sender != SENDER_MAIN)
        {
            if (!g_Config.enable || (!HandleOrderSelect(player, creature, sender, action) && !HandleCommandSelect(player, creature, sender, action)))
                CloseGossipMenuFor(player);
            return true;
        }

        if (!g_Config.enable && action != ACTION_GOODBYE)
        {
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
            case ACTION_ALTS:
                if (g_Config.autoAlt)
                    ShowAltList(player, creature);
                else
                    CloseGossipMenuFor(player);
                break;
            case ACTION_PARTIES:
                if (g_Config.autoAlt && g_Config.autoAltParties)
                    ShowPartyList(player, creature);
                else
                    CloseGossipMenuFor(player);
                break;
            case ACTION_AOE_ON:
            case ACTION_AOE_OFF:
                if (g_Config.aoeLoot)
                {
                    RunCommand(player, action == ACTION_AOE_ON ? ".aoeloot on" : ".aoeloot off");
                    // Reports the state the command actually left, not the one asked for.
                    ChatHandler(player->GetSession()).SendNotification(std::string("AoE loot: ") + (AoeLootOn(player) ? "ON" : "OFF"));
                }
                else
                    CloseGossipMenuFor(player);
                break;
            case ACTION_XP:
                if (g_Config.xp)
                    ShowXpMenu(player, creature);
                else
                    CloseGossipMenuFor(player);
                break;
            case ACTION_REALM_FIRST_OFF:
            case ACTION_REALM_FIRST_ON:
                if (g_Config.realmFirst)
                {
                    RunCommand(player, action == ACTION_REALM_FIRST_OFF ? ".realmfirst optout on" : ".realmfirst optout off");
                    // Yellow on-screen message with the state the command actually left (it refuses when opt-out is off).
                    ChatHandler(player->GetSession()).SendNotification(std::string("Realm First achievement eligibility: ") + (RealmFirstEligible(player) ? "ON" : "OFF"));
                }
                else
                    CloseGossipMenuFor(player);
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
        LOG_INFO("module", "mod-tofu-character-services: {} (name {}c, race {}c, faction {}c, leave combat {}, .charorder {}, autoalt {}, parties {}, aoeloot {}, xp {}, realm first {}).", g_Config.enable ? "enabled" : "disabled", g_Config.renameCost, g_Config.raceCost, g_Config.factionCost, g_Config.leaveCombat ? "on" : "off", g_Config.command ? "on" : "off", g_Config.autoAlt ? "on" : "off", g_Config.autoAltParties ? "on" : "off", g_Config.aoeLoot ? "on" : "off", g_Config.xp ? "on" : "off", g_Config.realmFirst ? "on" : "off");
    }
};

void AddSC_mod_tofu_character_services()
{
    new npc_tofu_character_services();
    new TofuCharServicesCommandScript();
    new TofuCharServicesPlayerScript();
    new TofuCharServicesWorldScript();
}
