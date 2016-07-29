#include "ai_squad_based_team_brain.h"
#include "ai_ground_trace_cache.h"
#include "bot.h"
#include <algorithm>
#include <limits>

int CachedTravelTimesMatrix::GetAASTravelTime(const edict_t *client1, const edict_t *client2)
{
    int client1Num = client1 - game.edicts;
    int client2Num = client2 - game.edicts;

#ifdef _DEBUG
    if (client1Num <= 0 || client1Num > gs.maxclients)
    {
        AI_Debug("CachedTravelTimesMatrix", "Entity `client1` #%d is not a client\n", client1Num);
        abort();
    }
    if (client2Num <= 0 || client2Num > gs.maxclients)
    {
        AI_Debug("CachedTravelTimesMatrix", "Entity `client2` #%d is not a client\n", client2Num);
        abort();
    }
#endif
    int index = client1Num * MAX_CLIENTS + client2Num;
    if (aasTravelTimes[index] < 0)
    {
        aasTravelTimes[index] = FindAASTravelTime(client1, client2);
    }
    return aasTravelTimes[index];
}

// Can't be defined in header since Bot class is not visible in it
int CachedTravelTimesMatrix::GetAASTravelTime(const Bot *from, const Bot *to)
{
    return GetAASTravelTime(from->Self(), to->Self());
}

int CachedTravelTimesMatrix::FindAASTravelTime(const edict_t *client1, const edict_t *client2)
{
    AiGroundTraceCache *groundTraceCache = AiGroundTraceCache::Instance();

    const edict_t *clients[2] = { client1, client2 };
    vec3_t origins[2];
    int areaNums[2] = { 0, 0 };

    for (int i = 0; i < 2; ++i)
    {
        if (!groundTraceCache->TryDropToFloor(clients[i], 96.0f, origins[i]))
            return 0;
        areaNums[i] = FindAASAreaNum(origins[i]);
        if (!areaNums[i])
            return 0;
    }

    int travelFlags[2] = { client1->ai->aiRef->PreferredTravelFlags(), client1->ai->aiRef->AllowedTravelFlags() };
    for (int i = 0; i < 2; ++i)
    {
        int travelTime = AAS_AreaTravelTimeToGoalArea(areaNums[0], origins[0], areaNums[1], travelFlags[i]);
        if (travelTime)
            return travelTime;
    }

    return 0;
}

AiSquad::SquadEnemyPool::SquadEnemyPool(AiSquad *squad, float skill)
    : AiBaseEnemyPool(skill), squad(squad)
{
    std::fill_n(botRoleWeights, AiSquad::MAX_SIZE, 0.0f);
    std::fill_n(botEnemies, AiSquad::MAX_SIZE, nullptr);
}

unsigned AiSquad::SquadEnemyPool::GetBotSlot(const Bot *bot) const
{
    for (unsigned i = 0, end = squad->bots.size(); i < end; ++i)
        if (bot == squad->bots[i])
            return i;

    if (bot)
        FailWith("Can't find a slot for bot %s", bot->Tag());
    else
        FailWith("Can't find a slot for a null bot");
}

void AiSquad::SquadEnemyPool::CheckSquadValid() const
{
    if (!squad->InUse())
        FailWith("Squad %s is not in use", squad->Tag());
    if (!squad->IsValid())
        FailWith("Squad %s is not valid", squad->Tag());
}

// We have to skip ghosting bots because squads itself did not think yet when enemy pool thinks

void AiSquad::SquadEnemyPool::OnNewThreat(const edict_t *newThreat)
{
    CheckSquadValid();
    // TODO: Use more sophisticated bot selection?
    for (Bot *bot: squad->bots)
        if (!bot->IsGhosting())
            bot->OnNewThreat(newThreat, this);
}

bool AiSquad::SquadEnemyPool::CheckHasQuad() const
{
    CheckSquadValid();
    for (Bot *bot: squad->bots)
        if (!bot->IsGhosting() && ::HasQuad(bot->Self()))
            return true;
    return false;
}

bool AiSquad::SquadEnemyPool::CheckHasShell() const
{
    CheckSquadValid();
    for (Bot *bot: squad->bots)
        if (!bot->IsGhosting() && ::HasShell(bot->Self()))
            return true;
    return false;
}

float AiSquad::SquadEnemyPool::ComputeDamageToBeKilled() const
{
    CheckSquadValid();
    float result = 0.0f;
    for (Bot *bot: squad->bots)
        if (!bot->IsGhosting())
            result += DamageToKill(bot->Self());
    return result;
}

void AiSquad::SquadEnemyPool::OnEnemyRemoved(const Enemy *enemy)
{
    CheckSquadValid();
    for (Bot *bot: squad->bots)
        bot->OnEnemyRemoved(enemy);
}

void AiSquad::SquadEnemyPool::TryPushNewEnemy(const edict_t *enemy)
{
    CheckSquadValid();
    for (Bot *bot: squad->bots)
        if (!bot->IsGhosting())
            TryPushEnemyOfSingleBot(bot->Self(), enemy);
}

void AiSquad::SquadEnemyPool::SetBotRoleWeight(const edict_t *bot, float weight)
{
    CheckSquadValid();
    botRoleWeights[GetBotSlot(bot->ai->botRef)] = weight;
}

float AiSquad::SquadEnemyPool::GetAdditionalEnemyWeight(const edict_t *bot, const edict_t *enemy) const
{
    CheckSquadValid();
    if (!enemy)
        FailWith("Illegal null enemy");

    // TODO: Use something more sophisticated...

    const unsigned botSlot = GetBotSlot(bot->ai->botRef);
    float result = 0.0f;
    for (unsigned i = 0, end = squad->bots.size(); i < end; ++i)
    {
        // Do not add extra score for the own enemy
        if (botSlot == i)
            continue;
        if (botEnemies[i] && enemy == botEnemies[i]->ent)
            result += botRoleWeights[i];
    }

    return result;
}

