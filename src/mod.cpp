// Lizalfos Nightmare v1 - a fighting-game duelist built out of the Lizalfos's own states. Health is never touched.
//
// HOW IT WORKS
//   The whole brain runs from a PRE hook and a POST hook on the Lizalfos's own update function
//   (src/d/actor/d_a_e_dn.cpp#daE_DN_Execute). The PRE hook runs immediately before that Lizalfos's update, so
//   every field it writes is consumed by the update that follows (its hit check is the first thing that runs in
//   there). The POST hook runs right after the update and before collision resolves at the end of the frame.
//   Link is tracked once per frame in mod_update (which runs before actors) from Link's move id only: no combo
//   or cut counters are ever read.
//
//   The mod only WRITES: action, mode, timer[], invulnerability_timer, the two guard-entry fields, play speed,
//   speedF, and (POST) the attack POWER of its sword sphere. It never writes a position, a hitbox shape or health.
//   It calls no game function: the only game code that runs is the hooked update itself. If the hook service is
//   missing the mod loads and does nothing (and says so in the log).
//
// BUILD NOTE: compile-checked against BOTH header trees (upstream v2.0.2 and the kamilink fork), see
//   REPORT.md. Every number below lives in namespace cfg.
//
// 30 game ticks = 1 second. "Real" seconds below use the wall clock.

#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_e_dn.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_OPTIONAL_SERVICE(HookService, svc_hook);

DEFINE_HOOK_SYMBOL("src/d/actor/d_a_e_dn.cpp#daE_DN_Execute", int(e_dn_class*), DnExecute);

#define LZ_VERSION "Lizalfos Nightmare v1"

namespace cfg {
// ---- switches (one per risky feature) ----
constexpr bool kEnableReads = true;      // read your moves the moment you start them
constexpr bool kEnablePatterns = true;   // 16 moves / 8 dodge sides / 8 spacings of memory
constexpr bool kEnableMemory = true;     // 30 s memory of what got through, bait for what was blocked
constexpr bool kEnablePunish = true;     // counters, roll / backflip / item / stillness / turtle punishes
constexpr bool kEnableChains = true;     // chained swings, heart-loss bursts
constexpr bool kEnableCut = true;        // end its own swing's tail early, shake off hits
constexpr bool kEnableFeints = true;     // wind-up holds, feints into guard, stop-and-surge, fake retreats
constexpr bool kEnablePack = true;       // the others in range swing when one reads, blocks or is hit
constexpr bool kEnableSpeed = true;      // swing speed and movement speed
constexpr bool kEnableDamage = true;     // attack POWER of its sword sphere (not the hitbox) x6..x8
constexpr bool kEnableZoning = true;     // keep spacing just outside your reach

// ---- reading ----
constexpr float kReadChance = 0.99f;     // on every move
constexpr int kReadCooldown = 1;         // ticks
constexpr float kReadRange = 560.0f;     // ground distance
constexpr float kAirHopChance = 0.85f;   // jump strike / helm splitter / down thrust / great spin: hop away
constexpr float kSlashHopChance = 0.25f; // plain slashes: mostly shield
constexpr float kWeakHopBonus = 0.50f;   // moves that got through are dodged this much more

// ---- fight memory / bait ----
constexpr float kWeakMemorySec = 30.0f;  // a move that got through is remembered this long
constexpr int kBaitBlocks = 4;           // a move blocked this often is "abandoned" as bait...
constexpr float kBaitChance = 0.50f;     // ...with this chance, once
constexpr int kMoveHistory = 16;         // at most 16
constexpr int kDodgeHistory = 8;         // at most 8
constexpr int kSpaceHistory = 8;         // at most 8
constexpr int kPatternMax = 6;           // repeat streak at which it stops getting harder to read
constexpr float kSpaceBucket = 100.0f;   // distance bucket size for the spacing pattern

// ---- rage / heat / last stand ----
constexpr float kRageStart = 0.60f;
constexpr float kRageAfterHit = 1.00f;   // one hit takes it to full rage; rage never decays within a fight
constexpr float kFightIdleSec = 12.0f;   // no Lizalfos engaged this long = a new fight (rage resets)
constexpr float kHeatRampSec = 25.0f;    // seconds without being hit until heat is full
constexpr float kHeatLowHearts = 0.5f;   // below this fraction of hearts heat gets extra
constexpr int kLastStandHealth = 40;     // reads its health only; at or below this it is in last-stand mode

// ---- damage (attack power of its sword sphere, scaled after the game wrote it) ----
constexpr float kDamageMult = 6.0f;
constexpr float kDamageMultRage = 8.0f;  // at full rage
constexpr int kAtpMax = 120;

// ---- swing speed (animation play speed, never 0) ----
constexpr float kSwingSpeed = 2.0f;
constexpr float kSwingRageBonus = 0.6f;  // at full rage
constexpr float kSwingLastStandBonus = 0.4f;
constexpr float kSwingCap = 4.0f;        // cap on the multiplier
constexpr float kSwingRateCap = 5.5f;    // cap on frames per tick: the narrowest damage window is 6 frames wide
constexpr float kMinPlaySpeed = 0.05f;   // never exactly 0

// ---- tempo ----
constexpr int kAttackGap = 2;            // ticks between its own attacks (cap on the game's attack timer)
constexpr int kChainMax = 10;
constexpr int kBurstChainBonus = 6;
constexpr float kBurstSec = 3.0f;        // after you lose a heart: this long with no pause
constexpr float kChainRange = 460.0f;
constexpr int kChainReset = 75;          // ticks without a swing and the chain count resets
constexpr int kCounterWindow = 24;       // ticks in which it will strike back after a read / block / shake-off
constexpr float kPunishRange = 470.0f;
constexpr int kShakeTicks = 3;           // it shakes off a small hit after this many ticks (about 3 animation frames)
// frames after which the harmless tail of each swing is cut short (the hit windows are 13-18, 22-28, 25-30, 33-41)
constexpr float kCutFrameAttack0 = 30.0f;
constexpr float kCutFrameAttack = 32.0f;
constexpr float kCutFrameTail = 44.0f;

// ---- feints / bait / zoning ----
constexpr float kFeintBase = 0.20f;
constexpr float kFeintPerPattern = 0.08f;
constexpr float kFeintMax = 0.60f;
constexpr int kFeintHold = 8;            // ticks the wind-up is held (play speed kMinPlaySpeed)
constexpr float kFeintFrame = 9.0f;      // held in the first frames of the swing
constexpr float kStopSurgeChance = 0.015f; // per tick while running at you
constexpr int kStopTicks = 5;
constexpr int kSurgeTicks = 14;
constexpr float kZoneMin = 190.0f;       // closer than this with you idle: it steps back out of reach
constexpr float kZoneChance = 0.05f;     // per tick
constexpr int kStillFrames = 18;         // you stood still this many frames (mod_update calls) and it swings
constexpr float kTurtleSec = 2.2f;       // you have not attacked for this long inside its range: tail sweep
constexpr float kTurtleRange = 330.0f;

// ---- movement (absolute speeds, never stacked) ----
constexpr float kRunBase = 40.0f;        // the fork's dash speed
constexpr float kStrafeBase = 7.5f;      // its strafe hop speed
constexpr float kMoveMult = 2.5f;
constexpr float kSpeedCap = 75.0f;       // a hard cap; above this it can step through thin walls
constexpr float kBrakeMargin = 90.0f;    // never moves farther in one tick than (distance - this)
constexpr s16 kFaceTolerance = 0x1000;   // only boosts the run while it is pointing at you

// ---- pack ----
constexpr float kPackRange = 700.0f;

// ---- self-check ----
constexpr int kLayoutBadLimit = 12;
constexpr float kEngageRange = 1800.0f;
constexpr float kStatusSec = 5.0f;
constexpr float kPruneSec = 6.0f;
}  // namespace cfg

