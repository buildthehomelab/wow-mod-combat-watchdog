/*
 * mod-combat-watchdog
 *
 * Releases players and bots from combat with creatures that stopped fighting them. In dungeons
 * a mob that gets stranded somewhere the group can't reach (knocked into water, stuck on a ledge,
 * pulled through a wall) can keep its combat reference on the whole group forever: instanced
 * creatures don't leash, and a boss stuck that way pulls everyone within 250 yards back into
 * combat every 5 seconds. Nobody in the group can drink, mount or leave combat, and porting out
 * and back doesn't help because the mob is still in the instance waiting for you.
 *
 * How:
 *   1. Every creature remembers when it last did something: entered combat, dealt or took
 *      damage, healed or was healed.
 *   2. Every few seconds, each player or bot in combat looks at the creatures it is in combat
 *      with. A creature that is casting, or standing in melee range of its target, counts as
 *      fighting. One that has done nothing for IdleSeconds (BossIdleSeconds for bosses) is stale.
 *   3. A stale creature is sent into evade mode, which ends its combat with everyone at once.
 *      If its script ignores the evade, its combat with players and their pets/bots is ended
 *      directly.
 *
 * The core's own unreachable-target evade (10 s) still runs first; this only catches what it
 * misses. Every release is logged with the creature's entry, spawn ID and position so recurring
 * spots can be fixed for real.
 *
 * Released under the MIT License.
 */