void AiSquad::SquadEnemyPool::OnBotEnemyAssigned(const edict_t *bot, const Enemy *enemy)
{
    CheckSquadValid();
    botEnemies[GetBotSlot(bot->ai->botRef)] = enemy;
}

AiSquad::AiSquad(CachedTravelTimesMatrix &travelTimesMatrix)
    : isValid(false),
      inUse(false),
      brokenConnectivityTimeoutAt(0),
      travelTimesMatrix(travelTimesMatrix)
{
    std::fill_n(lastDroppedByBotTimestamps, MAX_SIZE, 0);
    std::fill_n(lastDroppedForBotTimestamps, MAX_SIZE, 0);

    float skillLevel = trap_Cvar_Value("sv_skilllevel"); // {0, 1, 2}
    float skill = std::min(1.0f, 0.33f * (0.1f + skillLevel + random())); // (0..1)
    // There is a clash with a getter name, thus we have to introduce a type alias
    squadEnemyPool = new (G_Malloc(sizeof(SquadEnemyPool)))SquadEnemyPool(this, skill);
}

AiSquad::AiSquad(AiSquad &&that)
    : travelTimesMatrix(that.travelTimesMatrix)
{
    isValid = that.isValid;
    inUse = that.inUse;
    canFightTogether = that.canFightTogether;
    canMoveTogether = that.canMoveTogether;
    brokenConnectivityTimeoutAt = that.brokenConnectivityTimeoutAt;
    for (Bot *bot: that.bots)
        bots.push_back(bot);

    std::fill_n(lastDroppedByBotTimestamps, MAX_SIZE, 0);
    std::fill_n(lastDroppedForBotTimestamps, MAX_SIZE, 0);

    // Move the allocated enemy pool
    this->squadEnemyPool = that.squadEnemyPool;
    // Hack! Since EnemyPool refers to `that`, modify the reference
    this->squadEnemyPool->squad = this;
    that.squadEnemyPool = nullptr;
}

AiSquad::~AiSquad()
{
    // If the enemy pool has not been moved
    if (squadEnemyPool)
    {
        squadEnemyPool->~SquadEnemyPool();
        G_Free(squadEnemyPool);
    }
}

void AiSquad::OnBotRemoved(Bot *bot)
{
    // Unused squads do not have bots. From the other hand, invalid squads may still have some bots to remove
    if (!inUse) return;

    for (auto it = bots.begin(); it != bots.end(); ++it)
    {
        if (*it == bot)
        {
            bots.erase(it);
            Invalidate();
            return;
        }
    }
}

void AiSquad::Invalidate()
{
    for (Bot *bot: bots)
        bot->OnDetachedFromSquad(this);

    isValid = false;
}

// Squad connectivity should be restored in this limit of time, otherwise a squad should be invalidated
constexpr unsigned CONNECTIVITY_TIMEOUT = 750;
// This value defines a distance limit for quick rejection of non-feasible bot pairs for new squads
constexpr float CONNECTIVITY_PROXIMITY = 500;
// This value defines summary aas move time limit from one bot to other and back
constexpr int CONNECTIVITY_MOVE_CENTISECONDS = 400;

void AiSquad::Frame()
{
    // Update enemy pool
    if (inUse && isValid)
        squadEnemyPool->Update();
}

void AiSquad::Think()
{
    if (!inUse || !isValid) return;

    for (const auto &bot: bots)
    {
        if (bot->IsGhosting())
        {
            Invalidate();
            return;
        }
    }

    canMoveTogether = CheckCanMoveTogether();
    canFightTogether = CheckCanFightTogether();

    if (canMoveTogether || canFightTogether)
        brokenConnectivityTimeoutAt = level.time + CONNECTIVITY_TIMEOUT;
    else if (brokenConnectivityTimeoutAt <= level.time)
        Invalidate();

    if (!isValid)
        return;

    UpdateBotRoleWeights();

    CheckMembersInventory();
}

bool AiSquad::CheckCanMoveTogether() const
{
    // Check whether each bot is reachable for at least a single other bot
    // or may reach at least a single other bot
    // (some reachabilities such as teleports are not reversible)
    int aasTravelTime;
    for (unsigned i = 0; i < bots.size(); ++i)
    {
        for (unsigned j = i + 1; j < bots.size(); ++j)
        {
            // Check direct travel time (it's given in seconds^-2)
            aasTravelTime = travelTimesMatrix.GetAASTravelTime(bots[i], bots[j]);
            // At least bot j is reachable from bot i, move to next bot
            if (aasTravelTime && aasTravelTime < CONNECTIVITY_MOVE_CENTISECONDS / 2)
                continue;
            // Bot j is not reachable from bot i, check travel time from j to i
            aasTravelTime = travelTimesMatrix.GetAASTravelTime(bots[j], bots[i]);
            if (!aasTravelTime || aasTravelTime >= CONNECTIVITY_MOVE_CENTISECONDS / 2)
                return false;
        }
    }
    return true;
}

bool AiSquad::CheckCanFightTogether() const
{
    // Just check that each bot is visible for each other one
    trace_t trace;
    for (unsigned i = 0; i < bots.size(); ++i)
    {
        for (unsigned j = i + 1; j < bots.size(); ++j)
        {
            edict_t *firstEnt = const_cast<edict_t*>(bots[i]->Self());
            edict_t *secondEnt = const_cast<edict_t*>(bots[j]->Self());
            G_Trace(&trace, firstEnt->s.origin, nullptr, nullptr, secondEnt->s.origin, firstEnt, MASK_AISOLID);
            if (trace.fraction != 1.0f && trace.ent != ENTNUM(secondEnt))
                return false;
        }
    }
    return true;
}