static_assert(cfg::kMoveHistory >= 1 && cfg::kMoveHistory <= 16, "move history is 1..16");
static_assert(cfg::kDodgeHistory >= 1 && cfg::kDodgeHistory <= 8, "dodge history is 1..8");
static_assert(cfg::kSpaceHistory >= 1 && cfg::kSpaceHistory <= 8, "space history is 1..8");

// ---------------------------------------------------------------------------------------------------------------
// The Lizalfos's own action numbers (the enum is file-local in d_a_e_dn.cpp, identical in both trees)
namespace lz {
constexpr s16 NORMAL = 0, FIGHT_RUN = 3, ATTACK_0 = 4, ATTACK = 5, TAIL_ATTACK = 6, GUARD = 7, JUMP = 8, DRAWBACK = 12,
              S_DAMAGE = 20, DAMAGE = 21;
}

using Clock = std::chrono::steady_clock;
static double now_sec() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

static uint32_t g_rng = 0x2F6E2B1u;
static float rand01() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (g_rng & 0xFFFFFF) / float(0x1000000);
}
static float clampf(float x, float lo, float hi) {
    return std::max(lo, std::min(hi, x));
}

// The game's own wrappers (dComIfGp_getPlayer, dComIfGp_event_runCheck, dComIfGs_getLife, dComIfGs_getMaxLife) and
// dCcD_GObjInf::ChkTgHit are declared NOINLINE, so calling them would call INTO the game. These read the same
// members straight out of the game's global data instead (g_dComIfG_gameInfo), and the collider's own inline flag
// check. Their offsets are measured in both header trees in REPORT.md.
static fopAc_ac_c* player_ptr() {
    return g_dComIfG_gameInfo.play.getPlayer(0);
}
static bool event_running() {
    return g_dComIfG_gameInfo.play.getEvent()->runCheck() != 0;
}
static int life_now() {
    return g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusA().getLife();
}
static int max_life_now() {
    return g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusA().getMaxLife();
}
static bool tg_hit(const dCcD_Sph& s) {
    return s.cCcD_ObjHitInf::ChkTgHit() != 0;
}

static float dist_xz(const fopAc_ac_c& a, const fopAc_ac_c& b) {
    const float dx = a.current.pos.x - b.current.pos.x;
    const float dz = a.current.pos.z - b.current.pos.z;
    return std::sqrt(dx * dx + dz * dz);
}
static s16 angle_to(const fopAc_ac_c& from, const fopAc_ac_c& to) {
    const float dx = to.current.pos.x - from.current.pos.x;
    const float dz = to.current.pos.z - from.current.pos.z;
    return static_cast<s16>(static_cast<int>(std::atan2(dx, dz) * (32768.0f / 3.14159265f)));
}

// ---------------------------------------------------------------------------------------------------------------
// Link, tracked from his move id only
enum class AType : uint8_t { None, Slash, Jump, Spin, Down, Head, Large, Bash };

