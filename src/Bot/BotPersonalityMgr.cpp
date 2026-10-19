/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotPersonalityMgr.h"
#include "AreaDefines.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomBotLevelMgr.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include <algorithm>
#include <cmath>

namespace
{
// Bots in a faction below this count get no pressure; shares are too noisy to act on.
constexpr uint32 PRESSURE_MIN_BOTS = 20;

// A new upstream RPG status must be added to the profiles and GetExpectedSeconds() below, or bots
// with a personality never use it.
static_assert(RPG_STATUS_END == 10, "Update the personality profiles in BotPersonalityMgr.cpp");

// Profiles are target shares of time (percent). Statuses last very different times (resting 30 s,
// questing up to 30 min), so each share is divided by the expected stint length to get a weight.
// Statuses missing from a profile are never chosen. Unavailable statuses drop out and the rest
// keep their ratio.
struct StatusShare
{
    NewRpgStatus status;
    uint32 pct;
};

constexpr StatusShare PROFILE_QUESTER[] = {{RPG_DO_QUEST, 75}, {RPG_GO_GRIND, 5},    {RPG_WANDER_RANDOM, 5},
                                           {RPG_GO_CAMP, 3},   {RPG_WANDER_NPC, 3},  {RPG_TRAVEL_FLIGHT, 3},
                                           {RPG_REST, 1},      {RPG_OUTDOOR_PVP, 2}, {RPG_DO_GATHER, 3}};
constexpr StatusShare PROFILE_GRINDER[] = {{RPG_DO_QUEST, 10}, {RPG_GO_GRIND, 35},   {RPG_WANDER_RANDOM, 40},
                                           {RPG_GO_CAMP, 3},   {RPG_WANDER_NPC, 3},  {RPG_TRAVEL_FLIGHT, 3},
                                           {RPG_REST, 1},      {RPG_OUTDOOR_PVP, 2}, {RPG_DO_GATHER, 3}};
constexpr StatusShare PROFILE_TRAVELER[] = {{RPG_DO_QUEST, 35}, {RPG_GO_GRIND, 10},   {RPG_WANDER_RANDOM, 10},
                                            {RPG_GO_CAMP, 10},  {RPG_WANDER_NPC, 10}, {RPG_TRAVEL_FLIGHT, 20},
                                            {RPG_REST, 1},      {RPG_OUTDOOR_PVP, 1}, {RPG_DO_GATHER, 3}};
// Lazy at a quest hub or inn: sits and chats with the NPCs there, rarely quests.
constexpr StatusShare PROFILE_LAZY_AT_CAMP[] = {{RPG_REST, 65}, {RPG_WANDER_NPC, 30}, {RPG_DO_QUEST, 5}};
// Lazy anywhere else: heads back to a quest hub.
constexpr StatusShare PROFILE_LAZY_AWAY[] = {{RPG_GO_CAMP, 85}, {RPG_REST, 10}, {RPG_DO_QUEST, 5}};
constexpr StatusShare PROFILE_GATHERER[] = {
    {RPG_DO_GATHER, 60}, {RPG_REST, 15}, {RPG_GO_CAMP, 10}, {RPG_WANDER_NPC, 10}, {RPG_DO_QUEST, 5}};
// CityIdler inside a city, only used while grouped (HandleCityIdlerIdle covers the rest).
constexpr StatusShare PROFILE_CITY_IDLER_IN_CITY[] = {{RPG_REST, 60}, {RPG_WANDER_NPC, 40}};

template <size_t N>
uint32 FindShare(StatusShare const (&profile)[N], NewRpgStatus status)
{
    for (StatusShare const& entry : profile)
    {
        if (entry.status == status)
            return entry.pct;
    }
    return 0;
}

uint32 GetTimeSharePct(BotActivityProfile profile, NewRpgStatus status)
{
    switch (profile)
    {
        case BotActivityProfile::Quester:
            return FindShare(PROFILE_QUESTER, status);
        case BotActivityProfile::Grinder:
            return FindShare(PROFILE_GRINDER, status);
        case BotActivityProfile::Traveler:
            return FindShare(PROFILE_TRAVELER, status);
        case BotActivityProfile::LazyAtCamp:
            return FindShare(PROFILE_LAZY_AT_CAMP, status);
        case BotActivityProfile::LazyAway:
            return FindShare(PROFILE_LAZY_AWAY, status);
        case BotActivityProfile::Gatherer:
            return FindShare(PROFILE_GATHERER, status);
        case BotActivityProfile::CityIdlerInCity:
            return FindShare(PROFILE_CITY_IDLER_IN_CITY, status);
    }
    return 0;
}

// Rough average stint length per status, in seconds (see the durations in NewRpgAction.h; going to
// a grind spot or camp includes the walk and the wandering that follows).
uint32 GetExpectedSeconds(NewRpgStatus status)
{
    switch (status)
    {
        case RPG_GO_GRIND:
        case RPG_GO_CAMP:
            return 420;
        case RPG_WANDER_RANDOM:
        case RPG_WANDER_NPC:
            return 300;
        case RPG_DO_QUEST:
        case RPG_DO_GATHER:
            return 900;
        case RPG_TRAVEL_FLIGHT:
            return 240;
        case RPG_REST:
            return 30;
        case RPG_OUTDOOR_PVP:
            return 3600;
        default:
            return 300;
    }
}

// Scale so the smallest share (1% of a one-hour status) still yields a weight of 1.
constexpr uint32 WEIGHT_SCALE = 3600;

// CityIdler inside a city: stays at a spot for a random time, then either visits NPCs (chance per
// decision; about a quarter of its time, since a visit lasts 5 minutes) or walks to a new open spot.
constexpr uint32 CITY_IDLER_STAY_MIN_MS = 10 * IN_MILLISECONDS;
constexpr uint32 CITY_IDLER_STAY_MAX_MS = 5 * MINUTE * IN_MILLISECONDS;
constexpr uint32 CITY_IDLER_NPC_VISIT_CHANCE = 20;
constexpr float CITY_IDLER_SPOT_MIN_DISTANCE = 20.0f;
constexpr float CITY_IDLER_SPOT_MAX_DISTANCE = 60.0f;
constexpr uint32 CITY_IDLER_SPOT_ATTEMPTS = 12;
// A spot must be fully reachable without a long detour around buildings.
constexpr float CITY_IDLER_MAX_DETOUR = 2.0f;
constexpr float CITY_IDLER_SPOT_TOLERANCE = 2.0f;
// Free space required around a spot: no wall within this distance in any of the probe directions,
// and no drop (canal, ledge) right next to it.
constexpr float CITY_IDLER_CLEARANCE = 3.0f;
constexpr uint32 CITY_IDLER_CLEARANCE_DIRECTIONS = 8;
constexpr float CITY_IDLER_MAX_GROUND_STEP = 1.5f;
// Bots look at someone standing this close, otherwise out into the open.
constexpr float CITY_IDLER_FACE_PLAYER_DISTANCE = 8.0f;
constexpr float CITY_IDLER_VIEW_DISTANCE = 15.0f;
// Arrival at a spot; a bot that stops short or gets stuck stays where it is after a while.
constexpr float CITY_IDLER_ARRIVAL_DISTANCE = 1.5f;
constexpr float CITY_IDLER_STOPPED_DISTANCE = 5.0f;
constexpr uint32 CITY_IDLER_WALK_TIMEOUT_MS = 2 * MINUTE * IN_MILLISECONDS;
constexpr float EYE_HEIGHT = 1.5f;
constexpr uint32 SIT_STAND_SALT = 0x5F3759DFu;
constexpr uint32 MAX_LEVEL_SALT = 0x2545F491u;
// Each bot rolls its personality anew every period. The period length (2 to 6 hours) and its start
// differ per bot, so bots do not all switch at the same moment.
constexpr uint32 PERIOD_MIN_SECONDS = 2 * HOUR;
constexpr uint32 PERIOD_MAX_SECONDS = 6 * HOUR;
constexpr uint32 PERIOD_LENGTH_SALT = 0x6C8E9CF5u;
constexpr uint32 PERIOD_OFFSET_SALT = 0x1B873593u;

// A bot this close to an inn or quest hub counts as being at camp. Matches the minimum distance
// SelectRandomCampPos uses, so a bot just outside always has a camp to walk back to.
constexpr float CAMP_RADIUS = 50.0f;

// Integer hash so personalities do not follow GUID creation order (bots are created one per class).
uint32 MixGuidCounter(uint32 value)
{
    constexpr uint32 MIX_MULTIPLIER_1 = 0x7FEB352Du;
    constexpr uint32 MIX_MULTIPLIER_2 = 0x846CA68Bu;
    value ^= value >> 16;
    value *= MIX_MULTIPLIER_1;
    value ^= value >> 15;
    value *= MIX_MULTIPLIER_2;
    value ^= value >> 16;
    return value;
}

// Index of the bot's current personality period; changes every 2 to 6 hours.
uint32 GetPersonalityPeriod(uint32 guidCounter)
{
    uint32 const length = PERIOD_MIN_SECONDS +
                          MixGuidCounter(guidCounter ^ PERIOD_LENGTH_SALT) % (PERIOD_MAX_SECONDS - PERIOD_MIN_SECONDS);
    uint32 const offset = MixGuidCounter(guidCounter ^ PERIOD_OFFSET_SALT) % length;
    uint64 const now = static_cast<uint64>(GameTime::GetGameTime().count());
    return static_cast<uint32>((now + offset) / length);
}

// Bots in a group or inside a dungeon, raid or battleground ignore their personality.
bool IsPersonalityFree(Player* bot)
{
    if (bot->GetGroup())
        return false;
    Map* map = bot->GetMap();
    return map && !map->Instanceable();
}

// Fixed per bot, so a bot does not switch between sitting and standing every rest stint.
bool SitsWhileIdling(Player* bot) { return MixGuidCounter(bot->GetGUID().GetCounter() ^ SIT_STAND_SALT) & 1; }

void StartRestHere(Player* bot, NewRpgInfo& info)
{
    info.ChangeToRest();
    bot->SetStandState(SitsWhileIdling(bot) ? UNIT_STAND_STATE_SIT : UNIT_STAND_STATE_STAND);
}

bool IsOpenTowards(Map* map, uint32 phaseMask, float x, float y, float z, float angle, float distance)
{
    return map->isInLineOfSight(x, y, z + EYE_HEIGHT, x + std::cos(angle) * distance, y + std::sin(angle) * distance,
                                z + EYE_HEIGHT, phaseMask, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
}

// True if no wall is within CITY_IDLER_CLEARANCE in any probe direction and the ground around the
// spot is level, so the bot does not stand against a wall or at the edge of a canal.
bool HasClearance(Map* map, uint32 phaseMask, float x, float y, float z)
{
    for (uint32 i = 0; i < CITY_IDLER_CLEARANCE_DIRECTIONS; ++i)
    {
        float const angle = 2.0f * static_cast<float>(M_PI) * i / CITY_IDLER_CLEARANCE_DIRECTIONS;
        if (!IsOpenTowards(map, phaseMask, x, y, z, angle, CITY_IDLER_CLEARANCE))
            return false;

        float const probeX = x + std::cos(angle) * CITY_IDLER_CLEARANCE;
        float const probeY = y + std::sin(angle) * CITY_IDLER_CLEARANCE;
        float const ground = map->GetHeight(phaseMask, probeX, probeY, z + CITY_IDLER_MAX_GROUND_STEP);
        if (ground <= INVALID_HEIGHT || std::fabs(ground - z) > CITY_IDLER_MAX_GROUND_STEP)
            return false;
    }
    return true;
}

// Faces a player or bot standing close by, otherwise a random direction with open view.
void FaceSomethingSensible(Player* bot)
{
    if (Player* neighbour = bot->SelectNearestPlayer(CITY_IDLER_FACE_PLAYER_DISTANCE))
    {
        bot->SetFacingToObject(neighbour);
        return;
    }

    Map* map = bot->GetMap();
    float const start = frand(0.0f, 2.0f * static_cast<float>(M_PI));
    for (uint32 i = 0; i < CITY_IDLER_CLEARANCE_DIRECTIONS; ++i)
    {
        float const angle = start + 2.0f * static_cast<float>(M_PI) * i / CITY_IDLER_CLEARANCE_DIRECTIONS;
        if (IsOpenTowards(map, bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                          angle, CITY_IDLER_VIEW_DISTANCE))
        {
            bot->SetFacingTo(Position::NormalizeOrientation(angle));
            return;
        }
    }
    bot->SetFacingTo(Position::NormalizeOrientation(start));
}

uint32 TeamIndex(TeamId team) { return team == TEAM_ALLIANCE ? 0 : 1; }

bool IsLevelingStatus(NewRpgStatus status)
{
    return status == RPG_DO_QUEST || status == RPG_GO_GRIND || status == RPG_WANDER_RANDOM;
}

// Spreads arrivals around a banker spot so idlers do not stack on one point: 1 to 5 yards from it in
// a random direction, in sight of the original spot. The spot stands 6 yards in front of the banker
// and faces it; moving at most 2 yards towards the banker keeps bots off the counter.
void RandomizeArrivalSpot(WorldLocation& loc)
{
    Map* map = sMapMgr->FindMap(loc.GetMapId(), 0);
    if (!map)
        return;

    constexpr float MIN_OFFSET = 1.0f;
    constexpr float MAX_OFFSET = 5.0f;
    constexpr float MAX_TOWARDS_BANKER = 2.0f;
    float const facing = loc.GetOrientation();
    float const distance = frand(MIN_OFFSET, MAX_OFFSET);
    float const direction = frand(0.0f, 2.0f * static_cast<float>(M_PI));
    float const forward = std::min(MAX_TOWARDS_BANKER, distance * std::cos(direction));
    float const sideways = distance * std::sin(direction);
    float const x = loc.GetPositionX() + std::cos(facing) * forward - std::sin(facing) * sideways;
    float const y = loc.GetPositionY() + std::sin(facing) * forward + std::cos(facing) * sideways;
    float const z = loc.GetPositionZ();

    // Static geometry only: the target map runs on another thread, so its game objects are off limits.
    if (!map->isInLineOfSight(loc.GetPositionX(), loc.GetPositionY(), z + EYE_HEIGHT, x, y, z + EYE_HEIGHT,
                              PHASEMASK_NORMAL, LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::Nothing))
        return;

    loc.Relocate(x, y, z, facing);
}

uint32 GetFactionCapital(TeamId team) { return team == TEAM_ALLIANCE ? AREA_STORMWIND_CITY : AREA_ORGRIMMAR; }

uint32 GetRacialCapital(Player* bot)
{
    switch (bot->getRace())
    {
        case RACE_HUMAN:
            return AREA_STORMWIND_CITY;
        case RACE_DWARF:
        case RACE_GNOME:
            return AREA_IRONFORGE;
        case RACE_NIGHTELF:
            return AREA_DARNASSUS;
        case RACE_DRAENEI:
            return AREA_THE_EXODAR;
        case RACE_ORC:
        case RACE_TROLL:
            return AREA_ORGRIMMAR;
        case RACE_TAUREN:
            return AREA_THUNDER_BLUFF;
        case RACE_UNDEAD_PLAYER:
            return AREA_UNDERCITY;
        case RACE_BLOODELF:
            return AREA_SILVERMOON_CITY;
        default:
            return GetFactionCapital(bot->GetTeamId());
    }
}
}  // namespace

BotPersonalityMgr::BotPersonalityMgr()
{
    for (auto& teamFactors : _levelFactorPct)
        for (auto& factor : teamFactors)
            factor.store(FULL_FACTOR_PCT, std::memory_order_relaxed);
}

bool BotPersonalityMgr::IsInCapital(Player* bot) { return TravelMgr::IsCapitalZone(bot->GetZoneId()); }

bool BotPersonalityMgr::IsAtCamp(Player* bot)
{
    if (bot->HasRestFlag(REST_FLAG_IN_TAVERN))
        return true;

    for (WorldLocation const& hub : sTravelMgr.GetTravelHubs(bot))
    {
        if (hub.GetMapId() == bot->GetMapId() && bot->GetExactDist(hub) < CAMP_RADIUS)
            return true;
    }
    return false;
}

BotActivityProfile BotPersonalityMgr::GetActivityProfile(Player* bot, BotPersonality personality)
{
    switch (personality)
    {
        case BotPersonality::Quester:
            return BotActivityProfile::Quester;
        case BotPersonality::Grinder:
            return BotActivityProfile::Grinder;
        case BotPersonality::Traveler:
            return BotActivityProfile::Traveler;
        case BotPersonality::Gatherer:
            if (bot->HasSkill(SKILL_HERBALISM) || bot->HasSkill(SKILL_MINING))
                return BotActivityProfile::Gatherer;
            break;
        case BotPersonality::CityIdler:
            if (IsInCapital(bot))
                return BotActivityProfile::CityIdlerInCity;
            break;
        default:
            break;
    }
    // Lazy, plus Gatherers without a gathering skill and CityIdlers not (yet) in a city.
    return IsAtCamp(bot) ? BotActivityProfile::LazyAtCamp : BotActivityProfile::LazyAway;
}

BotPersonality BotPersonalityMgr::GetPersonality(Player* bot) const
{
    if (!bot || !sPlayerbotAIConfig.PersonalityEnabled)
        return BotPersonality::None;

    std::array<std::pair<BotPersonality, uint32>, 6> const shares = {{
        {BotPersonality::Quester, sPlayerbotAIConfig.PersonalityPctQuester},
        {BotPersonality::Grinder, sPlayerbotAIConfig.PersonalityPctGrinder},
        {BotPersonality::Traveler, sPlayerbotAIConfig.PersonalityPctTraveler},
        {BotPersonality::Lazy, sPlayerbotAIConfig.PersonalityPctLazy},
        {BotPersonality::CityIdler, sPlayerbotAIConfig.PersonalityPctCityIdler},
        {BotPersonality::Gatherer, sPlayerbotAIConfig.PersonalityPctGatherer},
    }};

    uint32 total = 0;
    for (auto const& share : shares)
        total += share.second;
    if (total == 0)
        return BotPersonality::None;

    uint32 const guidCounter = bot->GetGUID().GetCounter();
    uint32 const periodSeed = MixGuidCounter(GetPersonalityPeriod(guidCounter));
    BotPersonality personality = BotPersonality::None;
    uint32 roll = MixGuidCounter(guidCounter ^ periodSeed) % total;
    for (auto const& share : shares)
    {
        if (roll < share.second)
        {
            personality = share.first;
            break;
        }
        roll -= share.second;
    }

    return ApplyMaxLevelOverride(bot, personality);
}

// At the bot level cap, a second roll per period turns some bots into CityIdlers or Lazy bots.
// Raising the cap gives them their own personality back.
BotPersonality BotPersonalityMgr::ApplyMaxLevelOverride(Player* bot, BotPersonality personality)
{
    if (bot->GetLevel() < sPlayerbotAIConfig.RandomBotMaxLevel)
        return personality;

    uint32 const cityIdlerShare = sPlayerbotAIConfig.PersonalityMaxLevelCityIdler;
    uint32 const lazyShare = sPlayerbotAIConfig.PersonalityMaxLevelLazy;
    uint32 const total = cityIdlerShare + lazyShare + sPlayerbotAIConfig.PersonalityMaxLevelKeep;
    if (total == 0)
        return personality;

    uint32 const guidCounter = bot->GetGUID().GetCounter();
    uint32 const periodSeed = MixGuidCounter(GetPersonalityPeriod(guidCounter));
    uint32 const roll = MixGuidCounter(guidCounter ^ MAX_LEVEL_SALT ^ periodSeed) % total;
    if (roll < cityIdlerShare)
        return BotPersonality::CityIdler;
    if (roll < cityIdlerShare + lazyShare)
        return BotPersonality::Lazy;
    return personality;
}

char const* BotPersonalityMgr::GetPersonalityName(BotPersonality personality)
{
    switch (personality)
    {
        case BotPersonality::Quester:
            return "Quester";
        case BotPersonality::Grinder:
            return "Grinder";
        case BotPersonality::Traveler:
            return "Traveler";
        case BotPersonality::Lazy:
            return "Lazy";
        case BotPersonality::CityIdler:
            return "CityIdler";
        case BotPersonality::Gatherer:
            return "Gatherer";
        default:
            return "";
    }
}

uint32 BotPersonalityMgr::GetLevelFactorPct(TeamId team, uint8 level) const
{
    if (!sPlayerbotAIConfig.PersonalityPressureEnabled || (team != TEAM_ALLIANCE && team != TEAM_HORDE))
        return FULL_FACTOR_PCT;

    return _levelFactorPct[TeamIndex(team)][level].load(std::memory_order_relaxed);
}

bool BotPersonalityMgr::IsCityFull(uint32 zoneId) const
{
    uint32 const maxPerCity = sPlayerbotAIConfig.PersonalityCityIdlerMaxPerCity;
    if (maxPerCity == 0)
        return false;

    std::lock_guard<std::mutex> lock(_cityMutex);
    auto itr = _cityIdlerCounts.find(zoneId);
    return itr != _cityIdlerCounts.end() && itr->second >= maxPerCity;
}

uint32 BotPersonalityMgr::SelectCityIdlerCity(Player* bot) const
{
    uint32 const level = bot->GetLevel();
    uint32 const shattrathMinLevel = sPlayerbotAIConfig.PersonalityCityIdlerShattrathMinLevel;
    uint32 const dalaranMinLevel = sPlayerbotAIConfig.PersonalityCityIdlerDalaranMinLevel;
    TeamId const team = bot->GetTeamId();

    std::vector<uint32> viable;
    for (uint32 zoneId : TravelMgr::GetCapitalZones(team))
    {
        if (zoneId == AREA_SHATTRATH_CITY && level < shattrathMinLevel)
            continue;
        if (zoneId == AREA_DALARAN && level < dalaranMinLevel)
            continue;
        viable.push_back(zoneId);
    }

    bool const highLevel = level >= shattrathMinLevel;
    uint32 const neutralCity = level >= dalaranMinLevel ? AREA_DALARAN : AREA_SHATTRATH_CITY;
    uint32 const neutralShare = highLevel ? sPlayerbotAIConfig.PersonalityCityIdlerHighNeutral : 0;
    uint32 const capitalShare = highLevel ? sPlayerbotAIConfig.PersonalityCityIdlerHighCapital
                                          : sPlayerbotAIConfig.PersonalityCityIdlerLowCapital;
    uint32 const racialShare = highLevel ? sPlayerbotAIConfig.PersonalityCityIdlerHighRacial
                                         : sPlayerbotAIConfig.PersonalityCityIdlerLowRacial;
    uint32 const randomShare = highLevel ? sPlayerbotAIConfig.PersonalityCityIdlerHighRandom
                                         : sPlayerbotAIConfig.PersonalityCityIdlerLowRandom;

    uint32 chosen = 0;
    uint32 const total = neutralShare + capitalShare + racialShare + randomShare;
    if (total > 0)
    {
        uint32 roll = urand(0, total - 1);
        if (roll < neutralShare)
            chosen = neutralCity;
        else if ((roll -= neutralShare) < capitalShare)
            chosen = GetFactionCapital(team);
        else if ((roll -= capitalShare) < racialShare)
            chosen = GetRacialCapital(bot);
        else if (!viable.empty())
            chosen = viable[urand(0, viable.size() - 1)];
    }

    if (chosen && !IsCityFull(chosen) && !sTravelMgr.GetCapitalBankerLocations(chosen).empty())
        return chosen;

    // Chosen city full or unknown: fall back to any viable city that still has room.
    std::vector<uint32> open;
    for (uint32 zoneId : viable)
    {
        if (!IsCityFull(zoneId) && !sTravelMgr.GetCapitalBankerLocations(zoneId).empty())
            open.push_back(zoneId);
    }
    return open.empty() ? 0 : open[urand(0, open.size() - 1)];
}

WorldPosition BotPersonalityMgr::SelectCityIdleSpot(Player* bot)
{
    Map* map = bot->GetMap();
    if (!map)
        return WorldPosition();

    uint32 const zoneId = bot->GetZoneId();
    uint32 const phaseMask = bot->GetPhaseMask();
    for (uint32 attempt = 0; attempt < CITY_IDLER_SPOT_ATTEMPTS; ++attempt)
    {
        float const angle = frand(0.0f, 2.0f * static_cast<float>(M_PI));
        float const distance = frand(CITY_IDLER_SPOT_MIN_DISTANCE, CITY_IDLER_SPOT_MAX_DISTANCE);
        float const targetX = bot->GetPositionX() + std::cos(angle) * distance;
        float const targetY = bot->GetPositionY() + std::sin(angle) * distance;
        float const targetZ = map->GetHeight(phaseMask, targetX, targetY, bot->GetPositionZ() + distance / 2);
        if (targetZ <= INVALID_HEIGHT)
            continue;

        // A complete walking path, ending at the point itself rather than where a wall stops it.
        PathGenerator path(bot);
        if (!path.CalculatePath(targetX, targetY, targetZ) || path.GetPathType() != PATHFIND_NORMAL ||
            path.GetPath().empty())
            continue;
        if (path.getPathLength() > distance * CITY_IDLER_MAX_DETOUR)
            continue;

        G3D::Vector3 const& end = path.GetPath().back();
        if (std::hypot(end.x - targetX, end.y - targetY) > CITY_IDLER_SPOT_TOLERANCE)
            continue;
        if (map->GetZoneId(phaseMask, end.x, end.y, end.z) != zoneId)
            continue;
        if (map->IsInWater(phaseMask, end.x, end.y, end.z, bot->GetCollisionHeight()))
            continue;
        if (!HasClearance(map, phaseMask, end.x, end.y, end.z))
            continue;

        return WorldPosition(bot->GetMapId(), end.x, end.y, end.z);
    }
    return WorldPosition();
}

void BotPersonalityMgr::StartCityIdlerStay(Player* bot, NewRpgInfo& info)
{
    {
        std::lock_guard<std::mutex> lock(_stayMutex);
        _cityIdlerStays[bot->GetGUID().GetCounter()] = {getMSTime(),
                                                        urand(CITY_IDLER_STAY_MIN_MS, CITY_IDLER_STAY_MAX_MS)};
    }
    StartRestHere(bot, info);
}

bool BotPersonalityMgr::HandleCityIdlerIdle(Player* bot, NewRpgInfo& info)
{
    if (!bot || !IsPersonalityFree(bot))
        return false;

    if (GetPersonality(bot) != BotPersonality::CityIdler)
        return TrySendFormerCityIdlerBack(bot);

    if (!IsInCapital(bot))
        return TrySendCityIdlerToCity(bot);

    // Busy idlers follow the normal RPG logic.
    if (!bot->IsAlive() || bot->IsInCombat())
        return false;

    bool staying = false;
    {
        std::lock_guard<std::mutex> lock(_stayMutex);
        auto itr = _cityIdlerStays.find(bot->GetGUID().GetCounter());
        if (itr != _cityIdlerStays.end())
        {
            if (GetMSTimeDiffToNow(itr->second.start) < itr->second.duration)
                staying = true;
            else
                _cityIdlerStays.erase(itr);
        }
    }

    if (staying)
    {
        StartRestHere(bot, info);
        return true;
    }

    if (urand(1, 100) <= CITY_IDLER_NPC_VISIT_CHANCE)
    {
        info.ChangeToWanderNpc();
        return true;
    }

    WorldPosition const spot = SelectCityIdleSpot(bot);
    if (spot != WorldPosition())
        info.ChangeToGoCamp(spot);
    else
        StartCityIdlerStay(bot, info);
    return true;
}

bool BotPersonalityMgr::HandleCityIdlerWalk(Player* bot, NewRpgInfo& info, WorldPosition const& spot,
                                            bool& statusChanged)
{
    statusChanged = false;
    if (!bot || !IsPersonalityFree(bot) || GetPersonality(bot) != BotPersonality::CityIdler || !IsInCapital(bot))
        return false;

    float const distance = bot->GetExactDist(spot);
    bool const arrived = distance < CITY_IDLER_ARRIVAL_DISTANCE ||
                         (distance < CITY_IDLER_STOPPED_DISTANCE && !bot->isMoving()) ||
                         info.HasStatusPersisted(CITY_IDLER_WALK_TIMEOUT_MS);
    if (!arrived)
        return true;

    bot->StopMoving();
    FaceSomethingSensible(bot);
    StartCityIdlerStay(bot, info);
    statusChanged = true;
    return true;
}

// Teleports reset the bot's AI, which would drop a pending dungeon finder proposal, so never teleport
// a bot that is queued or has a proposal.
bool BotPersonalityMgr::CanTeleportNow(Player* bot)
{
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->IsBeingTeleported())
        return false;
    if (bot->InBattlegroundQueue() || sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
        return false;
    return IsPersonalityFree(bot);
}

bool BotPersonalityMgr::TrySendCityIdlerToCity(Player* bot)
{
    if (!bot || GetPersonality(bot) != BotPersonality::CityIdler || IsInCapital(bot))
        return false;

    if (!CanTeleportNow(bot))
        return false;

    uint32 const zoneId = SelectCityIdlerCity(bot);
    if (!zoneId)
        return false;

    std::vector<WorldLocation> locs = sTravelMgr.GetCapitalBankerLocations(zoneId);
    for (WorldLocation& loc : locs)
        RandomizeArrivalSpot(loc);
    sRandomPlayerbotMgr.RandomTeleportToLocations(bot, locs);
    if (!bot->IsBeingTeleported())
        return false;

    std::lock_guard<std::mutex> lock(_stayMutex);
    _sentToCity.insert(bot->GetGUID().GetCounter());
    return true;
}

bool BotPersonalityMgr::TrySendFormerCityIdlerBack(Player* bot)
{
    uint32 const guidCounter = bot->GetGUID().GetCounter();
    {
        std::lock_guard<std::mutex> lock(_stayMutex);
        if (!_sentToCity.contains(guidCounter))
            return false;
    }

    // Left the city on its own (dungeon, flight): nothing to undo.
    if (!IsInCapital(bot))
    {
        std::lock_guard<std::mutex> lock(_stayMutex);
        _sentToCity.erase(guidCounter);
        return false;
    }

    if (!CanTeleportNow(bot))
        return false;

    // Back to a quest hub of its level, like a player taking the hearthstone home.
    sRandomPlayerbotMgr.RandomTeleportGrindForLevel(bot);
    if (!bot->IsBeingTeleported())
        return false;

    std::lock_guard<std::mutex> lock(_stayMutex);
    _sentToCity.erase(guidCounter);
    _cityIdlerStays.erase(guidCounter);
    return true;
}

void BotPersonalityMgr::GetRpgWeights(Player* bot, std::array<uint32, RPG_STATUS_END>& weights,
                                      FlightDestinationOptions& flightOptions) const
{
    auto const& baseWeights = sPlayerbotAIConfig.RpgStatusProbWeight;
    for (uint32 i = 0; i < RPG_STATUS_END; ++i)
    {
        auto itr = baseWeights.find(static_cast<NewRpgStatus>(i));
        weights[i] = itr != baseWeights.end() ? itr->second : 0;
    }

    if (!sPlayerbotAIConfig.PersonalityEnabled && !sPlayerbotAIConfig.PersonalityPressureEnabled)
        return;

    if (!IsPersonalityFree(bot))
        return;

    BotPersonality const personality = GetPersonality(bot);
    if (personality != BotPersonality::None)
    {
        BotActivityProfile const profile = GetActivityProfile(bot, personality);
        for (uint32 i = 0; i < RPG_STATUS_END; ++i)
        {
            NewRpgStatus const status = static_cast<NewRpgStatus>(i);
            // A configured weight of 0 still disables the status for every bot.
            if (weights[i] == 0)
                continue;

            uint32 const sharePct = GetTimeSharePct(profile, status);
            weights[i] = sharePct ? std::max<uint32>(1, sharePct * WEIGHT_SCALE / GetExpectedSeconds(status)) : 0;
        }
    }

    uint32 const levelFactor = GetLevelFactorPct(bot->GetTeamId(), bot->GetLevel());
    if (levelFactor >= FULL_FACTOR_PCT)
        return;

    for (uint32 i = 0; i < RPG_STATUS_END; ++i)
    {
        if (weights[i] && IsLevelingStatus(static_cast<NewRpgStatus>(i)))
            weights[i] = std::max<uint32>(1, weights[i] * levelFactor / FULL_FACTOR_PCT);
    }

    // Crowded brackets also send their flights to capitals more often.
    float const baseChance = sPlayerbotAIConfig.ProbTeleToBankers;
    float const crowding = 1.0f - static_cast<float>(levelFactor) / FULL_FACTOR_PCT;
    flightOptions.capitalChance = baseChance + (1.0f - baseChance) * crowding;
}

void BotPersonalityMgr::Recalculate()
{
    bool const countCities = sPlayerbotAIConfig.PersonalityEnabled;
    bool const countLevels = sPlayerbotAIConfig.PersonalityPressureEnabled;

    std::unordered_map<uint32, uint32> cityCounts;
    std::array<std::array<uint32, MAX_TRACKED_LEVEL>, 2> levelCounts{};
    std::array<uint32, 2> teamTotals{};

    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* player = itr.second;
        if (!player || !player->IsInWorld() || !GET_PLAYERBOT_AI(player))
            continue;

        if (countCities && IsInCapital(player) && GetPersonality(player) == BotPersonality::CityIdler)
            ++cityCounts[player->GetZoneId()];

        TeamId const team = player->GetTeamId();
        if (countLevels && (team == TEAM_ALLIANCE || team == TEAM_HORDE) && sRandomPlayerbotMgr.IsRandomBot(player))
        {
            ++levelCounts[TeamIndex(team)][player->GetLevel()];
            ++teamTotals[TeamIndex(team)];
        }
    }