#include "Chat.h"
#include "CombatManager.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DataMap.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    struct Config
    {
        bool enabled = true;
        uint32 checkInterval = 2000;  // ms
        uint32 idleMs = 30000;
        uint32 bossIdleMs = 90000;
        bool dungeons = true;
        bool raids = false;
        bool openWorld = false;
        bool evade = true;
        bool announce = true;
        std::unordered_set<uint32> ignoredEntries;
    };

    Config config;

    std::string const ACTIVITY_KEY = "CombatWatchdog.Activity";
    std::string const TIMER_KEY = "CombatWatchdog.Timer";

    // Safety cap when ending references one by one: AI callbacks could in theory re-add combat.
    constexpr uint32 MAX_REF_ENDS = 64;

    struct CreatureActivity : public DataMap::Base
    {
        uint64 lastMs = 0;
    };

    struct PlayerTimer : public DataMap::Base
    {
        uint32 remaining = 0;
    };

    // Bot sessions: IsHeadless() on current playerbots cores, IsBot() on older ones.
    template <typename Session, typename = void>
    struct HasIsHeadless : std::false_type { };

    template <typename Session>
    struct HasIsHeadless<Session, std::void_t<decltype(std::declval<Session&>().IsHeadless())>> : std::true_type { };

    template <typename Session, typename = void>
    struct HasIsBot : std::false_type { };

    template <typename Session>
    struct HasIsBot<Session, std::void_t<decltype(std::declval<Session&>().IsBot())>> : std::true_type { };

    template <typename Session>
    bool IsBotSession(Session* session)
    {
        if constexpr (HasIsHeadless<Session>::value)
            return session->IsHeadless();
        else if constexpr (HasIsBot<Session>::value)
            return session->IsBot();
        else
            return false;
    }

    bool IsRealPlayer(Player* player)
    {
        WorldSession* session = player ? player->GetSession() : nullptr;
        return session && !IsBotSession(session);
    }

    uint64 NowMs()
    {
        return static_cast<uint64>(GameTime::GetGameTimeMS().count());
    }

    void Touch(Unit* unit)
    {
        if (Creature* creature = unit ? unit->ToCreature() : nullptr)
            creature->CustomData.GetDefault<CreatureActivity>(ACTIVITY_KEY)->lastMs = NowMs();
    }

    bool ShouldWatch(Map const* map)
    {
        if (!map || map->IsBattlegroundOrArena())
            return false;
        if (map->IsRaid())
            return config.raids;
        if (map->IsDungeon())
            return config.dungeons;
        return config.openWorld;
    }

    bool IsBoss(Creature const* creature)
    {
        return creature->IsDungeonBoss() || creature->isWorldBoss();
    }

    // How long the creature has done nothing, or 0 if it is fighting right now.
    uint64 IdleFor(Creature* creature, uint64 now)
    {
        CreatureActivity* activity = creature->CustomData.GetDefault<CreatureActivity>(ACTIVITY_KEY);

        bool fighting = creature->HasUnitState(UNIT_STATE_CASTING);
        if (!fighting)
            if (Unit* victim = creature->GetVictim())
                fighting = creature->IsWithinMeleeRange(victim);

        // First time we see it (engaged before the module loaded, or the enter-combat hook
        // didn't fire): start its clock now instead of judging it on no data.
        if (fighting || !activity->lastMs)
        {
            activity->lastMs = now;
            return 0;
        }

        return now > activity->lastMs ? now - activity->lastMs : 0;
    }

    bool IsStale(Creature* creature, uint64 now, uint64& idle)
    {
        if (!creature->IsAlive() || creature->IsControlledByPlayer())
            return false;
        if (config.ignoredEntries.count(creature->GetEntry()))
            return false;

        idle = IdleFor(creature, now);
        return idle >= (IsBoss(creature) ? config.bossIdleMs : config.idleMs);
    }

    // The creature's combat references to players, their pets and bots. Re-read each time
    // because ending one invalidates the map.
    CombatReference* FindPlayerSideRef(Creature* creature)
    {
        for (auto const& [guid, ref] : creature->GetCombatManager().GetPvECombatRefs())
            if (ref->GetOther(creature)->IsControlledByPlayer())
                return ref;
        return nullptr;
    }

    void Release(Creature* creature, uint64 idle)
    {
        // Who to tell, before the references are gone.
        std::vector<ObjectGuid> recipients;
        uint32 holders = 0;
        for (auto const& [guid, ref] : creature->GetCombatManager().GetPvECombatRefs())
        {
            Unit* other = ref->GetOther(creature);
            if (!other->IsControlledByPlayer())
                continue;
            ++holders;
            if (Player* player = other->ToPlayer())
                if (IsRealPlayer(player))
                    recipients.push_back(player->GetGUID());
        }

        std::string const name = creature->GetName();
        uint32 const seconds = static_cast<uint32>(idle / IN_MILLISECONDS);

        LOG_INFO("module", "[CombatWatchdog] Releasing {} (entry {}, spawn {}, boss {}) on map {} instance {} at "
            "{:.1f} {:.1f} {:.1f}: idle {}s, {} player-side combat refs",
            name, creature->GetEntry(), creature->GetSpawnId(), IsBoss(creature) ? "yes" : "no",
            creature->GetMapId(), creature->GetInstanceId(), creature->GetPositionX(), creature->GetPositionY(),
            creature->GetPositionZ(), seconds, holders);

        if (config.evade && creature->IsAIEnabled)
            if (CreatureAI* ai = creature->AI())
                ai->EnterEvadeMode(CreatureAI::EVADE_REASON_OTHER);

        // Scripts can ignore the evade (and evade is off by config): end player-side combat directly.
        uint32 ended = 0;
        while (ended < MAX_REF_ENDS)
        {
            CombatReference* ref = FindPlayerSideRef(creature);
            if (!ref)
                break;
            ref->EndCombat();
            ++ended;
        }

        if (ended)
            LOG_INFO("module", "[CombatWatchdog] {} kept combat after evade; ended {} refs directly", name, ended);

        if (!config.announce)
            return;

        for (ObjectGuid const& guid : recipients)
            if (Player* player = ObjectAccessor::FindPlayer(guid))
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff00ccff[Combat Watchdog]|r Released you from combat with {}: it hadn't fought anyone for {} seconds.",
                    name, seconds);
    }

    void Check(Player* player)
    {
        if (!player->IsInWorld() || !ShouldWatch(player->GetMap()))
            return;

        uint64 const now = NowMs();
        std::vector<std::pair<ObjectGuid, uint64>> stale;

        for (auto const& [guid, ref] : player->GetCombatManager().GetPvECombatRefs())
        {
            Creature* creature = ref->GetOther(player)->ToCreature();
            uint64 idle = 0;
            if (creature && IsStale(creature, now, idle))
                stale.emplace_back(creature->GetGUID(), idle);
        }

        for (auto const& [guid, idle] : stale)
            if (Creature* creature = ObjectAccessor::GetCreature(*player, guid))
                if (creature->IsInCombat())
                    Release(creature, idle);
    }

    std::unordered_set<uint32> ParseEntries(std::string const& list)
    {
        std::unordered_set<uint32> entries;
        std::stringstream stream(list);
        std::string token;
        while (std::getline(stream, token, ','))
        {
            try
            {
                if (token.find_first_not_of(" \t") != std::string::npos)
                    entries.insert(static_cast<uint32>(std::stoul(token)));
            }
            catch (...)
            {
                LOG_ERROR("server.loading", "mod-combat-watchdog: ignoring bad IgnoreEntries value '{}'", token);
            }
        }
        return entries;
    }
}