void AiSquad::UpdateBotRoleWeights()
{
    if (!inUse || !isValid)
        return;

    // Find a carrier
    bool hasCarriers = false;
    for (Bot *bot: bots)
    {
        if (!bot->IsGhosting() && IsCarrier(bot->Self()))
        {
            hasCarriers = true;
            break;
        }
    }
    if (!hasCarriers)
    {
        for (Bot *bot: bots)
            squadEnemyPool->SetBotRoleWeight(bot->Self(), 0.25f);
    }
    else
    {
        for (Bot *bot: bots)
        {
            if (!bot->IsGhosting() && IsCarrier(bot->Self()))
                squadEnemyPool->SetBotRoleWeight(bot->Self(), 1.0f);
            else
                squadEnemyPool->SetBotRoleWeight(bot->Self(), 0.0f);
        }
    }
}

static bool areWeaponDefHelpersInitialized = false;
// i-th value contains a tier for weapon #i
static int tiersForWeapon[WEAP_TOTAL];
// Contains weapons sorted by tier in descending order (best weapons first)
static int bestWeapons[WEAP_TOTAL];
// i-th value contains weapon def for weapon #i
static gs_weapon_definition_t *weaponDefs[WEAP_TOTAL];

static void InitWeaponDefHelpers()
{
    if (areWeaponDefHelpersInitialized)
        return;

    struct WeaponAndTier
    {
        int weapon, tier;
        WeaponAndTier() {}
        WeaponAndTier(int weapon, int tier): weapon(weapon), tier(tier) {}
        bool operator<(const WeaponAndTier &that) const { return tier > that.tier; }
    };

    WeaponAndTier weaponTiers[WEAP_TOTAL];

    weaponTiers[WEAP_NONE]            = WeaponAndTier(WEAP_NONE, 0);
    weaponTiers[WEAP_GUNBLADE]        = WeaponAndTier(WEAP_GUNBLADE, 0);
    weaponTiers[WEAP_GRENADELAUNCHER] = WeaponAndTier(WEAP_GRENADELAUNCHER, 1);
    weaponTiers[WEAP_RIOTGUN]         = WeaponAndTier(WEAP_RIOTGUN, 1);
    weaponTiers[WEAP_MACHINEGUN]      = WeaponAndTier(WEAP_MACHINEGUN, 2);
    weaponTiers[WEAP_PLASMAGUN]       = WeaponAndTier(WEAP_PLASMAGUN, 2);
    weaponTiers[WEAP_LASERGUN]        = WeaponAndTier(WEAP_LASERGUN, 3);
    weaponTiers[WEAP_ROCKETLAUNCHER]  = WeaponAndTier(WEAP_ROCKETLAUNCHER, 3);
    weaponTiers[WEAP_ELECTROBOLT]     = WeaponAndTier(WEAP_ELECTROBOLT, 3);
    weaponTiers[WEAP_INSTAGUN]        = WeaponAndTier(WEAP_INSTAGUN, 3);

    static_assert(WEAP_NONE == 0, "This loop assume zero lower bound");
    for (int weapon = WEAP_NONE; weapon < WEAP_TOTAL; ++weapon)
        tiersForWeapon[weapon] = weaponTiers[weapon].tier;

    std::sort(weaponTiers, weaponTiers + WEAP_TOTAL);
    for (int i = 0; i < WEAP_TOTAL; ++i)
        bestWeapons[i] = weaponTiers[i].weapon;

    for (int weapon = WEAP_NONE; weapon < WEAP_TOTAL; ++weapon)
        weaponDefs[weapon] = GS_GetWeaponDef(weapon);

    areWeaponDefHelpersInitialized = true;
}

void AiSquad::CheckMembersInventory()
{
    if (!(level.gametype.dropableItemsMask & (IT_WEAPON|IT_AMMO)))
        return;

    InitWeaponDefHelpers();

    // i-th bot has best weapon of tier maxBotWeaponTiers[i]
    int maxBotWeaponTiers[MAX_SIZE];
    std::fill_n(maxBotWeaponTiers, MAX_SIZE, 0);
    // Worst weapon tier among all squad bots
    int minBotWeaponTier = 3;

    for (unsigned botNum = 0; botNum < bots.size(); ++botNum)
    {
        for (int weapon = WEAP_GUNBLADE; weapon != WEAP_TOTAL; ++weapon)
        {
            if (bots[botNum]->Inventory()[weapon])
            {
                int weaponAmmoTag = weaponDefs[weapon]->firedef.ammo_id;
                if (weaponAmmoTag == AMMO_NONE)
                    continue;
                if (bots[botNum]->Inventory()[weaponAmmoTag] > weaponDefs[weapon]->firedef.ammo_low)
                    if (maxBotWeaponTiers[botNum] < tiersForWeapon[weapon])
                        maxBotWeaponTiers[botNum] = tiersForWeapon[weapon];
            }
        }
        if (minBotWeaponTier > maxBotWeaponTiers[botNum])
            minBotWeaponTier = maxBotWeaponTiers[botNum];
    }

    // We try to skip expensive ShouldNotDropWeaponsNow() call by doing this cheap test first
    if (minBotWeaponTier > 2)
        return;

    if (ShouldNotDropWeaponsNow())
        return;

    for (unsigned botNum = 0; botNum < bots.size(); ++botNum)
    {
        // Bot has a weapon good enough
        if (maxBotWeaponTiers[botNum] > 2)
            continue;

        // We can't set special goal immediately (a dropped entity must touch a solid first)
        // For this case, we just prevent dropping for this bot for 1000 ms
        if (level.time - lastDroppedForBotTimestamps[botNum] < 1000)
            continue;

        // Bot already has a some special goal that the squad owns
        if (bots[botNum]->HasSpecialGoal() && bots[botNum]->IsSpecialGoalSetBy(this))
            continue;

        RequestWeaponAndAmmoDrop(botNum, maxBotWeaponTiers);
    }
}

