# mod-tofu-character-services

An AzerothCore module for Tofu Worldview (mod-playerbots fork of azerothcore-wotlk, 3.3.5a). It adds one NPC, **Archivist Ellery Ashby <Character Services>** (creature 900042), whose menu offers:

1. **Arrange my characters**: reorder the characters on your account. The new order shows on the character screen. Also available anywhere with the player command `.charorder`.
2. **Change my name**: the client asks for a new name at the character screen after you log out.
3. **Change my race**: the client's race change screen (same faction) opens at the character screen after you log out.
4. **Change my faction**: the client's faction change screen opens at the character screen after you log out. Your mailbox must be empty and you must have no auctions or bids.
5. **Leave combat**: shown only while you are in combat. Refused inside dungeons, raids, battlegrounds, and arenas, and while anything alive is attacking you nearby.
6. **Autonomous alts**: your other characters, each marked on or off; turn one on or off, or show its status (`.autoalt start|stop|status <char>`, `.autoalt list`); also a read-only list of every autonomous alt on the realm with its account name.
7. **Alt parties**: every party with its member count; create, join and lead, add one of your alts, remove a member, disband, or list (`.autoalt party ...`).
8. **Toggle AoE loot: ON/OFF** (`.aoeloot on|off`): one option that shows the current state, read from the player setting `mod-aoe-loot` index 0 kept by the Tofu mod-aoe-loot fork (the choice survives relogs and restarts). The new state is shown on screen.
9. **My XP rate**: show my XP rate, set a custom XP rate from 1 to 20, reset back to 1 (`.xp view|set|default`). Turning XP gain on or off is left to the Experience Eliminator NPCs.
10. **Toggle Realm First achievement eligibility: ON/OFF** (`.realmfirst optout on|off`, Tofu fork of mod-realm-first-titles with `RealmFirstTitles.AllowPlayerOptOut = 1`); the new state is shown on screen in yellow. Off keeps the Realm Firsts the character already holds and leaves new ones open for others; turning it back on counts only new progress.

Sections 6 to 10 each have their own settings key and are off by default.

While a name, race, or faction change is waiting, the menu shows **Cancel my pending change** instead, which refunds what you paid. Bots cannot use the services.

## How it works

- Character order writes `acore_characters.characters.order`, which the core's character list query already sorts by.
- Name, race, and faction change set the core's at-login flags (`AT_LOGIN_RENAME`, `AT_LOGIN_CHANGE_RACE`, `AT_LOGIN_CHANGE_FACTION`), the same flags as `.character rename`, `.character changerace`, and `.character changefaction`. The core's own character screen handlers then do the conversion (items, spells, reputations, titles, and quests through the stock `player_factionchange_*` tables).
- What was paid for the pending change is kept in the player setting `mod-tofu-character-services` index 0 (`character_settings`, needs `EnablePlayerSettings = 1`).
- Bots are recognized by their headless session, so the module does not depend on mod-playerbots.
- Sections 6 to 10 run the real player command for you through `ChatHandler::ParseCommands`, the same path as typing it, so each module's checks, permissions, challenge locks, and replies apply unchanged, and this module does not link against those modules. Typed input (a party name, an XP rate) comes from the dialog's text box and is validated before it goes into a command. The menus read state with SELECTs only: `character_settings` source `mod_autonomous_altbots` index 0, `tofu_autoalt_party`, `tofu_autoalt_party_member`, and the player settings `mod-realm-first-titles` index 0 and `mod-aoe-loot` index 0. The Autonomous alts menu also has a read-only list of every autonomous alt on the realm (any account) with its level, class, whether it is in the world, and its account name; it cannot change anything.
- If one of those modules renames a command or subcommand, its menu here must change the same way.

## Install

1. Clone into the deploy set as `mod-tofu-character-services` (the folder name sets the loader name `Addmod_tofu_character_servicesScripts`).
2. Re-run CMake and build worldserver.
3. Copy `conf/mod_tofu_character_services.conf.dist` to `configs/modules/mod_tofu_character_services.conf` and set `TofuCharServices.Enable = 1`.
4. Start the worldserver (the updater applies `data/sql/db-world/updates/*.sql`), then `.npc add 900042`.

Removing it: take the folder out of the build, rebuild, then run `sql-revert/db-world/revert_tofu_character_services_2026_11_22_00_archivist.sql` by hand. Saved character orders stay; the core keeps honoring them.

## Notes

- With `.charorder` on, `.char` stops working as an abbreviation of `.character`. Type `.chara` or the full name.
- The menu icons are stock 3.3.5a icon names, and the class colors are darkened to stay readable on the parchment.

## Credits

- Character ordering is ported from [Souicia/mod-character-order](https://github.com/Souicia/mod-character-order) (commit 2270973, MIT; see `NOTICE`).
- Menu style (inline icons on each option) after [azerothcore/mod-npc-services](https://github.com/azerothcore/mod-npc-services); no code is copied from it.