static AType attack_type_of(u16 proc) {
    switch (proc) {
    case daAlink_c::PROC_CUT_NORMAL:
    case daAlink_c::PROC_CUT_FINISH:
    case daAlink_c::PROC_CUT_FINISH_JUMP_UP:
    case daAlink_c::PROC_CUT_REVERSE: return AType::Slash;
    case daAlink_c::PROC_CUT_JUMP: return AType::Jump;
    case daAlink_c::PROC_CUT_TURN: return AType::Spin;
    case daAlink_c::PROC_CUT_DOWN: return AType::Down;
    case daAlink_c::PROC_CUT_HEAD: return AType::Head;
    case daAlink_c::PROC_CUT_LARGE_JUMP: return AType::Large;
    case daAlink_c::PROC_GUARD_ATTACK: return AType::Bash;
    default: return AType::None;
    }
}
static bool is_air(AType t) {
    return t == AType::Jump || t == AType::Down || t == AType::Head || t == AType::Large;
}
static bool is_unblockable(AType t) {  // go around a raised shield
    return t == AType::Head || t == AType::Down;
}
static bool is_roll(u16 p) {
    return p == daAlink_c::PROC_FRONT_ROLL || p == daAlink_c::PROC_SIDE_ROLL || p == daAlink_c::PROC_ROLL_JUMP;
}
static bool is_item(u16 p) {
    return p == daAlink_c::PROC_BOW_MOVE || p == daAlink_c::PROC_BOW_SUBJECT || p == daAlink_c::PROC_BOOMERANG_MOVE ||
           p == daAlink_c::PROC_BOOMERANG_SUBJECT || p == daAlink_c::PROC_HOOKSHOT_MOVE ||
           p == daAlink_c::PROC_HOOKSHOT_SUBJECT || p == daAlink_c::PROC_HOOKSHOT_FLY;
}
static bool is_charge(u16 p) {
    return p == daAlink_c::PROC_CUT_TURN_CHARGE || p == daAlink_c::PROC_CUT_LARGE_JUMP_CHARGE;
}

enum class Dodge : uint8_t { None, Front, Left, Right, Back };

struct Link {
    bool have = false;
    u16 proc = 0;
    AType type = AType::None;
    bool attacking = false;
    // events: each has a sequence number that only ever grows, so a Lizalfos that runs at a different rate than
    // this mod can never miss one
    uint32_t attackSeq = 0, attackEndSeq = 0, rollSeq = 0, backSeq = 0, itemSeq = 0, chargeSeq = 0;
    AType lastType = AType::None;
    float lastDist = 0.0f;
    Dodge lastDodge = Dodge::None;
    int stillFrames = 0;
    double lastAttackAt = 0.0;
    int life = -1, maxLife = 0;
};
static Link g_link;

// histories
static AType g_moves[16];
static int g_movesN = 0, g_movesPos = 0;
static Dodge g_dodges[8];
static int g_dodgesN = 0, g_dodgesPos = 0;
static int g_spaces[8];
static int g_spacesN = 0, g_spacesPos = 0;
static double g_weakUntil[8];  // per AType: it got through
static int g_blocked[8];       // per AType: it was blocked

template <class T>
static int streak(const T* buf, int n, int pos, int cap) {  // how many of the newest entries equal the newest
    if (n == 0) return 0;
    const int newest = (pos + cap - 1) % cap;
    int s = 1;
    for (int i = 1; i < n; i++) {
        if (buf[(newest + cap - i) % cap] != buf[newest]) break;
        s++;
    }
    return s;
}
static int move_streak() { return streak(g_moves, g_movesN, g_movesPos, cfg::kMoveHistory); }
static int dodge_streak() { return streak(g_dodges, g_dodgesN, g_dodgesPos, cfg::kDodgeHistory); }
static int space_streak() { return streak(g_spaces, g_spacesN, g_spacesPos, cfg::kSpaceHistory); }
static int pattern_level() {
    if (!cfg::kEnablePatterns) return 0;
    const int s = std::max(move_streak(), std::max(dodge_streak(), space_streak()));
    return std::min(std::max(s - 1, 0), cfg::kPatternMax);
}

// ---------------------------------------------------------------------------------------------------------------
// fight-wide state
static float g_rage = cfg::kRageStart;
static double g_lastEngaged = 0.0;
static double g_lastLinkHit = 0.0;
static double g_burstUntil = 0.0;
static uint32_t g_alarmSeq = 0;
static bool g_off = false;       // layout self-check failed
static bool g_hooksOk = false;
static int g_good = 0, g_bad = 0;
static double g_nextStatus = 0.0, g_nextPrune = 0.0;

struct Stats {
    int read = 0, readMiss = 0, guard = 0, hop = 0, weakHop = 0, bait = 0;
    int counter = 0, rollPunish = 0, backPunish = 0, itemPunish = 0, chargePunish = 0, stillPunish = 0, turtle = 0;
    int pattMove = 0, pattDodge = 0, pattSpace = 0;
    int feint = 0, feintCancel = 0, fakeRetreat = 0, surge = 0, zone = 0;
    int chain = 0, burst = 0, cut = 0, shake = 0, pack = 0, lastStand = 0, scaled = 0, sped = 0;
};
static Stats g_st;

struct Mem {
    double lastSeen = 0.0;
    int t = 0;
    s16 prevAction = -1;
    uint32_t seenAttack = 0, seenEnd = 0, seenRoll = 0, seenBack = 0, seenItem = 0, seenCharge = 0, seenAlarm = 0;
    int readCd = 0, counterArm = 0, counterTail = 0, chain = 0, sinceAtk = 999, punishCd = 0;
    int feintHold = 0, stopTicks = 0, surgeTicks = 0, afterHop = 0, shakeT = 0, zoneCd = 0;
    bool feinting = false;
    float baseRate = 1.0f;
    int baseAtp = 0, scaledAtp = 0;
};
static std::unordered_map<const e_dn_class*, Mem> g_mem;