bool AiSquad::ShouldNotDropWeaponsNow() const
{
    // First, compute squad AABB
    vec3_t mins = { +999999, +999999, +999999 };
    vec3_t maxs = { -999999, -999999, -999999 };
    for (const Bot *bot: bots)
    {
        const float *origin = bot->Self()->s.origin;
        for (int i = 0; i < 3; ++i)
        {
            if (maxs[i] < origin[i])
                maxs[i] = origin[i];
            if (mins[i] > origin[i])
                mins[i] = origin[i];
        }
    }
    Vec3 squadCenter(mins);
    squadCenter += maxs;
    squadCenter *= 0.5f;

    struct PotentialStealer
    {
        const Enemy *enemy;
        Vec3 extrapolatedOrigin;
        PotentialStealer(const Enemy *enemy, const Vec3 &extrapolatedOrigin)
            : enemy(enemy), extrapolatedOrigin(extrapolatedOrigin) {}

        // Recently seen stealers should be first in a sorted list
        bool operator<(const PotentialStealer &that) const
        {
            return enemy->LastSeenAt() > that.enemy->LastSeenAt();
        }
    };

    // First reject enemies by distance
    StaticVector<PotentialStealer, AiBaseEnemyPool::MAX_TRACKED_ENEMIES> potentialStealers;
    for (const Enemy &enemy: squadEnemyPool->TrackedEnemies())
    {
        // Check whether an enemy has been invalidated and invalidation is not processed yet to prevent crash
        if (!enemy.IsValid() || G_ISGHOSTING(enemy.ent))
            continue;

        Vec3 enemyVelocityDir(enemy.LastSeenVelocity());
        float squareEnemySpeed = enemyVelocityDir.SquaredLength();
        if (squareEnemySpeed < 1)
            continue;

        float enemySpeed = 1.0f / Q_RSqrt(squareEnemySpeed);
        enemyVelocityDir *= 1.0f / enemySpeed;

        // Extrapolate last seen position but not more for 1 second
        // TODO: Test for collisions with the solid (it may be expensive)
        // If an extrapolated origin is inside solid, further trace test will treat an enemy as invisible
        float extrapolationSeconds = std::min(1.0f, 0.001f * (level.time - enemy.LastSeenAt()));
        Vec3 extrapolatedLastSeenPosition(enemy.LastSeenVelocity());
        extrapolatedLastSeenPosition *= extrapolationSeconds;
        extrapolatedLastSeenPosition += enemy.LastSeenPosition();

        Vec3 enemyToSquadCenterDir(squadCenter);
        enemyToSquadCenterDir -= extrapolatedLastSeenPosition;
        if (enemyToSquadCenterDir.SquaredLength() < 1)
        {
            potentialStealers.push_back(PotentialStealer(&enemy, extrapolatedLastSeenPosition));
            continue;
        }
        enemyToSquadCenterDir.NormalizeFast();

        float directionFactor = enemyToSquadCenterDir.Dot(enemyVelocityDir);
        if (directionFactor < 0)
        {
            if (BoundsAndSphereIntersect(mins, maxs, extrapolatedLastSeenPosition.Data(), 192.0f))
                potentialStealers.push_back(PotentialStealer(&enemy, extrapolatedLastSeenPosition));
        }
        else
        {
            float radius = 192.0f + extrapolationSeconds * enemySpeed * directionFactor;
            if (BoundsAndSphereIntersect(mins, maxs, extrapolatedLastSeenPosition.Data(), radius))
                potentialStealers.push_back(PotentialStealer(&enemy, extrapolatedLastSeenPosition));
        }
    }

    // Sort all stealers based on last seen time (recently seen first)
    std::sort(potentialStealers.begin(), potentialStealers.end());

    // Check not more than 4 most recently seen stealers.
    // Use trace instead of path travel time estimation because pathfinder may fail to find a path.
    trace_t trace;
    for (unsigned i = 0, end = std::min(4u, potentialStealers.size()); i < end; ++i)
    {
        PotentialStealer stealer = potentialStealers[i];
        for (const Bot *bot: bots)
        {
            edict_t *botEnt = const_cast<edict_t*>(bot->Self());
            G_Trace(&trace, botEnt->s.origin, nullptr, nullptr, stealer.extrapolatedOrigin.Data(), botEnt, MASK_AISOLID);
            if (trace.fraction == 1.0f || game.edicts + trace.ent == stealer.enemy->ent)
                return true;
        }
    }

    return false;
}