class CombatWatchdogWorldScript : public WorldScript
{
public:
    CombatWatchdogWorldScript() : WorldScript("CombatWatchdogWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        config.enabled = sConfigMgr->GetOption<bool>("CombatWatchdog.Enable", true);
        config.checkInterval = std::max<uint32>(500, sConfigMgr->GetOption<uint32>("CombatWatchdog.CheckInterval", 2000));
        config.idleMs = sConfigMgr->GetOption<uint32>("CombatWatchdog.IdleSeconds", 30) * IN_MILLISECONDS;
        config.bossIdleMs = sConfigMgr->GetOption<uint32>("CombatWatchdog.BossIdleSeconds", 90) * IN_MILLISECONDS;
        config.dungeons = sConfigMgr->GetOption<bool>("CombatWatchdog.Dungeons", true);
        config.raids = sConfigMgr->GetOption<bool>("CombatWatchdog.Raids", false);
        config.openWorld = sConfigMgr->GetOption<bool>("CombatWatchdog.OpenWorld", false);
        config.evade = sConfigMgr->GetOption<bool>("CombatWatchdog.Evade", true);
        config.announce = sConfigMgr->GetOption<bool>("CombatWatchdog.Announce", true);
        config.ignoredEntries = ParseEntries(sConfigMgr->GetOption<std::string>("CombatWatchdog.IgnoreEntries", ""));

        LOG_INFO("server.loading", "mod-combat-watchdog: {}, idle {}s (bosses {}s), dungeons {}, raids {}, open world {}",
            config.enabled ? "enabled" : "disabled", config.idleMs / IN_MILLISECONDS, config.bossIdleMs / IN_MILLISECONDS,
            config.dungeons ? "on" : "off", config.raids ? "on" : "off", config.openWorld ? "on" : "off");
    }
};

class CombatWatchdogUnitScript : public UnitScript
{
public:
    CombatWatchdogUnitScript() : UnitScript("CombatWatchdogUnitScript", true, {
        UNITHOOK_ON_DAMAGE,
        UNITHOOK_ON_HEAL,
        UNITHOOK_ON_UNIT_ENTER_COMBAT,
        UNITHOOK_ON_UNIT_EXIT_COMBAT
    }) { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& /*damage*/) override
    {
        if (!config.enabled)
            return;
        Touch(attacker);
        Touch(victim);
    }

    void OnHeal(Unit* healer, Unit* receiver, uint32& /*gain*/) override
    {
        if (!config.enabled)
            return;
        Touch(healer);
        Touch(receiver);
    }

    void OnUnitEnterCombat(Unit* unit, Unit* /*victim*/) override
    {
        if (config.enabled)
            Touch(unit);
    }

    // Forget the last fight, so a creature re-engaged later doesn't look idle from the start.
    void OnUnitExitCombat(Unit* unit) override
    {
        if (Creature* creature = unit ? unit->ToCreature() : nullptr)
            if (CreatureActivity* activity = creature->CustomData.Get<CreatureActivity>(ACTIVITY_KEY))
                activity->lastMs = 0;
    }
};

// Runs in the player's map thread, so touching creatures on the same map is safe. Bots are
// players too, which matters when only the bots are held.
class CombatWatchdogPlayerScript : public PlayerScript
{
public:
    CombatWatchdogPlayerScript() : PlayerScript("CombatWatchdogPlayerScript", { PLAYERHOOK_ON_UPDATE }) { }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!config.enabled || !player->IsInCombat())
            return;

        PlayerTimer* timer = player->CustomData.GetDefault<PlayerTimer>(TIMER_KEY);
        if (timer->remaining > diff)
        {
            timer->remaining -= diff;
            return;
        }

        timer->remaining = config.checkInterval;
        Check(player);
    }
};

void AddCombatWatchdogScripts()
{
    new CombatWatchdogWorldScript();
    new CombatWatchdogUnitScript();
    new CombatWatchdogPlayerScript();
}