enum class St { Other, Ready, Attack, Guard, Shake, Hop };
static St classify(const e_dn_class* d) {
    switch (d->action) {
    case lz::FIGHT_RUN: return St::Ready;
    case lz::ATTACK_0:
    case lz::ATTACK:
    case lz::TAIL_ATTACK: return St::Attack;
    case lz::GUARD: return St::Guard;
    case lz::S_DAMAGE: return St::Shake;
    case lz::DRAWBACK: return St::Hop;
    default: return St::Other;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// forced states. Each one is the entry the game itself uses (see REPORT.md for the line-by-line proof).
static void force_guard(e_dn_class* d, bool hop) {  // d_a_e_dn.cpp fight_run -> ACTION_GUARD
    d->action = lz::GUARD;
    d->mode = hop ? 0 : 5;
    d->field_0x708 = 30.0f;
    d->field_0x70c = d->actor.shape_angle.y;
    d->timer[0] = 0;
    d->invulnerability_timer = 15;
}
static void force_hop(e_dn_class* d) {  // ACTION_DRAWBACK mode 0 (sets its own invulnerability)
    d->action = lz::DRAWBACK;
    d->mode = 0;
}
static void force_attack(e_dn_class* d, s16 which) {  // ACTION_ATTACK_0 / ATTACK / TAIL_ATTACK mode 0
    d->action = which;
    d->mode = 0;
}
static void force_run(e_dn_class* d) {  // ACTION_FIGHT_RUN mode 0
    d->action = lz::FIGHT_RUN;
    d->mode = 0;
}
static s16 pick_attack(float dist, int variety) {
    if (variety == 1) return lz::TAIL_ATTACK;
    return dist < 300.0f ? lz::ATTACK_0 : lz::ATTACK;
}

// ---------------------------------------------------------------------------------------------------------------
// frame-level helpers
static float swing_mult(bool lastStand) {
    const float rageN = clampf((g_rage - cfg::kRageStart) / (1.0f - cfg::kRageStart), 0.0f, 1.0f);
    float m = cfg::kSwingSpeed + cfg::kSwingRageBonus * rageN;
    if (lastStand) m += cfg::kSwingLastStandBonus;
    return std::min(m, cfg::kSwingCap);
}
static float damage_mult() {
    const float rageN = clampf((g_rage - cfg::kRageStart) / (1.0f - cfg::kRageStart), 0.0f, 1.0f);
    return cfg::kDamageMult + (cfg::kDamageMultRage - cfg::kDamageMult) * rageN;
}
static float heat_now() {
    const double now = now_sec();
    float h = clampf(float((now - g_lastLinkHit) / cfg::kHeatRampSec), 0.0f, 1.0f);
    if (g_link.maxLife > 0 && g_link.life >= 0) {
        const float frac = float(g_link.life) / float(g_link.maxLife);
        if (frac < cfg::kHeatLowHearts) h = clampf(h + (cfg::kHeatLowHearts - frac) * 2.0f, 0.0f, 1.0f);
    }
    return h;
}

static void raise_alarm(Mem& m) {
    if (!cfg::kEnablePack) return;
    g_alarmSeq++;
    m.seenAlarm = g_alarmSeq;  // the one that raised it does not answer itself
}

// ---------------------------------------------------------------------------------------------------------------
// The brain, called from the PRE hook: runs immediately before this Lizalfos's own update.
static void brain(e_dn_class* d) {
    if (d->status != 0) return;
    fopAc_ac_c* plrp = player_ptr();
    if (plrp == nullptr || event_running()) return;
    const fopAc_ac_c& plr = *plrp;
    fopEn_enemy_c& me = d->actor;

    // layout self-check: a real Lizalfos has a small action number, a small mode and a model
    const bool sane = d->action >= 0 && d->action <= 64 && d->mode >= -16 && d->mode <= 64 && d->anm_p != nullptr;
    if (sane) {
        g_good++;
    } else {
        g_bad++;
        return;  // never touch an actor whose state numbers are not real
    }
    if (me.health <= 0 || d->death_flag != 0) return;

    const float dist = dist_xz(me, plr);
    if (dist > cfg::kEngageRange) return;
    g_link.lastDist = dist;  // the spacing pattern uses the last Lizalfos the brain measured

    const double now = now_sec();
    if (now - g_lastEngaged > cfg::kFightIdleSec) {  // a new fight: rage starts over, the old chain is forgotten
        g_rage = cfg::kRageStart;
    }
    g_lastEngaged = now;

    Mem& m = g_mem[d];
    if (m.t == 0) {  // first tick we see it: start level with every event counter
        m.seenAttack = g_link.attackSeq;
        m.seenEnd = g_link.attackEndSeq;
        m.seenRoll = g_link.rollSeq;
        m.seenBack = g_link.backSeq;
        m.seenItem = g_link.itemSeq;
        m.seenCharge = g_link.chargeSeq;
        m.seenAlarm = g_alarmSeq;
    }
    m.lastSeen = now;
    m.t++;
    if (m.readCd > 0) m.readCd--;
    if (m.punishCd > 0) m.punishCd--;
    if (m.counterArm > 0) m.counterArm--;
    if (m.zoneCd > 0) m.zoneCd--;
    m.sinceAtk++;
    if (m.sinceAtk > cfg::kChainReset) m.chain = 0;

    const St st = classify(d);
    const bool actionChanged = d->action != m.prevAction;
    const s16 prevAction = m.prevAction;
    m.prevAction = d->action;
    const bool lastStand = me.health <= cfg::kLastStandHealth;
    if (lastStand) g_st.lastStand++;

    // ---- what the last collision pass did to it (flags set at the end of the previous frame) ----
    bool gotHit = false, blocked = false;
    for (int i = 0; i < 3; i++) {
        if (tg_hit(d->cc_sph[i])) gotHit = true;
    }
    if (tg_hit(d->tate_sph)) blocked = true;
    if (gotHit) {
        g_rage = std::max(g_rage, cfg::kRageAfterHit);
        if (cfg::kEnableMemory) g_weakUntil[static_cast<int>(g_link.lastType)] = now + cfg::kWeakMemorySec;
        m.counterArm = cfg::kCounterWindow;  // after it shakes the hit off, it strikes back
        raise_alarm(m);
    }
    if (blocked) {
        if (cfg::kEnableMemory) g_blocked[static_cast<int>(g_link.lastType)]++;
        m.counterArm = cfg::kCounterWindow;
        raise_alarm(m);
    }

    // ---- bookkeeping when its state just changed ----
    if (actionChanged) {
        m.feinting = false;
        if (st == St::Attack) {
            m.sinceAtk = 0;
            m.chain++;
            m.baseRate = 0.0f;  // re-read the game's rate for this swing
            g_st.chain += (m.chain > 1) ? 1 : 0;
            // a feint: hold the wind-up. Chance grows with every repeated pattern of yours.
            if (cfg::kEnableFeints && !g_link.attacking) {
                const float p = std::min(cfg::kFeintMax, cfg::kFeintBase + cfg::kFeintPerPattern * pattern_level());
                if (rand01() < p) {
                    m.feintHold = cfg::kFeintHold;
                    m.feinting = true;
                    g_st.feint++;
                }
            }
        }
        if (prevAction == lz::DRAWBACK && st == St::Ready && m.afterHop > 0) {
            // fake retreat: the back-hop ends and it comes straight back with a swing
            if (cfg::kEnableFeints && dist < cfg::kPunishRange && m.punishCd == 0) {
                force_attack(d, pick_attack(dist, 0));
                m.punishCd = 6;
                m.afterHop = 0;
                g_st.fakeRetreat++;
                return;
            }
        }
        if (st != St::Hop && st != St::Ready) m.afterHop = 0;
    }

    // ---- Link's events ----
    const Link& L = g_link;
    const bool newAttack = m.seenAttack != L.attackSeq;
    const bool newEnd = m.seenEnd != L.attackEndSeq;
    const bool newRoll = m.seenRoll != L.rollSeq;
    const bool newBack = m.seenBack != L.backSeq;
    const bool newItem = m.seenItem != L.itemSeq;
    const bool newCharge = m.seenCharge != L.chargeSeq;
    m.seenAttack = L.attackSeq;
    m.seenEnd = L.attackEndSeq;
    m.seenRoll = L.rollSeq;
    m.seenBack = L.backSeq;
    m.seenItem = L.itemSeq;
    m.seenCharge = L.chargeSeq;

    const bool canAct = (st == St::Ready) ||
                      (st == St::Guard) ||
                      (st == St::Attack && (m.feinting || d->anm_p->getFrame() >= 30.0f));  // an attack in its tail

    // PACK: another Lizalfos read, blocked or got hit: swing at once
    if (cfg::kEnablePack && m.seenAlarm != g_alarmSeq) {
        m.seenAlarm = g_alarmSeq;
        if (st == St::Ready && dist < cfg::kPackRange && m.punishCd == 0) {
            force_attack(d, pick_attack(dist, rand01() < 0.3f ? 1 : 0));
            m.punishCd = 8;
            g_st.pack++;
            return;
        }
    }

    // READ: you started an attack. It answers with shield or hop before the blade is out.
    if (cfg::kEnableReads && newAttack && m.readCd == 0 && canAct && dist < cfg::kReadRange) {
        m.readCd = cfg::kReadCooldown;
        const AType t = L.lastType;
        const int ti = static_cast<int>(t);

        // fight memory: a move that was blocked too often is dropped as bait; it lets one land, then strikes
        if (cfg::kEnableMemory && g_blocked[ti] >= cfg::kBaitBlocks && rand01() < cfg::kBaitChance) {
            g_blocked[ti] = 0;
            m.counterArm = cfg::kCounterWindow;
            g_st.bait++;
        } else if (rand01() < cfg::kReadChance) {
            g_st.read++;
            const int level = pattern_level();
            if (move_streak() >= 2) g_st.pattMove++;
            if (space_streak() >= 2) g_st.pattSpace++;
            const bool weak = cfg::kEnableMemory && now < g_weakUntil[ti];
            float hopP = is_air(t) ? cfg::kAirHopChance : cfg::kSlashHopChance;
            if (weak) hopP = std::min(1.0f, hopP + cfg::kWeakHopBonus);
            if (is_unblockable(t)) hopP = 1.0f;  // a shield does not stop these
            // a repeated move gets the exact counter: slashes are shielded (then swept), air moves hopped (then hit)
            if (level >= 2 && !is_unblockable(t)) hopP = is_air(t) ? 1.0f : 0.0f;
            m.counterTail = (t == AType::Slash || t == AType::Bash) ? 1 : 0;
            if (rand01() < hopP) {
                force_hop(d);
                m.afterHop = 20;
                g_st.hop++;
                if (weak) g_st.weakHop++;
            } else {
                force_guard(d, is_air(t));
                g_st.guard++;
            }
            m.counterArm = cfg::kCounterWindow;
            raise_alarm(m);
            return;
        } else {
            g_st.readMiss++;
        }
    }

    // while you keep attacking it keeps the guard up: no streak count ever drops it
    if (st == St::Guard && d->mode == 7 && (L.attacking || (now - L.lastAttackAt) < 0.35)) {
        d->timer[0] = 5;
    }

    // PUNISH: everything you do that is not safe
    if (cfg::kEnablePunish && m.punishCd == 0 && dist < cfg::kPunishRange) {
        const bool settled = st == St::Ready || (st == St::Guard && (d->mode >= 7)) ||
                             (st == St::Attack && m.feinting);
        // after your swing ends (you missed or were blocked), after a block, after a shake-off: 100%
        if (newEnd && settled) m.counterArm = std::max(m.counterArm, cfg::kCounterWindow);
        if (m.counterArm > 0 && !L.attacking && settled) {
            force_attack(d, pick_attack(dist, m.counterTail));
            m.counterArm = 0;
            m.punishCd = 6;
            g_st.counter++;
            return;
        }
        if (newRoll && settled) {
            if (dodge_streak() >= 2) g_st.pattDodge++;
            force_attack(d, pick_attack(dist, 0));
            m.punishCd = 6;
            g_st.rollPunish++;
            return;
        }
        if (newBack && settled) {  // backflip: it follows with a strike
            force_attack(d, lz::ATTACK);
            m.punishCd = 6;
            g_st.backPunish++;
            return;
        }
        if (newCharge && settled) {  // spin charge: it interrupts the charge
            force_attack(d, lz::ATTACK_0);
            m.punishCd = 6;
            g_st.chargePunish++;
            return;
        }
        if (newItem && settled) {  // bow / boomerang / hookshot: close in (and swing) or shield the arrow
            if (dist < 360.0f) {
                force_attack(d, pick_attack(dist, 0));
            } else {
                force_guard(d, false);
            }
            m.punishCd = 6;
            g_st.itemPunish++;
            return;
        }
        if (st == St::Ready && L.stillFrames >= cfg::kStillFrames && !L.attacking) {
            force_attack(d, pick_attack(dist, 0));
            m.punishCd = 10;
            g_st.stillPunish++;
            return;
        }
        // shield-up turtling or hiding: the tail sweep goes around a shield
        if (st == St::Ready && dist < cfg::kTurtleRange && !L.attacking &&
            (now - L.lastAttackAt) > cfg::kTurtleSec) {
            force_attack(d, lz::TAIL_ATTACK);
            m.punishCd = 12;
            g_st.turtle++;
            return;
        }
    }

    // ---- shake off small hits so a combo can never loop on it ----
    if (cfg::kEnableCut && st == St::Shake) {
        m.shakeT++;
        if (m.shakeT >= cfg::kShakeTicks) {
            m.shakeT = 0;
            force_run(d);
            m.counterArm = cfg::kCounterWindow;
            g_st.shake++;
            return;
        }
    } else {
        m.shakeT = 0;
    }

    // ---- its own swing ----
    if (st == St::Attack) {
        const float frame = d->anm_p->getFrame();
        // feint: hold the wind-up at the minimum play speed; if you swing into the hold it drops into guard
        if (m.feinting && m.feintHold > 0 && frame < cfg::kFeintFrame + 6.0f) {
            if (newAttack || L.attacking) {
                m.feinting = false;
                m.feintHold = 0;
                d->anm_p->setPlaySpeed(m.baseRate > 0.0f ? m.baseRate : 1.0f);
                force_guard(d, false);
                g_st.feintCancel++;
                g_st.guard++;
                return;
            }
            if (frame >= cfg::kFeintFrame) {
                if (m.baseRate == 0.0f && d->mode >= 1) m.baseRate = std::max(d->anm_p->getPlaySpeed(), cfg::kMinPlaySpeed);
                d->anm_p->setPlaySpeed(cfg::kMinPlaySpeed);
                m.feintHold--;
            }
        } else if (m.feinting) {
            m.feinting = false;  // hold over: restore the game's rate (re-scaled below)
            if (m.baseRate > 0.0f) d->anm_p->setPlaySpeed(m.baseRate);
        }
        // swing speed: the game's own rate for this swing, scaled by an absolute factor (never compounding)
        if (cfg::kEnableSpeed && !m.feinting && d->mode >= 1) {
            if (m.baseRate == 0.0f) m.baseRate = std::max(d->anm_p->getPlaySpeed(), cfg::kMinPlaySpeed);
            const float want = std::min(m.baseRate * swing_mult(lastStand), cfg::kSwingRateCap);
            d->anm_p->setPlaySpeed(std::max(want, cfg::kMinPlaySpeed));
            g_st.sped++;
        }
        // cut the harmless tail so it can chain the next swing
        const float cutFrame = d->action == lz::ATTACK_0 ? cfg::kCutFrameAttack0
                               : d->action == lz::ATTACK  ? cfg::kCutFrameAttack
                                                          : cfg::kCutFrameTail;
        if (cfg::kEnableCut && !m.feinting && frame >= cutFrame) {
            force_run(d);
            g_st.cut++;
            // chain: straight into the next swing while you are in range
            const int maxChain = lastStand ? 99 : cfg::kChainMax + ((now < g_burstUntil) ? cfg::kBurstChainBonus : 0);
            if (cfg::kEnableChains && m.chain < maxChain && dist < cfg::kChainRange) {
                static const s16 order[3] = {lz::ATTACK_0, lz::ATTACK, lz::TAIL_ATTACK};
                force_attack(d, order[(m.chain + pattern_level()) % 3]);
                g_st.chain++;
            }
            return;
        }
    }

    // ---- tempo: its attack timer is cut to almost nothing (burst and heat make it nothing at all) ----
    if (st == St::Ready) {
        int gap = cfg::kAttackGap;
        if (now < g_burstUntil || lastStand || heat_now() > 0.9f) gap = 0;
        if (d->timer[2] > gap) d->timer[2] = static_cast<s16>(gap);
    }

    // ---- zoning: it keeps just out of your reach while you are idle, and enters when you commit ----
    if (cfg::kEnableZoning && st == St::Ready && m.zoneCd == 0 && !L.attacking && dist > 1.0f && dist < cfg::kZoneMin &&
        rand01() < cfg::kZoneChance) {
        force_hop(d);
        m.zoneCd = 40;
        m.afterHop = 0;
        g_st.zone++;
        return;
    }

    // ---- movement (absolute speeds): the run and the strafe hops, only while it faces you ----
    if (cfg::kEnableSpeed && st == St::Ready && d->mode >= 0 && d->mode <= 2) {
        const s16 faceErr = static_cast<s16>(angle_to(me, plr) - me.current.angle.y);
        if (std::abs(static_cast<int>(faceErr)) < cfg::kFaceTolerance) {
            // stop-and-surge: it stops dead for a moment, then surges
            if (cfg::kEnableFeints && m.stopTicks == 0 && m.surgeTicks == 0 && d->mode <= 1 &&
                rand01() < cfg::kStopSurgeChance) {
                m.stopTicks = cfg::kStopTicks;
            }
            if (m.stopTicks > 0) {
                m.stopTicks--;
                me.speedF = 0.0f;
                if (m.stopTicks == 0) m.surgeTicks = cfg::kSurgeTicks;
            } else {
                float v = 0.0f;
                if (d->mode <= 1) {
                    v = cfg::kRunBase * cfg::kMoveMult;
                } else if (me.speedF > 0.5f) {
                    v = cfg::kStrafeBase * cfg::kMoveMult;
                }
                if (m.surgeTicks > 0) {
                    m.surgeTicks--;
                    v = cfg::kSpeedCap;
                    g_st.surge++;
                }
                if (v > 0.0f) {
                    v = std::min(v, cfg::kSpeedCap);
                    v = std::min(v, std::max(0.0f, dist - cfg::kBrakeMargin));
                    if (v > me.speedF) {
                        me.speedF = v;
                    }
                }
            }
        }
    }
}

// POST hook: runs right after its update, before collision resolves. The sword sphere's power was just written
// by the game (1 or 2); scale it here. The game rewrites it every frame, so this never compounds.
static void after(e_dn_class* d) {
    if (!cfg::kEnableDamage) return;
    auto it = g_mem.find(d);
    if (it == g_mem.end()) return;
    Mem& m = it->second;
    const int cur = d->at_sph.GetAtAtp();
    if (cur != m.scaledAtp || m.scaledAtp == 0) {
        m.baseAtp = cur;  // the game wrote a fresh value this frame
    }
    if (m.baseAtp <= 0) return;
    const int want = std::min(cfg::kAtpMax, std::max(1, static_cast<int>(std::lround(m.baseAtp * damage_mult()))));
    d->at_sph.SetAtAtp(want);
    m.scaledAtp = want;
    g_st.scaled++;
}

static HookAction on_pre(ModContext*, void* args, void*, void*) {
    if (g_off) return HOOK_CONTINUE;
    e_dn_class* d = mods::arg<e_dn_class*>(args, 0);
    if (d == nullptr) return HOOK_CONTINUE;
    brain(d);
    if (g_bad >= cfg::kLayoutBadLimit && g_bad * 3 > g_good) {
        g_off = true;
        mods::log::error(
            "lizalfos_nightmare: LAYOUT MISMATCH (bad={} good={}): the game does not match the headers this was "
            "built against. The mod is switched OFF.",
            g_bad, g_good);
    }
    return HOOK_CONTINUE;
}

static void on_post(ModContext*, void* args, void*, void*) {
    if (g_off) return;
    e_dn_class* d = mods::arg<e_dn_class*>(args, 0);
    if (d != nullptr && d->status == 0) after(d);
}

// ---------------------------------------------------------------------------------------------------------------
// Link tracking, once per frame, before any actor runs.
static void track_link() {
    fopAc_ac_c* plrp = player_ptr();
    if (plrp == nullptr) {
        g_link.have = false;
        return;
    }
    auto* link = static_cast<daAlink_c*>(plrp);
    Link& L = g_link;
    const u16 proc = link->mProcID;
    if (!L.have) {
        L.have = true;
        L.proc = proc;
        L.type = attack_type_of(proc);
        L.attacking = L.type != AType::None;
        return;
    }
    const AType type = attack_type_of(proc);
    const bool changed = proc != L.proc;
    const double now = now_sec();

    if (changed && type != AType::None) {
        L.attackSeq++;
        L.lastType = type;
        L.lastAttackAt = now;
        g_moves[g_movesPos] = type;
        g_movesPos = (g_movesPos + 1) % cfg::kMoveHistory;
        g_movesN = std::min(g_movesN + 1, cfg::kMoveHistory);
        // spacing at the moment you commit: distance to the nearest Lizalfos is taken in the brain; here we use
        // the last one the brain measured
        g_spaces[g_spacesPos] = static_cast<int>(L.lastDist / cfg::kSpaceBucket);
        g_spacesPos = (g_spacesPos + 1) % cfg::kSpaceHistory;
        g_spacesN = std::min(g_spacesN + 1, cfg::kSpaceHistory);
    }
    if (L.attacking && type == AType::None) {
        L.attackEndSeq++;
    }
    if (changed && is_roll(proc)) {
        L.rollSeq++;
        Dodge dd = Dodge::Front;
        if (proc == daAlink_c::PROC_SIDE_ROLL) {
            const s16 d = static_cast<s16>(link->current.angle.y - link->shape_angle.y);
            dd = d > 0 ? Dodge::Right : Dodge::Left;
        }
        L.lastDodge = dd;
        g_dodges[g_dodgesPos] = dd;
        g_dodgesPos = (g_dodgesPos + 1) % cfg::kDodgeHistory;
        g_dodgesN = std::min(g_dodgesN + 1, cfg::kDodgeHistory);
    }
    if (changed && (proc == daAlink_c::PROC_BACK_JUMP || proc == daAlink_c::PROC_SIDESTEP)) {
        L.backSeq++;
        const Dodge dd = proc == daAlink_c::PROC_BACK_JUMP ? Dodge::Back : Dodge::Left;
        g_dodges[g_dodgesPos] = dd;
        g_dodgesPos = (g_dodgesPos + 1) % cfg::kDodgeHistory;
        g_dodgesN = std::min(g_dodgesN + 1, cfg::kDodgeHistory);
    }
    if (changed && is_item(proc)) L.itemSeq++;
    if (changed && is_charge(proc)) L.chargeSeq++;

    // standing still: no horizontal speed and a waiting move id
    if ((proc == daAlink_c::PROC_WAIT) && link->speedF < 0.5f) {
        L.stillFrames++;
    } else {
        L.stillFrames = 0;
    }

    // hearts
    const int life = life_now();
    L.maxLife = max_life_now();
    if (L.life >= 0 && life < L.life) {
        g_lastLinkHit = now;
        if (cfg::kEnableChains) {
            g_burstUntil = now + cfg::kBurstSec;
            g_st.burst++;
        }
    }
    L.life = life;

    L.proc = proc;
    L.type = type;
    L.attacking = type != AType::None;
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    g_hooksOk = false;
    if (svc_hook != nullptr) {
        const ModResult a = mods::hook::add_pre<DnExecute>(on_pre);
        const ModResult b = mods::hook::add_post<DnExecute>(on_post);
        g_hooksOk = (a == MOD_OK && b == MOD_OK);
    }
    mods::log::info(
        LZ_VERSION " initialized | hooks {} | read {:.0f}% | damage x{:.0f}..x{:.0f} | swing x{:.1f}..x{:.1f} | "
        "run x{:.1f} | rage {:.0f}% -> {:.0f}% | status line every {:.0f} s",
        g_hooksOk ? "ON" : "UNAVAILABLE (the mod will do nothing)", cfg::kReadChance * 100.0f, cfg::kDamageMult,
        cfg::kDamageMultRage, cfg::kSwingSpeed, cfg::kSwingSpeed + cfg::kSwingRageBonus, cfg::kMoveMult,
        cfg::kRageStart * 100.0f, cfg::kRageAfterHit * 100.0f, cfg::kStatusSec);
    g_nextStatus = now_sec() + cfg::kStatusSec;
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    if (!g_hooksOk || g_off) return MOD_OK;
    track_link();
    const double now = now_sec();
    if (now >= g_nextPrune) {
        g_nextPrune = now + 2.0;
        for (auto it = g_mem.begin(); it != g_mem.end();) {
            it = (now - it->second.lastSeen > cfg::kPruneSec) ? g_mem.erase(it) : std::next(it);
        }
    }
    if (now >= g_nextStatus) {
        g_nextStatus = now + cfg::kStatusSec;
        if (now - g_lastEngaged < cfg::kStatusSec * 2) {
            const Stats& s = g_st;
            mods::log::info(
                LZ_VERSION ": liz={} rage={:.0f}% heat={:.0f}% | read ok={} miss={} | shield={} hop={} weak-hop={} "
                "bait={} | pattern move/dodge/space={}/{}/{} (level {}) | counter={} roll={} backflip={} item={} "
                "charge={} still={} turtle={} | feint={} cancel={} fake-retreat={} surge={} zone={} | chain={} "
                "burst={} cut={} shake={} pack={} last-stand-ticks={} | swing-scaled={} dmg-scaled={} | layout={}/{}",
                g_mem.size(), g_rage * 100.0f, heat_now() * 100.0f, s.read, s.readMiss, s.guard, s.hop, s.weakHop,
                s.bait, s.pattMove, s.pattDodge, s.pattSpace, pattern_level(), s.counter, s.rollPunish, s.backPunish,
                s.itemPunish, s.chargePunish, s.stillPunish, s.turtle, s.feint, s.feintCancel, s.fakeRetreat, s.surge,
                s.zone, s.chain, s.burst, s.cut, s.shake, s.pack, s.lastStand, s.sped, s.scaled, g_good, g_bad);
            g_st = Stats{};
        }
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_mem.clear();
    g_link = Link{};
    g_movesN = g_dodgesN = g_spacesN = 0;
    g_movesPos = g_dodgesPos = g_spacesPos = 0;
    for (double& w : g_weakUntil) w = 0.0;
    for (int& b : g_blocked) b = 0;
    g_rage = cfg::kRageStart;
    g_alarmSeq = 0;
    g_off = false;
    g_good = g_bad = 0;
    g_st = Stats{};
    return MOD_OK;
}
}