void AiSquad::FindSupplierCandidates(unsigned botNum, StaticVector<unsigned, AiSquad::MAX_SIZE - 1> &result) const
{
    Vec3 botVelocityDir(bots[botNum]->Self()->velocity);
    float squareBotSpeed = botVelocityDir.SquaredLength();

    // If a bot moves fast, modify score for mates depending of the bot velocity direction
    // (try to avoid stopping a fast-moving bot)
    bool applyDirectionFactor = false;
    if (squareBotSpeed > DEFAULT_PLAYERSPEED * DEFAULT_PLAYERSPEED)
    {
        botVelocityDir *= Q_RSqrt(squareBotSpeed);
        applyDirectionFactor = true;
    }

    struct BotAndScore
    {
        unsigned botNum;
        float score;
        BotAndScore(unsigned botNum, float score): botNum(botNum), score(score) {}
        bool operator<(const BotAndScore &that) const { return score > that.score; }
    };

    StaticVector<BotAndScore, MAX_SIZE - 1> candidates;
    for (unsigned thatBotNum = 0; thatBotNum < bots.size(); ++thatBotNum)
    {
        if (thatBotNum == botNum)
            continue;
        // Wait a second for next drop
        if (level.time - lastDroppedByBotTimestamps[botNum] < 1000)
            continue;

        // The lowest feasible AAS travel time is 1, 0 means that thatBot is not reachable for currBot
        int travelTime = travelTimesMatrix.GetAASTravelTime(bots[botNum], bots[thatBotNum]);
        if (!travelTime)
            continue;

        float score = 1.0f - BoundedFraction(travelTime, CONNECTIVITY_MOVE_CENTISECONDS);
        if (applyDirectionFactor)
        {
            Vec3 botToThatBot(bots[thatBotNum]->Self()->s.origin);
            botToThatBot -= bots[botNum]->Self()->s.origin;
            botToThatBot.NormalizeFast();
            score *= 0.5f + 0.5f * botToThatBot.Dot(botVelocityDir);
        }
        candidates.push_back(BotAndScore(thatBotNum, score));
    }

    // Sort mates, most suitable item suppliers first
    std::sort(candidates.begin(), candidates.end());

    // Copy sorted mates nums to result
    result.clear();
    for (BotAndScore &botAndScore: candidates)
        result.push_back(botAndScore.botNum);
}

// See definition explanation why the code is weird
void AiSquad::SetDroppedEntityAsBotGoal(edict_t *ent)
{
    const char *tag = __FUNCTION__;
    if (!ent || !ent->r.inuse)
        AI_FailWith(tag, "ent is null or not in use");

    // The target ent should be set to a bot entity
    if (!ent->target_ent || !ent->target_ent->r.inuse)
        AI_FailWith(tag, "target_ent is not set or not in use");

    // Check whether bot has been removed
    edict_t *bot = ent->target_ent;
    if (!bot->ai || !bot->ai->botRef)
        return;

    // The enemy should point to a squad ref
    AiSquad *squad = (AiSquad *)ent->enemy;
    if (!squad)
        AI_FailWith(tag, "Squad is not set");

    // Force dropped item as a special goal for the suppliant
    bot->ai->botRef->SetSpecialGoalFromEntity(ent, squad);
    // Allow other bots (and itself) to grab this item too
    // (But the suppliant has a priority since the goal has been set immediately)
    AI_AddNavEntity(ent, (ai_nav_entity_flags)(AI_NAV_REACH_AT_TOUCH | AI_NAV_DROPPED));
}

void AiSquad::RequestWeaponAndAmmoDrop(unsigned botNum, const int *maxBotWeaponTiers)
{
    StaticVector<unsigned, MAX_SIZE - 1> bestSuppliers;
    FindSupplierCandidates(botNum, bestSuppliers);

    // Should be set to a first chosen supplier's botNum
    // Further drop attempts should be made only for this bot.
    // (Items should be dropped from the same origin to be able to set a common movement goal)
    unsigned chosenSupplier = std::numeric_limits<unsigned>::max();
    // Not more than 3 items may be dropped on the same time (and by the same bot)
    int droppedItemsCount = 0;

    for (int i = 0; i < WEAP_TOTAL; ++i)
    {
        const int currWeapon = bestWeapons[i];
        const auto &fireDef = weaponDefs[currWeapon]->firedef;

        edict_t *dropped = nullptr;

        // If the bot has this weapon, check whether he needs an ammo for it
        if (bots[botNum]->Inventory()[currWeapon])
        {
            // No ammo is required, go to the next weapon
            if (fireDef.ammo_id == AMMO_NONE)
                continue;
            // Bot has enough ammo, go to the next weapon
            if (bots[botNum]->Inventory()[fireDef.ammo_id] > fireDef.ammo_low)
                continue;

            // Find who may drop an ammo
            if (level.gametype.dropableItemsMask & IT_AMMO)
            {
                if (chosenSupplier != std::numeric_limits<unsigned>::max())
                {
                    dropped = TryDropAmmo(botNum, chosenSupplier, currWeapon);
                }
                else
                {
                    for (unsigned mateNum: bestSuppliers)
                    {
                        dropped = TryDropAmmo(botNum, mateNum, currWeapon);
                        if (dropped)
                        {
                            chosenSupplier = mateNum;
                            break;
                        }
                    }
                }
            }
        }

        // Check who may drop a weapon
        if (!dropped)
        {
            if (chosenSupplier != std::numeric_limits<unsigned>::max())
            {
                dropped = TryDropWeapon(botNum, chosenSupplier, currWeapon, maxBotWeaponTiers);
            }
            else
            {
                for (unsigned mateNum: bestSuppliers)
                {
                    dropped = TryDropWeapon(botNum, mateNum, currWeapon, maxBotWeaponTiers);
                    if (dropped)
                    {
                        chosenSupplier = mateNum;
                        break;
                    }
                }
            }
        }

        if (dropped)
        {
            // If this is first dropped item, set is as a pending goal
            if (!droppedItemsCount)
            {
                dropped->target_ent = bots[botNum]->Self();
                dropped->enemy = (edict_t *)this;
                dropped->stop = AiSquad::SetDroppedEntityAsBotGoal;
                // Register drop timestamp
                lastDroppedForBotTimestamps[botNum] = level.time;
                lastDroppedByBotTimestamps[chosenSupplier] = level.time;
            }
            droppedItemsCount++;
            if (droppedItemsCount == 3)
                return;
        }
    }
}

