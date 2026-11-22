# mod-tofu-character-services

An AzerothCore module for Tofu Worldview (mod-playerbots fork of azerothcore-wotlk, 3.3.5a). It adds one NPC, **Archivist Ellery Ashby <Character Services>** (creature 900042), whose menu offers:

1. **Arrange my characters**: reorder the characters on your account. The new order shows on the character screen. Also available anywhere with the player command `.charorder`.
2. **Change my name**: the client asks for a new name at the character screen after you log out.
3. **Change my race**: the client's race change screen (same faction) opens at the character screen after you log out.
4. **Change my faction**: the client's faction change screen opens at the character screen after you log out. Your mailbox must be empty and you must have no auctions or bids.
5. **Leave combat**: shown only while you are in combat. Refused inside dungeons, raids, battlegrounds, and arenas, and while anything alive is attacking you nearby.

While a name, race, or faction change is waiting, the menu shows **Cancel my pending change** instead, which refunds what you paid. Bots cannot use the services.

## How it works

- Character order writes `acore_characters.characters.order`, which the core's character list query already sorts by.
- Name, race, and faction change set the core's at-login flags (`AT_LOGIN_RENAME`, `AT_LOGIN_CHANGE_RACE`, `AT_LOGIN_CHANGE_FACTION`), the same flags as `.character rename`, `.character changerace`, and `.character changefaction`. The core's own character screen handlers then do the conversion (items, spells, reputations, titles, and quests through the stock `player_factionchange_*` tables).
- What was paid for the pending change is kept in the player setting `mod-tofu-character-services` index 0 (`character_settings`, needs `EnablePlayerSettings = 1`).
- Bots are recognized by their headless session, so the module does not depend on mod-playerbots.

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
