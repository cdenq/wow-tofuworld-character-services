-- revert_tofu_character_services_2026_11_22_00_archivist.sql
-- Removes everything tofu_character_services_2026_11_22_00_archivist.sql adds (creature_template and creature_template_model 900042, npc_text 900018 to 900019) and any spawn of 900042. Run by hand in HeidiSQL against acore_world only after the module is removed from the build; otherwise the updater adds the rows back at the next start.
-- Schema: acore_world. Re-runnable. No stock rows change.
-- Check: SELECT COUNT(*) FROM acore_world.creature_template WHERE entry = 900042;  -- expected 0

DELETE FROM `creature` WHERE `id1` = 900042;
DELETE FROM `creature_template_model` WHERE `CreatureID` = 900042;
DELETE FROM `creature_template` WHERE `entry` = 900042;
DELETE FROM `npc_text` WHERE `ID` BETWEEN 900018 AND 900019;