edict_t *AiSquad::TryDropAmmo(unsigned botNum, unsigned supplierNum, int weapon)
{
    Bot *mate = bots[supplierNum];
    const auto &fireDef = weaponDefs[weapon]->firedef;
    // Min ammo quantity to be able to drop it
    float minAmmo = fireDef.ammo_pickup;
    // If mate has not only ammo but weapon, leave mate some ammo
    if (mate->Inventory()[weapon])
        minAmmo += fireDef.ammo_low;

    if (mate->Inventory()[fireDef.ammo_id] < minAmmo)
        return nullptr;

    edict_t *dropped = G_DropItem(mate->Self(), GS_FindItemByTag(fireDef.ammo_id));
    if (dropped)
        G_Say_Team(bots[supplierNum]->Self(), va("Dropped %%d at %%D for %s", Nick(bots[botNum]->Self())), false);
    return dropped;
}

edict_t *AiSquad::TryDropWeapon(unsigned botNum, unsigned supplierNum, int weapon, const int *maxBotWeaponTiers)
{
    Bot *mate = bots[supplierNum];

    // The mate does not have this weapon
    if (!mate->Inventory()[weapon])
        return nullptr;

    const auto &fireDef = weaponDefs[weapon]->firedef;
    // The mate does not have enough ammo for this weapon
    if (mate->Inventory()[fireDef.ammo_id] <= fireDef.ammo_low)
        return nullptr;

    // Compute a weapon tier of mate's inventory left after the possible drop
    int newMaxBestWeaponTier = 0;
    for (int otherWeapon = WEAP_GUNBLADE + 1; otherWeapon < WEAP_TOTAL; ++otherWeapon)
    {
        if (otherWeapon == weapon)
            continue;
        if (!mate->Inventory()[otherWeapon])
            continue;
        const auto &otherFireDef = weaponDefs[otherWeapon]->firedef;
        if (mate->Inventory()[otherFireDef.ammo_id] <= otherFireDef.ammo_low)
            continue;

        newMaxBestWeaponTier = std::max(newMaxBestWeaponTier, tiersForWeapon[otherWeapon]);
    }

    // If the does not keep its top weapon tier after dropping a weapon
    if (newMaxBestWeaponTier < maxBotWeaponTiers[supplierNum])
        return nullptr;

    // Try drop a weapon
    edict_t *dropped = G_DropItem(mate->Self(), GS_FindItemByTag(weapon));
    if (dropped)
        G_Say_Team(bots[supplierNum]->Self(), va("Dropped %%d at %%D for %s", Nick(bots[botNum]->Self())), false);
    return dropped;
}

void AiSquad::ReleaseBotsTo(StaticVector<Bot *, MAX_CLIENTS> &orphans)
{
    for (Bot *bot: bots)
        orphans.push_back(bot);

    bots.clear();
    inUse = false;
}

void AiSquad::PrepareToAddBots()
{
    isValid = true;
    inUse = true;
    canFightTogether = false;
    canMoveTogether = false;
    brokenConnectivityTimeoutAt = level.time + 1;
    bots.clear();
}

void AiSquad::AddBot(Bot *bot)
{
#ifdef _DEBUG
    if (!inUse || !isValid)
    {
        AI_Debug("AiSquad", "Can't add a bot to a unused or invalid squad\n");
        abort();
    }

    for (const Bot *presentBot: bots)
    {
        if (presentBot == bot)
        {
            AI_Debug("AiSquad", "Can't add a bot to the squad (it is already present)\n");
            abort();
        }
    }
#endif

    bots.push_back(bot);
    bot->OnAttachedToSquad(this);
}

bool AiSquad::MayAttachBot(const Bot *bot) const
{
    if (!inUse || !isValid)
        return false;
    if (bots.size() == bots.capacity())
        return false;

#ifdef _DEBUG
    // First, check all bots...
    for (Bot *presentBot: bots)
    {
        if (presentBot == bot)
        {
            AI_Debug("AiSquad", "Can't attach a bot to the squad (it is already present)\n");
            abort();
        }
    }
#endif

    for (Bot *presentBot: bots)
    {
        constexpr float squaredDistanceLimit = CONNECTIVITY_PROXIMITY * CONNECTIVITY_PROXIMITY;
        if (DistanceSquared(bot->Self()->s.origin, presentBot->Self()->s.origin) > squaredDistanceLimit)
            continue;

        int toPresentTravelTime = travelTimesMatrix.GetAASTravelTime(bot, presentBot);
        if (!toPresentTravelTime)
            continue;
        int fromPresentTravelTime = travelTimesMatrix.GetAASTravelTime(presentBot, bot);
        if (!fromPresentTravelTime)
            continue;
        if (toPresentTravelTime + fromPresentTravelTime < CONNECTIVITY_MOVE_CENTISECONDS)
            return true;
    }

    return false;
}

bool AiSquad::TryAttachBot(Bot *bot)
{
    if (MayAttachBot(bot))
    {
        AddBot(bot);
        return true;
    }
    return false;
}

void AiSquadBasedTeamBrain::Frame()
{
    // Call super method first, it may contain some logic
    AiBaseTeamBrain::Frame();

    // Drain invalid squads
    for (auto &squad: squads)
    {
        if (!squad.InUse())
            continue;
        if (squad.IsValid())
            continue;
        squad.ReleaseBotsTo(orphanBots);
    }

    // This should be called before AiSquad::Update() (since squads refer to this matrix)
    travelTimesMatrix.Clear();

    // Call squads Update() (and, thus, Frame() and, maybe, Think()) each frame as it is expected
    // even if all squad AI logic is performed only in AiSquad::Think()
    // to prevent further errors if we decide later to put some logic in Frame()
    for (auto &squad: squads)
        squad.Update();
}