    {
        std::lock_guard<std::mutex> lock(_cityMutex);
        _cityIdlerCounts = std::move(cityCounts);
    }

    uint32 const minFactor = sPlayerbotAIConfig.PersonalityPressureMinFactor;
    for (TeamId team : {TEAM_ALLIANCE, TEAM_HORDE})
    {
        uint32 const teamIndex = TeamIndex(team);
        std::array<uint8, MAX_TRACKED_LEVEL> factors;
        factors.fill(FULL_FACTOR_PCT);

        uint32 const total = teamTotals[teamIndex];
        if (countLevels && total >= PRESSURE_MIN_BOTS)
        {
            for (LevelBracketConfig const& range : RandomBotLevelMgr::instance().GetWorkingRanges(team))
            {
                if (range.Lower > range.Upper)
                    continue;

                uint32 count = 0;
                for (uint32 level = range.Lower; level <= range.Upper; ++level)
                    count += levelCounts[teamIndex][level];

                uint32 factor = FULL_FACTOR_PCT;
                if (range.Pct == 0)
                    factor = count ? minFactor : FULL_FACTOR_PCT;
                else
                {
                    // Ratio of actual share to target share; above 1 means overcrowded.
                    float const ratio = (static_cast<float>(count) / total) * FULL_FACTOR_PCT / range.Pct;
                    if (ratio > 1.0f)
                        factor = std::max<uint32>(minFactor, static_cast<uint32>(std::lround(FULL_FACTOR_PCT / ratio)));
                }

                for (uint32 level = range.Lower; level <= range.Upper; ++level)
                    factors[level] = static_cast<uint8>(factor);

                LOG_DEBUG("playerbots", "[BotPersonalityMgr] {} levels {}-{}: {} of {} bots, target {}%, factor {}%",
                          team == TEAM_ALLIANCE ? "Alliance" : "Horde", range.Lower, range.Upper, count, total,
                          range.Pct, factor);
            }
        }

        for (uint32 level = 0; level < MAX_TRACKED_LEVEL; ++level)
            _levelFactorPct[teamIndex][level].store(factors[level], std::memory_order_relaxed);
    }
}

void BotPersonalityMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.PersonalityEnabled && !sPlayerbotAIConfig.PersonalityPressureEnabled)
        return;

    _updateTimer += diff;
    if (_updateTimer < sPlayerbotAIConfig.PersonalityUpdateFrequency * IN_MILLISECONDS)
        return;

    _updateTimer = 0;
    Recalculate();
}

void BotPersonalityMgr::Reset()
{
    {
        std::lock_guard<std::mutex> lock(_cityMutex);
        _cityIdlerCounts.clear();
    }
    {
        std::lock_guard<std::mutex> lock(_stayMutex);
        _cityIdlerStays.clear();
        _sentToCity.clear();
    }

    for (auto& teamFactors : _levelFactorPct)
        for (auto& factor : teamFactors)
            factor.store(FULL_FACTOR_PCT, std::memory_order_relaxed);

    // Recalculate on the next update instead of waiting a full interval.
    _updateTimer = sPlayerbotAIConfig.PersonalityUpdateFrequency * IN_MILLISECONDS;
}

class BotPersonalityWorldScript : public WorldScript
{
public:
    BotPersonalityWorldScript()
        : WorldScript("BotPersonalityWorldScript", {WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_AFTER_CONFIG_LOAD})
    {
    }

    // The initial (reload == false) call happens before PlayerbotAIConfig::Initialize() has run.
    void OnAfterConfigLoad(bool reload) override
    {
        if (!reload)
            return;

        sPlayerbotAIConfig.LoadBotPersonalityConfig();
        BotPersonalityMgr::instance().Reset();
        LOG_INFO("playerbots", "[BotPersonalityMgr] Personality config reloaded.");
    }

    void OnUpdate(uint32 diff) override { BotPersonalityMgr::instance().Update(diff); }
};

void AddSC_bot_personality_mgr() { new BotPersonalityWorldScript(); }
