/*
 * mod-combat-watchdog loader.
 *
 * AzerothCore looks up a loader symbol derived from the module's folder name: for folder
 * "mod-combat-watchdog" that symbol is exactly "Addmod_combat_watchdogScripts". If you clone the
 * repo under a different folder name, rename this function to match.
 *
 * Released under the MIT License.
 */

void AddCombatWatchdogScripts();

void Addmod_combat_watchdogScripts()
{
    AddCombatWatchdogScripts();
}