void AiSquadBasedTeamBrain::OnBotAdded(Bot *bot)
{
    orphanBots.push_back(bot);
}

void AiSquadBasedTeamBrain::OnBotRemoved(Bot *bot)
{
    for (auto &squad: squads)
        squad.OnBotRemoved(bot);

    // Remove from orphans as well
    for (auto it = orphanBots.begin(); it != orphanBots.end(); ++it)
    {
        if (*it == bot)
        {
            orphanBots.erase(it);
            return;
        }
    }
}

void AiSquadBasedTeamBrain::Think()
{
    // Call super method first, this call must not be omitted
    AiBaseTeamBrain::Think();

    if (!orphanBots.empty())
        SetupSquads();
}

class NearbyMatesList;

struct NearbyBotProps
{
    Bot *bot;
    unsigned botOrphanIndex;
    NearbyMatesList *botMates;
    float distance;

    NearbyBotProps(Bot *bot, unsigned botOrphanIndex, NearbyMatesList *botMates, float distance)
        : bot(bot), botOrphanIndex(botOrphanIndex), botMates(botMates), distance(distance) {}

    bool operator<(const NearbyBotProps &that) const { return distance < that.distance; }
};

class NearbyMatesList
{
    StaticVector<NearbyBotProps, AiSquad::MAX_SIZE> mates;

    float minDistance;
public:
    unsigned botIndex;
    typedef const NearbyBotProps *const_iterator;

    NearbyMatesList(): minDistance(std::numeric_limits<float>::max()), botIndex((unsigned)-1) {}

    inline const_iterator begin() const { return &(*mates.cbegin()); }
    inline const_iterator end() const { return &(*mates.cend()); }
    inline bool empty() const { return mates.empty(); }

    void Add(const NearbyBotProps &props);

    inline bool operator<(const NearbyMatesList &that) const { return minDistance < that.minDistance; }
};

void NearbyMatesList::Add(const NearbyBotProps &props)
{
    if (mates.size() == AiSquad::MAX_SIZE)
    {
        std::pop_heap(mates.begin(), mates.end());
        mates.pop_back();
    }
    mates.push_back(props);
    std::push_heap(mates.begin(), mates.end());
    if (minDistance < props.distance)
        minDistance = props.distance;
}

static void SelectNearbyMates(NearbyMatesList *nearbyMates, StaticVector<Bot*, MAX_CLIENTS> &orphanBots,
                              CachedTravelTimesMatrix &travelTimesMatrix)
{
    for (unsigned i = 0; i < orphanBots.size(); ++i)
    {
        nearbyMates[i].botIndex = i;
        if (orphanBots[i]->IsGhosting())
            continue;

        // Always initialize mates list by an empty container
        for (unsigned j = 0; j < orphanBots.size(); ++j)
        {
            if (i == j)
                continue;
            if (orphanBots[j]->IsGhosting())
                continue;

            edict_t *firstEnt = orphanBots[i]->Self();
            edict_t *secondEnt = orphanBots[j]->Self();

            // Reject mismatching pair by doing a cheap vector distance test first
            if (DistanceSquared(firstEnt->s.origin, secondEnt->s.origin) > CONNECTIVITY_PROXIMITY * CONNECTIVITY_PROXIMITY)
                continue;

            // Check whether bots may mutually reach each other in short amount of time
            // (this means bots are not clustered across boundaries of teleports and other triggers)
            // (implementing clustering across teleports breaks cheap distance rejection)
            int firstToSecondAasTime = travelTimesMatrix.GetAASTravelTime(firstEnt, secondEnt);
            if (!firstToSecondAasTime)
                continue;
            int secondToFirstAasTime = travelTimesMatrix.GetAASTravelTime(secondEnt, firstEnt);
            if (!secondToFirstAasTime)
                continue;

            // AAS time is measured in seconds^-2
            if (firstToSecondAasTime + secondToFirstAasTime < CONNECTIVITY_MOVE_CENTISECONDS)
            {
                // Use the sum as a similar to distance thing
                float distanceLike = firstToSecondAasTime + secondToFirstAasTime;
                nearbyMates[i].Add(NearbyBotProps(orphanBots[j], j, nearbyMates + j, distanceLike));
            }
        }
    }
}

static void MakeSortedNearbyMatesLists(NearbyMatesList **sortedMatesLists, NearbyMatesList *nearbyMates, unsigned listsCount)
{
    // First, fill array of references
    for (unsigned i = 0; i < listsCount; ++i)
        sortedMatesLists[i] = nearbyMates + i;
    // Then, sort by referenced values
    auto cmp = [=](const NearbyMatesList *a, const NearbyMatesList *b) { return *a < *b; };
    std::sort(sortedMatesLists, sortedMatesLists + listsCount, cmp);
}

