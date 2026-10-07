# Combat Watchdog

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module. It releases players and bots
from combat with creatures that stopped fighting them.

In dungeons a mob can end up somewhere the group can't reach: knocked into water, stuck on a
ledge, pulled through a wall. Instanced creatures don't leash, so it keeps everyone in combat
forever. Nobody can drink, eat or mount, and porting out and back doesn't help because the mob is
still in the instance. A boss stuck this way is worse: it pulls everyone within 250 yards back
into combat every 5 seconds. This happens a lot with bot groups on long clears like Blackrock
Depths.

With this module:

- **Every creature tracks when it last fought.** Entering combat, dealing or taking damage,
  healing or being healed, casting, or standing in melee range of its target all count.
- **Idle creatures are released.** If a creature you're in combat with has done none of that for
  30 seconds (90 for bosses), it's sent into evade mode. It goes home and drops combat with the
  whole group at once. If its script ignores the evade, its combat with players, pets and bots is
  ended directly.
- **You're told when it happens.** Real players get a system message naming the creature.
- **Every release is logged** with the creature's entry, spawn ID, map and position, so places
  that keep causing it can be fixed properly.

The core's own unreachable-target evade (10 seconds) still runs first. The watchdog only catches
what that misses. It runs in dungeons by default; raids and the open world can be turned on in
the config. Battlegrounds and arenas are never watched.

## Patch Notes: Combat Watchdog

Category: Dungeons

- You no longer stay stuck in combat after a dungeon fight ends. Monsters that stop fighting for
  30 seconds (90 for bosses) now give up and return home, and you're told when it happens.
- Bots are released too, so they can eat, drink and follow you again.

> Mobs knocked into water or stranded on ledges used to hold whole groups in combat until
> someone left the group. Blackrock Depths full clears were the worst offender.

## Requirements

- AzerothCore (wotlk, master branch) from mid-2026 on, which has the TrinityCore-style `CombatManager`.
- A WoW 3.3.5a (12340) client. No SQL, no client patch.
- Optional: the mod-playerbots core fork. Bot sessions are detected with `WorldSession::IsHeadless()` or, on older cores, `IsBot()`; that only decides who gets the chat message.

## Installation

Clone it into your AzerothCore `modules` folder, **as `mod-combat-watchdog`**. AzerothCore derives
the module's loader name from the folder name:

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/buildthehomelab/wow-mod-combat-watchdog.git mod-combat-watchdog
```

Re-run CMake, rebuild the worldserver, and copy `conf/mod_combat_watchdog.conf.dist` to
`mod_combat_watchdog.conf` in your config directory. The module needs no SQL.

It needs the TrinityCore-style `CombatManager` (AzerothCore from mid-2026 on, including the
playerbots core fork). Bots are detected with `WorldSession::IsHeadless()` or, on older playerbots
cores, `WorldSession::IsBot()`; that only decides who gets the chat message.

To check that it's loaded, look for this line in the worldserver log at startup:

```
mod-combat-watchdog: enabled, idle 30s (bosses 90s), dungeons on, raids off, open world off
```

## Configuration

| Setting | Default | What it does |
| --- | --- | --- |
| `CombatWatchdog.Enable` | `1` | Master switch. |
| `CombatWatchdog.IdleSeconds` | `30` | Idle time before a creature is released. |
| `CombatWatchdog.BossIdleSeconds` | `90` | The same for dungeon and world bosses. |
| `CombatWatchdog.CheckInterval` | `2000` | How often (ms) each player or bot in combat is checked. |
| `CombatWatchdog.Dungeons` | `1` | Watch 5-player dungeons. |
| `CombatWatchdog.Raids` | `0` | Watch raids. Off because some raid bosses have long untouchable phases. |
| `CombatWatchdog.OpenWorld` | `0` | Watch the open world, where creatures already leash. |
| `CombatWatchdog.Evade` | `1` | Evade the creature (`1`) or only end its combat with players (`0`). |
| `CombatWatchdog.Announce` | `1` | Tell real players when they are released. |
| `CombatWatchdog.IgnoreEntries` | `""` | Comma-separated creature entries never to release. |

## Finding the culprits

Each release writes a line like this to the worldserver log (the `module` logger):

```
[CombatWatchdog] Releasing <name> (entry <entry>, spawn <spawn>, boss no) on map 230 instance <id> at <x> <y> <z>: idle 31s, 5 player-side combat refs
```

Go there with `.go creature <spawn>` to see why it got stuck. In game, `.debug combat` lists
everything you (or your target) are in combat with.

## How it works

A `UnitScript` stamps the time on a creature whenever it enters combat, deals or takes damage, or
heals or is healed. The stamp is cleared when it leaves combat. A `PlayerScript` update hook runs
on every player and bot that is in combat, once per `CheckInterval`, in that player's map thread.
It looks at each creature in the player's PvE combat references. A creature that is casting or in
melee range of its victim is fighting. Otherwise, if its stamp is older than the idle limit,
it is released.

## Troubleshooting

- **The `enabled, idle 30s` line is missing from the worldserver log:** the module isn't in the build. Re-run CMake and rebuild.
- **Nothing is released in a raid or the open world:** both are off by default. Set `CombatWatchdog.Raids` or `CombatWatchdog.OpenWorld` to `1`. Raids are off because some bosses have long untouchable phases.
- **A creature must never be released:** add its entry to `CombatWatchdog.IgnoreEntries`.
- **A creature ignores the evade:** with `CombatWatchdog.Evade` at `1` the module ends its combat with players, pets and bots directly when the script ignores the evade. Each release is logged with the entry, spawn ID, map and position.

## Credits

Author: [buildthehomelab](https://github.com/buildthehomelab)

## License

MIT. See [LICENSE](LICENSE).
