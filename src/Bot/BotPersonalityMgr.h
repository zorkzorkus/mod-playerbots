/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTPERSONALITYMGR_H
#define PLAYERBOTS_BOTPERSONALITYMGR_H

#include "ObjectGuid.h"
#include "PlayerbotAIConfig.h"
#include "SharedDefines.h"
#include "TravelMgr.h"
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

class Player;
struct NewRpgInfo;

enum class BotPersonality : uint8
{
    None,
    Quester,
    Grinder,
    Traveler,
    Lazy,
    CityIdler,
    Gatherer
};

// Activity mix actually applied to a bot. Lazy, Gatherer and CityIdler bots switch between several.
enum class BotActivityProfile : uint8
{
    Quester,
    Grinder,
    Traveler,
    LazyAtCamp,
    LazyAway,
    Gatherer,
    CityIdlerInCity
};

// Scales the new RPG strategy's status weights per bot. Two independent, opt-in layers:
// - Personality: fixed per bot, derived from its GUID, so it needs no storage.
// - Level pressure: in level brackets holding more random bots than their target share, questing
//   and grinding are scaled down and flights favour capitals.
// Update() runs on the world thread and publishes results; GetRpgWeights() is safe on map threads.
class BotPersonalityMgr
{
public:
    static BotPersonalityMgr& instance()
    {
        static BotPersonalityMgr instance;

        return instance;
    }

    BotPersonality GetPersonality(Player* bot) const;
    static char const* GetPersonalityName(BotPersonality personality);
    void GetRpgWeights(Player* bot, std::array<uint32, RPG_STATUS_END>& weights,
                       FlightDestinationOptions& flightOptions) const;
    // CityIdler decisions when its RPG status becomes idle: teleport into a city, or inside one stay
    // at its spot, visit NPCs or walk to a new spot. Returns true if it set a status.
    bool HandleCityIdlerIdle(Player* bot, NewRpgInfo& info);
    // Called while a bot walks to its camp destination. Returns true for a CityIdler walking to an idle
    // spot; statusChanged is set once it has arrived and started its stay.
    bool HandleCityIdlerWalk(Player* bot, NewRpgInfo& info, WorldPosition const& spot, bool& statusChanged);

    void Update(uint32 diff);
    void Reset();

private:
    BotPersonalityMgr();
    ~BotPersonalityMgr() = default;

    BotPersonalityMgr(BotPersonalityMgr const&) = delete;
    BotPersonalityMgr& operator=(BotPersonalityMgr const&) = delete;

    BotPersonalityMgr(BotPersonalityMgr&&) = delete;
    BotPersonalityMgr& operator=(BotPersonalityMgr&&) = delete;

    static constexpr uint32 MAX_TRACKED_LEVEL = 256;
    static constexpr uint32 FULL_FACTOR_PCT = 100;

    static BotPersonality ApplyMaxLevelOverride(Player* bot, BotPersonality personality);
    static bool IsInCapital(Player* bot);
    static bool IsAtCamp(Player* bot);
    static BotActivityProfile GetActivityProfile(Player* bot, BotPersonality personality);
    uint32 GetLevelFactorPct(TeamId team, uint8 level) const;
    bool IsCityFull(uint32 zoneId) const;
    bool TrySendCityIdlerToCity(Player* bot) const;
    void StartCityIdlerStay(Player* bot, NewRpgInfo& info);
    static WorldPosition SelectCityIdleSpot(Player* bot);
    uint32 SelectCityIdlerCity(Player* bot) const;
    void Recalculate();

    // Index 0 = Alliance, 1 = Horde. Percent factor applied to questing/grinding weights per level.
    std::array<std::array<std::atomic<uint8>, MAX_TRACKED_LEVEL>, 2> _levelFactorPct;

    mutable std::mutex _cityMutex;
    std::unordered_map<uint32, uint32> _cityIdlerCounts;  // capital zone ID -> CityIdlers inside

    struct CityIdlerStay
    {
        uint32 start;
        uint32 duration;
    };
    std::mutex _stayMutex;
    std::unordered_map<ObjectGuid::LowType, CityIdlerStay> _cityIdlerStays;

    uint32 _updateTimer = 0;
};

void AddSC_bot_personality_mgr();

#endif  // PLAYERBOTS_BOTPERSONALITYMGR_H