// For i-th orphan sets orphanSquadIds[i] to a numeric id of a new squad (that starts from 1),
// or 0 if bot has not been assigned to a new squad.
// Returns count of new squads.
static unsigned MakeNewSquads(NearbyMatesList **sortedMatesLists, unsigned listsCount, unsigned char *orphanSquadIds)
{
    static_assert(std::numeric_limits<unsigned char>::max() >= AiSquad::MAX_SIZE,
                  "Use ushort type for squads larger than 256(!) clients");
    unsigned char orphanSquadMatesCount[MAX_CLIENTS];

    std::fill_n(orphanSquadIds, listsCount, 0);
    std::fill_n(orphanSquadMatesCount, listsCount, 0);

    unsigned char newSquadsCount = 0;

    // For each bot and its mates list starting from bot that has closest teammates
    // (note that i-th list does not correspond to i-th orphan
    // after sorting but count of orphans and their lists is kept)
    for (unsigned i = 0; i < listsCount; ++i)
    {
        NearbyMatesList *matesList = sortedMatesLists[i];
        unsigned ownerOrphanIndex = matesList->botIndex;
        if (orphanSquadMatesCount[ownerOrphanIndex] >= AiSquad::MAX_SIZE - 1)
            continue;

        unsigned char squadId = orphanSquadIds[ownerOrphanIndex];

        StaticVector<unsigned, AiSquad::MAX_SIZE-1> assignedMatesOrphanIds;

        // For each bot close to the current orphan
        for (NearbyBotProps botProps: *matesList)
        {
            unsigned botOrphanIndex = botProps.botOrphanIndex;
            // Already assigned to some other squad
            if (orphanSquadMatesCount[botOrphanIndex])
                continue;

            bool areMutuallyClosest = false;
            for (NearbyBotProps thatProps: *botProps.botMates)
            {
                if (thatProps.botOrphanIndex == ownerOrphanIndex)
                {
                    areMutuallyClosest = true;
                    break;
                }
            }
            if (!areMutuallyClosest)
                continue;

            // Mutually assign orphans
            assignedMatesOrphanIds.push_back(botOrphanIndex);
            // Increase mates count of the mates list owner (the count does not include a bot itself)
            orphanSquadMatesCount[ownerOrphanIndex]++;
            const auto matesCount = orphanSquadMatesCount[ownerOrphanIndex];
            // For all already assigned mates modify their mates count
            for (unsigned orphanId: assignedMatesOrphanIds)
                orphanSquadMatesCount[orphanId] = matesCount;

            // Make new squad id only when we are sure that there are some selected squad members
            // (squad ids must be sequential and indicate feasible squads)
            if (!squadId)
                squadId = ++newSquadsCount;

            orphanSquadIds[ownerOrphanIndex] = squadId;
            orphanSquadIds[botOrphanIndex] = squadId;

            // Stop assignation of squad mates for the bot that is i-th NearbyMatesList owner
            if (matesCount == AiSquad::MAX_SIZE - 1)
                break;
        }
    }

    return newSquadsCount;
}

void AiSquadBasedTeamBrain::SetupSquads()
{
    NearbyMatesList nearbyMates[MAX_CLIENTS];

    SelectNearbyMates(nearbyMates, orphanBots, travelTimesMatrix);

    // We should start assignation from bots that have closest teammates
    // Thus, NearbyMatesList's should be sorted by minimal distance to a teammate

    // Addresses held in NearbyMatesProps should be kept stable
    // Thus, we sort not mates array itself but an array of references to these lists
    NearbyMatesList *sortedMatesLists[MAX_CLIENTS];
    MakeSortedNearbyMatesLists(sortedMatesLists, nearbyMates, orphanBots.size());

    unsigned char orphanSquadIds[MAX_CLIENTS];
    unsigned newSquadsCount = MakeNewSquads(sortedMatesLists, orphanBots.size(), orphanSquadIds);

    bool isSquadJustCreated[MAX_CLIENTS];
    std::fill_n(isSquadJustCreated, MAX_CLIENTS, false);

    for (unsigned squadId = 1; squadId <= newSquadsCount; ++squadId)
    {
        unsigned squadSlot = GetFreeSquadSlot();
        isSquadJustCreated[squadSlot] = true;
        for (unsigned i = 0; i < orphanBots.size(); ++i)
        {
            if (orphanSquadIds[i] == squadId)
                squads[squadSlot].AddBot(orphanBots[i]);
        }
    }

    StaticVector<Bot*, MAX_CLIENTS> keptOrphans;
    // For each orphan bot try attach a bot to an existing squad.
    // It a bot can't be attached, copy it to `keptOrphans`
    // (We can't modify orphanBots inplace, a logic assumes stable orphanBots indices)
    for (unsigned i = 0; i < orphanBots.size(); ++i)
    {
        // Skip just created squads
        if (orphanSquadIds[i])
            continue;

        bool attached = false;
        for (unsigned j = 0; j < squads.size(); ++j)
        {
            // Attaching a bot to a newly created squad is useless
            // (if a bot has not been included in it from the very beginning)
            if (isSquadJustCreated[j])
                continue;

            if (squads[j].TryAttachBot(orphanBots[i]))
            {
                attached = true;
                break;
            }
        }
        if (!attached)
            keptOrphans.push_back(orphanBots[i]);
    }

    // There are no `orphanBot` ops left, `orphanBots` can be modified now
    orphanBots.clear();
    for (unsigned i = 0; i < keptOrphans.size(); ++i)
        orphanBots.push_back(keptOrphans[i]);
}

unsigned AiSquadBasedTeamBrain::GetFreeSquadSlot()
{
    for (unsigned i = 0; i < squads.size(); ++i)
    {
        if (!squads[i].InUse())
        {
            squads[i].PrepareToAddBots();
            return i;
        }
    }
    squads.emplace_back(AiSquad(travelTimesMatrix));
    // This is very important action, otherwise the squad will not think
    squads.back().SetFrameAffinity(frameAffinityModulo, frameAffinityOffset);
    squads.back().PrepareToAddBots();
    return squads.size() - 1;
}

AiSquadBasedTeamBrain *AiSquadBasedTeamBrain::InstantiateTeamBrain(int team, const char *gametype)
{
    void *mem = G_Malloc(sizeof(AiSquadBasedTeamBrain));
    return new(mem) AiSquadBasedTeamBrain(team);
}