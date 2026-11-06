// Darknut Ruthless v12 (agile armor-off duelist, 7 more tactics; earlier notes below are from v1): makes every Darknut (the Temple of Time mini-boss and the Hyrule Castle ones) read
// your attacks, block or dodge them, and punish every miss. Nothing else in the game is touched, and the
// Darknut's health is never changed (it keeps its normal armor-break / damage rules).
//
// How it "reads" you:
//   * The moment you PRESS an attack (Link's move id changes), every Darknut that can see it rolls to read it
//     (82% to start, up to 96% when you keep repeating the same move). On a read it snaps to face you, raises
//     its shield from every angle for about 0.8 s, and (armor-off Darknut) may instead leap back or sideways
//     using its own dodge jumps. It uses the game's own guard / dodge states, so animations and sounds are
//     the normal ones.
//   * After 4 blocks in a row it drops its guard for about 1.2 s. That is your opening: the mod stays
//     beatable. Landing any hit resets its streak. (Change kStreakMax / kExposeTicks to taste.)
//   * Counter: when your swing ends, it strikes back within ~0.15 s, then chains up to 2 more swings
//     without waiting for its recovery. It also attacks on its own whenever you are in reach and not
//     swinging, closes distance with its leap and walks/circles faster.
//   * It keeps its own rules for the things only a skilled player can do: the 4th hit of a combo, shield
//     bashes and the helm splitter still work as in the normal game when it is exposed.
//
// Logs: the first line at start-up says whether the two game hooks installed. While a Darknut is near, a status
// line appears every 5 seconds in the Logs window. 30 ticks = 1 second. All tunables are in namespace cfg.

#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

#include "SSystem/SComponent/c_cc_d.h"
#include "SSystem/SComponent/c_cc_s.h"
#include "d/d_cc_d.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_b_tn.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_OPTIONAL_SERVICE(HookService, svc_hook);  // optional: without it the mod runs without dodge jumps / combo-finisher block

namespace cfg {
// ---- Reading your moves ----
constexpr float kReadBase = 0.92f;          // chance to read a move it has no history for
constexpr float kReadHabitBonus = 0.12f;    // extra chance when you keep repeating one move
constexpr float kReadMax = 0.99f;
constexpr float kReadRange = 520.0f;        // it reads swings started within this distance of it
constexpr float kReadRangeWide = 600.0f;    // spins, jump strikes, down thrust, helm splitter
constexpr float kReadLinkFacingCos = -0.1f; // narrow moves: you must be swinging roughly toward it
constexpr int kReadCooldown = 5;
constexpr float kRecoveryReadMult = 0.95f;  // reading while it is still recovering from its own swing
constexpr int kArmTicks = 24;               // how long a read keeps the guard up (0.8 s)
constexpr int kStreakMax = 6;               // reads in a row before it drops its guard
constexpr int kStreakMaxL = 10;           // armor-off: reads in a row before it drops its guard
// ---- v11: the armor-off phase is an agile duelist: fewer guards, more dodging, much faster, almost no openings ----
constexpr float kReadBaseL = 0.62f;         // chance it reacts to a swing at all (armor off); the rest of your swings connect
constexpr float kReadMaxL = 0.85f;
constexpr int kReadCooldownL = 3;
constexpr int kArmTicksL = 10;              // armor-off guard lasts only 0.33 s: it does not turtle
constexpr float kYorokeCancelFrame = 4.0f;  // your jump strike / 4th hit staggers it; it recovers after this many frames
constexpr float kYorokeDodge = 0.5f;        // ... and then dodges aside (else it swings straight back)
constexpr float kHitRunChance = 0.6f;       // after its swing it jumps aside instead of standing in your counter
constexpr float kHoldChanceL = 0.2f;        // armor-off wind-up holds (fewer: this is a fast fight)
constexpr int kHoldMinL = 4, kHoldMaxL = 8;   // a half-beat delay, 0.13-0.27 s
// ---- v12 ----
constexpr float kFastSwingChance = 0.4f;    // mixed timing: this share of its swings come out extra fast ...
constexpr float kFastSwingMult = 1.3f;      // ... at this extra speed
constexpr int kDoubleDodgeTicks = 25;       // you attack again within this long after it dodged: it dodges again
constexpr float kRollRush = 0.85f;          // you rolled / backflipped out of reach: it leaps in and swings
constexpr float kChargeRush = 0.5f;         // per tick while you charge a spin from a distance
constexpr float kChargePunishL = 0.7f;      // per tick while you charge a spin in its reach (armor off)
constexpr int kFlurryChain = 3;             // at full rage: extra swings in one burst, then it jumps away
constexpr float kPressureChanceL = 0.9f;
constexpr float kPressureRangeL = 480.0f;
constexpr float kStunCancelFrameL = 5.0f;   // armor-off: it shakes off a hit after this many animation frames (was 12)
constexpr float kStunCancelMinL = 3.0f;     // ... and at full rage after this many
// ---- v4: mind games ----
constexpr float kHoldChance = 0.7f;         // chance a swing is held back in its wind-up (so you cannot time a dodge)
constexpr int kHoldMin = 3;                 // ticks (0.1 s)
constexpr int kHoldMax = 12;                // ticks (0.4 s)
constexpr float kBreakerChance = 0.45f;     // per tick while you hold your shield up and it is in kick range
constexpr float kBreakerRange = 230.0f;
constexpr float kHabitShare = 0.6f;         // this share of your last 12 moves being the same = "you are repeating"
constexpr int kHabitMinSamples = 6;
constexpr int kHotTicks = 60;
constexpr float kPunishChance = 0.85f;      // it strikes the moment you roll / backflip near it (both phases)
constexpr float kChargePunish = 0.25f;      // per tick while you charge a spin attack near it
constexpr int kBiasMax = 4;                 // how well it learns which way you dodge
constexpr int kBiasAge = 120;
constexpr int kBiasTurn = 0x1200;           // it aims its swing this far toward your favourite dodge side
constexpr float kHeatSeconds = 30.0f;       // time without being hit that gives full pressure
constexpr float kPackChance = 0.85f;        // a second Darknut swings the moment the first one guards your attack
constexpr float kRangedLeapChance = 0.7f;   // leap-in chance (x0.1 per tick) while you use bow/boomerang/hookshot
constexpr float kRangedWalkH = 11.5f;
// ---- Safety switches: set one to false and rebuild if something ever misbehaves ----
constexpr bool kUseHitHook = false;         // OFF by design: with this on, some of your hits were cancelled. Now every hit connects.
constexpr bool kUseDodgeJumps = true;       // the armor-off dodge jumps (calls the game's own initChaseL)
constexpr int kComboGap = 36;               // ticks between swings that still count as one combo (1.2 s)
constexpr int kComboCancel = 4;             // the 4th swing of your combo is the one it refuses to be staggered by
constexpr int kStreakForget = 150;          // ticks without a read before the streak resets
constexpr int kExposeTicks = 24;            // your opening after kStreakMax reads (0.8 s)
constexpr int kExposeTicksL = 4;            // armor off: almost no opening (0.13 s)
// ---- Dodging (armor-off Darknut; needs the dodge-jump hook) ----
constexpr float kEvadeBackSlash = 0.35f, kEvadeSideSlash = 0.45f;
constexpr float kEvadeBackJump = 0.55f, kEvadeSideJump = 0.40f;
constexpr float kEvadeBackWide = 0.70f;     // spin / great spin / down thrust
constexpr float kEvadeSideHead = 0.75f;     // helm splitter
constexpr float kEvadeBackBash = 0.9f;      // shield bash (it never guards a bash: that would stun it)
// ---- Offense ----
constexpr float kRiposteChance = 0.92f;
constexpr int kRiposteDelay = 1;            // ticks after your swing ends
constexpr int kRiposteWindow = 26;          // it keeps trying for this long if it is busy
constexpr float kRiposteRange = 430.0f;
constexpr float kPressureRange = 380.0f;
constexpr float kPressureChance = 0.65f;    // per tick while ready and in reach
constexpr int kAttackGapH = 14;             // minimum ticks between its own attack starts (armored)
constexpr int kAttackGapL = 2;              // (armor off)
constexpr float kLinkReach = 260.0f;        // do not walk into a swing that is already reaching it
constexpr int kChainMax = 4;                // extra swings without waiting for its recovery (armored)
constexpr int kChainMaxL = 8;               // armor off
constexpr float kChainChance = 0.97f;
constexpr float kChainRange = 380.0f;
constexpr float kAttackSpeedMult = 1.4f;    // wind-up speed (1.0 = normal)
constexpr float kAttackSpeedMultL = 1.8f;   // armor off
constexpr int kMaxGameCooldown = 6;        // clamps the game's own attack timer (normally 75-175 ticks)
constexpr float kLeapMin = 350.0f;          // armor-off gap closer
constexpr float kLeapMax = 900.0f;
constexpr float kLeapChance = 0.5f;
constexpr float kLeapIdleL = 0.5f;          // leap-in chance multiplier when you just back away (was 0.1)
constexpr int kLeapCooldown = 45;
// ---- Footwork ----
constexpr int kTurnStep = 0x1000;           // max turn per tick toward you (the game turns ~0x160-0x800)
constexpr float kTurnRange = 700.0f;
constexpr float kEngageRange = 1400.0f;
constexpr float kWalkSpeedH = 8.5f;         // armored walk (game: 4.3), approach (2.3 -> 4.4)
constexpr float kApproachSpeedH = 6.0f;
constexpr float kWalkSpeedL = 12.0f;        // armor-off walk (game: 4.7)
constexpr float kStrafeSpeedL = 11.0f;      // armor-off circling (game: 4.8)
constexpr float kDamageMultH = 1.5f;       // its sword damage while armored (game: 4 = one heart)
constexpr float kDamageMultL = 3.0f;       // armor off (game: 2 -> 6 = a heart and a half)
// ---- v8: armor-off rage: every hit you land on the armor-off Darknut makes it fiercer (its health is untouched) ----
constexpr float kRageHits = 3.0f;           // hits that give full rage
constexpr int kRageStartHits = 2;           // it starts at two-thirds rage the moment its armor comes off
constexpr float kRageAttackSpeed = 0.4f;    // extra wind-up speed at full rage
constexpr float kRageGap = 0.4f;            // attack gap shrinks by this share at full rage
constexpr int kRageChain = 2;               // extra chained swings at full rage
constexpr float kRageExpose = 0.5f;         // your opening shrinks by this share at full rage
constexpr int kRageStreak = 4;              // extra reads in a row before it drops its guard
constexpr float kRageDamage = 0.5f;         // extra damage multiplier at full rage
constexpr float kShakeCounter = 1.0f;       // you hit it, it shakes the hit off and swings back at once (+0.3 at full rage)
constexpr float kGuardCounter = 1.0f;       // it blocks your swing, leaves its guard early and swings back (+0.3 at full rage)
constexpr float kGuardCancelFrameL = 8.0f;  // armor off: how long it holds the guard animation after your swing ends
constexpr int kLandingRange = 150;
// ---- v10: more tactics ----
constexpr float kPostMoveStrike = 1.0f;     // after any dodge-jump / leap it swings the moment it lands
constexpr int kFinishTicks = 60;            // after YOU take a hit it attacks with no pause for this long (1.5 s)
constexpr int kFinishChain = 3;             // ... and chains this many extra swings
constexpr float kZoneChance = 0.4f;         // per tick while you walk up to it: it steps back, then strikes as you follow
constexpr float kZoneMin = 260.0f, kZoneMax = 430.0f;
constexpr float kZoneApproach = 4.0f;       // you must be closing this many units per tick
constexpr int kZoneCooldown = 45;
constexpr float kStutterChance = 0.0f;      // off: no more stopping dead (this is a fast fight)
constexpr int kStutterMin = 5, kStutterMax = 12;
constexpr int kBurstTicks = 10;
constexpr float kBurstSpeedL = 15.0f;
constexpr float kStutterMinDist = 320.0f, kStutterMaxDist = 900.0f;
constexpr float kLandedBias = 0.12f;        // a move that got through makes it dodge that move more (per hit, max 3)
constexpr float kLandedBiasMax = 0.36f;
constexpr int kLandedAge = 600;             // ticks (20 s) for one of those memories to fade          // after your jump strike / helm splitter it punishes from further away
constexpr int kHistorySize = 12;
constexpr int kStatusTicks = 150;
}  // namespace cfg

// Game hooks (declared at file scope).
// ---- Calling the game's own dodge-jump setup, through a pass-through hook (no link dependency) ----
DEFINE_HOOK_SYMBOL("daB_TN_c::initChaseL", void(daB_TN_c*, int), TnInitChaseL);
// ---- The game's hit code: used only to cancel hits the guard cannot take on its own ----
DEFINE_HOOK_SYMBOL("cCcS::SetAtTgCommonHitInf", void(cCcS*, cCcD_Obj*, cCcD_Obj*, cXyz*), AtTgHit);

namespace {

using TN = daB_TN_c;

enum class AType { None, Slash, Jump, Spin, Down, Head, Large, Bash };
enum class St { Ready, Guard, Attack, Hurt, Other };

struct Mem {
    int armed = 0;        // ticks left in the current read
    int readCd = 0;
    int streak = 0;       // reads in a row
    int streakAge = 0;
    int expose = 0;       // ticks left of your opening
    int atkCd = 0;        // ticks until it may start another attack on its own
    int chain = 0;        // swings chained without a break
    int chainAge = 0;
    bool chainRolled = false;
    int leapCd = 0;
    int riposteIn = -1;   // ticks until the riposte window opens
    int riposteTry = 0;   // ticks left of the riposte window
    bool speeded = false;
    bool lastHurt = false;
    bool seen = false;
    int hold = 0;
    float savedRate = 1.0f;
    bool holdDone = false;
    int hot = 0;          // you are repeating a move: it counters harder
    int bias = 0;         // + = you dodge toward its yaw-positive side, - = the other
    int biasAge = 0;
    int hits = 0;         // hits you have landed on it since it lost its armor (drives its rage)
    bool rageInit = false;
    int zoneCd = 0;
    float prevDist = -1.0f;
    int stutter = 0;
    int burst = 0;
    int sinceDodge = 1000;  // ticks since it last dodged
};

struct Stats {
    int read = 0, guard = 0, evade = 0, negated = 0, landed = 0, riposte = 0, pressure = 0, chain = 0,
        leap = 0, opening = 0, breaker = 0, held = 0, habit = 0, punish = 0, pack = 0, counter = 0, landing = 0, finish = 0, zone = 0, stutter = 0, hitrun = 0, rush = 0, doubled = 0, jumpdodge = 0;
};

std::unordered_map<const void*, Mem> g_mem;
Stats g_stats;
int g_statTicks = 0;
int g_tick = 0;
int g_logBudget = 80;
bool g_hitHook = false;
bool g_jumpHook = false;
bool g_readThisTick = false;
int g_sinceLanded = 0;
float g_heat = 0.0f;
float g_rageSeen = 0.0f;   // highest armor-off rage in the last status interval (for the status line)

// Link's attack bookkeeping
u16 g_prevProc = 0;
bool g_prevSwinging = false;
bool g_prevLinkAtk = false;
int g_sinceAtkStart = 1000;
int g_sinceAtkEnd = 1000;
// Your combo swing number. Counted here on purpose: the game's own combo counter sits one byte off in the
// Dusklight fork this mod runs on, so reading it from this mod gives the wrong value.
int g_combo = 0;
AType g_lastAtype = AType::None;   // what your last attack was
int g_linkHurt = 0;                // ticks left of its "you just got hit" pressure window
int g_prevLife = -1;
int g_landedBy[8] = {};            // how often each of your move types got through its defence lately
int g_landedAge = 0;
AType g_hist[cfg::kHistorySize] = {};
int g_histCount = 0;
int g_histHead = 0;

uint32_t g_rng = 2463534242u;

constexpr float kPi = 3.14159265358979f;
constexpr float kAngleToRad = kPi / 32768.0f;  // game angles: 65536 = full circle

float rand01() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (g_rng & 0xFFFFFF) / float(0x1000000);
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float dist_xz(const fopAc_ac_c* a, const fopAc_ac_c* b) {
    const float dx = a->current.pos.x - b->current.pos.x;
    const float dz = a->current.pos.z - b->current.pos.z;
    return std::sqrt(dx * dx + dz * dz);
}

// Yaw (game angle) that points from `from` toward `to`.
s16 angle_to(const fopAc_ac_c* from, const fopAc_ac_c* to) {
    const float dx = to->current.pos.x - from->current.pos.x;
    const float dz = to->current.pos.z - from->current.pos.z;
    const int a = static_cast<int>(std::lround(std::atan2(dx, dz) / kAngleToRad));
    return static_cast<s16>(static_cast<uint16_t>(a & 0xFFFF));
}

int angle_gap(const TN* t, s16 target) {
    return std::abs(static_cast<int>(static_cast<s16>(target - t->shape_angle.y)));
}

void turn_toward(TN* t, s16 target, int maxStep) {
    const int diff = static_cast<int>(static_cast<s16>(target - t->shape_angle.y));
    const int step = std::clamp(diff, -maxStep, maxStep);
    t->shape_angle.y = static_cast<s16>(t->shape_angle.y + step);
}

bool h_mode(const TN* t) {
    return t->mActionMode1 < TN::ACT_CHANGEDEMO;  // armored (the shield-and-armor phase)
}

St classify(const TN* t) {
    switch (t->mActionMode1) {
    case TN::ACT_CHASEH: return St::Ready;
    case TN::ACT_CHASEL: {
        const int m2 = t->mActionMode2;
        return (m2 <= 5 || m2 == 16) ? St::Ready : St::Other;  // not mid-leap
    }
    case TN::ACT_GUARDH:
    case TN::ACT_GUARDL: return St::Guard;
    case TN::ACT_ATTACKH:
    case TN::ACT_ATTACKSHIELDH:
    case TN::ACT_ATTACKL:
    case TN::ACT_ATTACKSHIELDL: return St::Attack;
    case TN::ACT_DAMAGEH:
    case TN::ACT_DAMAGEL:
    case TN::ACT_YOROKE: return St::Hurt;
    default: return St::Other;  // demos, waiting, ending
    }
}

// Its own swing is past the part that can hurt you and is only recovering.
bool in_recovery(const TN* t) {
    if (t->mpModelMorf2 == nullptr) {
        return false;
    }
    const float f = t->mpModelMorf2->getFrame();
    const int m2 = t->mActionMode2;
    if (t->mActionMode1 == TN::ACT_ATTACKH) {
        if (m2 == 1) {
            return f >= 41.0f;
        }
        if (m2 == 2) {
            return f >= 37.0f;
        }
    } else if (t->mActionMode1 == TN::ACT_ATTACKL) {
        if (m2 == 1 || m2 == 101) {
            return f >= 14.5f;  // the sword hitbox turns off at frame 14
        }
        if (m2 == 2 || m2 == 102) {
            return f >= 38.5f;  // ... at frame 38
        }
    }
    return false;
}

// 0 = calm, 1 = full rage. Armor off only.
float rage_of(const TN* t, const Mem& m) {
    if (h_mode(t)) {
        return 0.0f;
    }
    return clampf(float(m.hits) / cfg::kRageHits, 0.0f, 1.0f);
}

// Same entry the game itself uses (setActionMode): two fields and the head-lock flag.
void enter(TN* t, int mode1, int mode2) {
    t->offHeadLockFlg();
    t->mActionMode1 = mode1;
    t->mActionMode2 = mode2;
}

AType attack_type_of(u16 proc) {
    switch (proc) {
    case daAlink_c::PROC_CUT_NORMAL:
    case daAlink_c::PROC_CUT_FINISH:
    case daAlink_c::PROC_CUT_FINISH_JUMP_UP: return AType::Slash;
    case daAlink_c::PROC_CUT_JUMP: return AType::Jump;
    case daAlink_c::PROC_CUT_TURN: return AType::Spin;
    case daAlink_c::PROC_CUT_DOWN: return AType::Down;
    case daAlink_c::PROC_CUT_HEAD: return AType::Head;
    case daAlink_c::PROC_CUT_LARGE_JUMP: return AType::Large;
    case daAlink_c::PROC_GUARD_ATTACK: return AType::Bash;
    default: return AType::None;
    }
}

const char* atype_name(AType t) {
    switch (t) {
    case AType::Slash: return "slash";
    case AType::Jump: return "jump strike";
    case AType::Spin: return "spin attack";
    case AType::Down: return "down thrust";
    case AType::Head: return "helm splitter";
    case AType::Large: return "great spin";
    case AType::Bash: return "shield bash";
    default: return "attack";
    }
}

bool wide_move(AType t) {
    return t == AType::Spin || t == AType::Down || t == AType::Large || t == AType::Jump || t == AType::Head;
}

void remember_attack(AType t) {
    g_hist[g_histHead] = t;
    g_histHead = (g_histHead + 1) % cfg::kHistorySize;
    if (g_histCount < cfg::kHistorySize) {
        g_histCount++;
    }
}

float habit_share(AType t) {
    if (g_histCount < 4) {
        return 0.0f;
    }
    int n = 0;
    for (int i = 0; i < g_histCount; i++) {
        if (g_hist[i] == t) {
            n++;
        }
    }
    return float(n) / float(g_histCount);
}

void log_event(const char* what, const TN* t, AType type, float dist) {
    if (g_logBudget <= 0) {
        return;
    }
    g_logBudget--;
    mods::log::info("darknut: {} ({}) - your {} at {:.0f} units", what, h_mode(t) ? "armored" : "armor off",
                    atype_name(type), dist);
}

// Face you, shield up from every angle (the game's own "mTimer10": guard from anywhere).
void raise_guard(TN* t, const fopAc_ac_c* plr, int ticks) {
    turn_toward(t, angle_to(t, plr), 0x7FFF);
    t->field_0xa91 = true;
    if (t->mTimer10 < ticks) {
        t->mTimer10 = static_cast<u8>(std::min(ticks, 200));
    }
}

// NEVER force the armor-off sword-guard state (ACT_GUARDL) from here. Its first tick calls the game's
// getCutType(), which dereferences mAtInfo.mpCollider with no null check. The game only enters that state from
// inside damage_check, right after it has pointed mpCollider at the thing that hit it; damage_check also resets
// mpCollider to NULL at the start of every frame, so a state forced from the mod always meets NULL = crash.
// Armor off therefore just raises the game's own guard flags: the first hit that lands is then blocked by the
// game's normal path (real sparks, real guard animation, valid collider). Armored is safe (GUARDH never reads it).
void enter_guard_state(TN* t, const fopAc_ac_c* plr, St st) {
    raise_guard(t, plr, (h_mode(t) ? cfg::kArmTicks : cfg::kArmTicksL) + 8);
    t->field_0xaa8 = false;
    if (h_mode(t)) {
        enter(t, TN::ACT_GUARDH, TN::ACTION2_0_e);
    } else if (st == St::Attack) {
        // mid-recovery of its own swing: drop the swing and go back to the guarded chase (turns the sword hitbox off)
        enter(t, TN::ACT_CHASEL, TN::ACTION2_0_e);
    }
}

// Blind-side jump: away from the side you usually dodge toward (code 10 = its negative side, 13 = positive side).
int pick_side(const Mem& m) {
    if (m.bias >= 2) {
        return 10;
    }
    if (m.bias <= -2) {
        return 13;
    }
    return rand01() < 0.5f ? 10 : 13;
}

// which: 6 = back-jump, 10 = jump right, 13 = jump left (the game's own dodge jumps)
bool do_dodge(TN* t, int which, Mem& m) {
    if (!g_jumpHook || TnInitChaseL::g_orig == nullptr || h_mode(t)) {
        return false;
    }
    enter(t, TN::ACT_CHASEL, TN::ACTION2_0_e);
    TnInitChaseL::g_orig(t, which);
    if (which != 6) {
        t->mTimer7 = 10;
    }
    m.sinceDodge = 0;
    return true;
}

void start_attack(TN* t, Mem& m, float dist) {
    if (h_mode(t)) {
        // like the game: the shield kick at point-blank, unless its armor is nearly gone
        enter(t, (dist < 200.0f && t->mNextBreakPart < 11) ? TN::ACT_ATTACKSHIELDH : TN::ACT_ATTACKH,
              TN::ACTION2_0_e);
    } else {
        enter(t, dist < 200.0f ? TN::ACT_ATTACKSHIELDL : TN::ACT_ATTACKL, TN::ACTION2_0_e);
    }
    m.atkCd = static_cast<int>((h_mode(t) ? cfg::kAttackGapH : cfg::kAttackGapL) * (1.0f - 0.5f * g_heat) *
                               (1.0f - cfg::kRageGap * rage_of(t, m)));
    m.chainRolled = false;
    m.speeded = false;
    m.armed = 0;
    if (std::abs(m.bias) >= 2) {
        // you keep dodging to one side: it aims its swing a little that way
        t->shape_angle.y = static_cast<s16>(t->shape_angle.y + (m.bias > 0 ? cfg::kBiasTurn : -cfg::kBiasTurn));
    }
}

float read_chance(AType t, bool recovering, bool lmode) {
    const float habit = clampf((habit_share(t) - 0.25f) / 0.5f, 0.0f, 1.0f);
    float c = lmode ? std::min(cfg::kReadMaxL, cfg::kReadBaseL + cfg::kReadHabitBonus * habit)
                    : std::min(cfg::kReadMax, cfg::kReadBase + cfg::kReadHabitBonus * habit);
    if (recovering) {
        c *= cfg::kRecoveryReadMult;
    }
    return c;
}

// Returns true if the Darknut reacted (read the move).
bool try_read(TN* t, Mem& m, const fopAc_ac_c* plr, AType type, St st, float dist, float fx, float fz) {
    if (m.expose > 0 || m.readCd > 0) {
        return false;
    }
    // A swing it is holding back in its wind-up is a feint: if you swing into it, it drops the swing and guards.
    const bool recovering = (st == St::Attack) && (in_recovery(t) || m.hold > 0);
    // You attack again right after it dodged, while it is still landing: it dodges AGAIN, so chasing it with combos fails.
    const int m2now = t->mActionMode2;
    const bool doubleDodge = !h_mode(t) && g_jumpHook && TnInitChaseL::g_orig != nullptr &&
                             m.sinceDodge < cfg::kDoubleDodgeTicks && st == St::Other &&
                             t->mActionMode1 == TN::ACT_CHASEL &&
                             (m2now == 7 || m2now == 9 || m2now == 12 || m2now == 15);
    if (!(st == St::Ready || st == St::Guard || recovering || doubleDodge)) {
        return false;
    }
    const bool wide = wide_move(type);
    if (dist > (wide ? cfg::kReadRangeWide : cfg::kReadRange) || dist < 1.0f) {
        return false;
    }
    if (!wide) {
        const float ex = (t->current.pos.x - plr->current.pos.x) / dist;  // you -> it
        const float ez = (t->current.pos.z - plr->current.pos.z) / dist;
        if (ex * fx + ez * fz < cfg::kReadLinkFacingCos) {
            return false;
        }
    }
    // No dodge from inside the game's own guard animation: that state is left to the game.
    const bool canDodge = g_jumpHook && TnInitChaseL::g_orig != nullptr && !h_mode(t) && st != St::Guard;
    if (type == AType::Bash && !canDodge) {
        return false;  // a bash is meant to break its guard: only a dodge makes sense
    }
    // A jump strike is never even rolled for: it just sidesteps it (then punishes the landing).
    const bool jumpCounter = canDodge && (type == AType::Jump || type == AType::Large);
    if (!jumpCounter && !doubleDodge && rand01() >= read_chance(type, recovering, !h_mode(t))) {
        return false;
    }

    const bool repeating =
        g_histCount >= cfg::kHabitMinSamples && habit_share(type) >= cfg::kHabitShare;
    if (repeating) {
        m.hot = cfg::kHotTicks;
        g_stats.habit++;
    }
    g_readThisTick = true;
    m.readCd = h_mode(t) ? cfg::kReadCooldown : cfg::kReadCooldownL;
    m.streakAge = 0;
    m.riposteTry = 0;
    m.riposteIn = -1;
    g_stats.read++;
    m.streak++;
    m.armed = h_mode(t) ? cfg::kArmTicks : cfg::kArmTicksL;
    const float rage = rage_of(t, m);
    if (m.streak >= (h_mode(t) ? cfg::kStreakMax : cfg::kStreakMaxL + int(cfg::kRageStreak * rage))) {
        m.streak = 0;
        m.expose = h_mode(t) ? cfg::kExposeTicks
                             : std::max(4, int(cfg::kExposeTicksL * (1.0f - cfg::kRageExpose * rage)));
        g_stats.opening++;
    }

    // Choose a dodge for the armor-off Darknut (the armored one has no dodge jumps and always guards).
    if (canDodge) {
        float back = 0.0f, side = 0.0f;
        switch (type) {
        case AType::Slash: back = cfg::kEvadeBackSlash; side = cfg::kEvadeSideSlash; break;
        case AType::Jump: back = cfg::kEvadeBackJump; side = cfg::kEvadeSideJump; break;
        case AType::Spin:
        case AType::Large:
        case AType::Down: back = cfg::kEvadeBackWide; break;
        case AType::Head: side = cfg::kEvadeSideHead; break;
        case AType::Bash: back = cfg::kEvadeBackBash; break;
        default: break;
        }
        if (repeating) {
            if (type == AType::Head) {
                back = 0.0f;
                side = 0.85f;
            } else if (type == AType::Slash) {
                back = 0.45f;
                side = 0.40f;
            } else {
                back = 0.85f;
                side = 0.0f;
            }
        }
        {
            // a move of this type got through its defence recently: it dodges that one more
            const int idx = std::clamp(static_cast<int>(type), 0, 7);
            const float lb = std::min(cfg::kLandedBiasMax, cfg::kLandedBias * float(g_landedBy[idx]));
            if (back > 0.0f) {
                back = std::min(0.95f, back + lb);
            } else {
                side = std::min(0.95f, side + lb);
            }
        }
        if (jumpCounter) {
            back = 0.2f;
            side = 0.8f;
        }
        if (doubleDodge) {
            back = 0.35f;
            side = 0.65f;
        }
        const float r = rand01();
        if (r < back) {
            if (do_dodge(t, 6, m)) {
                g_stats.evade++;
                g_stats.jumpdodge += jumpCounter ? 1 : 0;
                g_stats.doubled += doubleDodge ? 1 : 0;
                log_event("read -> back-jump", t, type, dist);
                return true;
            }
        } else if (r < back + side) {
            if (do_dodge(t, pick_side(m), m)) {
                g_stats.evade++;
                g_stats.jumpdodge += jumpCounter ? 1 : 0;
                g_stats.doubled += doubleDodge ? 1 : 0;
                log_event("read -> side-jump", t, type, dist);
                return true;
            }
        }
    }
    if (doubleDodge) {
        return false;  // no dodge this time: it never guards from the middle of a landing
    }
    if (type == AType::Bash) {
        m.armed = 0;
        return false;
    }
    enter_guard_state(t, plr, st);
    g_stats.guard++;
    log_event("read -> guard", t, type, dist);
    return true;
}

int collect_tn(void* actor, void* data) {
    auto* list = static_cast<std::vector<fopAc_ac_c*>*>(data);
    auto* a = static_cast<fopAc_ac_c*>(actor);
    if (fopAcM_GetProfName(a) == fpcNm_B_TN_e) {
        list->push_back(a);
    }
    return 1;
}

void reset_link_state() {
    g_prevProc = 0;
    g_prevSwinging = false;
    g_prevLinkAtk = false;
    g_sinceAtkStart = 1000;
    g_sinceAtkEnd = 1000;
    g_combo = 0;
    g_lastAtype = AType::None;
    g_linkHurt = 0;
    g_prevLife = -1;
}

constexpr u32 kHitMask = u32(AT_TYPE_NORMAL_SWORD) | u32(AT_TYPE_MASTER_SWORD) | u32(AT_TYPE_SHIELD_ATTACK) |
                         u32(AT_TYPE_WOLF_CUT_TURN) | u32(AT_TYPE_WOLF_ATTACK);

// Hits the Darknut's guard cannot take by itself are cancelled while it has read your move: the 4th hit of a
// combo (armor off: that is how the game staggers it) and any hit once its armor is nearly gone.
HookAction on_at_tg_hit(ModContext*, void* args, void*, void*) {
    cCcD_Obj* at = mods::arg<cCcD_Obj*>(args, 1);
    cCcD_Obj* tg = mods::arg<cCcD_Obj*>(args, 2);
    if (at == nullptr || tg == nullptr || (u32(at->GetAtType()) & kHitMask) == 0) {
        return HOOK_CONTINUE;
    }
    cCcD_Stts* as = at->GetStts();
    cCcD_Stts* ts = tg->GetStts();
    fopAc_ac_c* plr = dComIfGp_getPlayer(0);
    if (as == nullptr || ts == nullptr || plr == nullptr) {
        return HOOK_CONTINUE;
    }
    fopAc_ac_c* dst = ts->GetAc();
    if (as->GetAc() != plr || dst == nullptr || dst == plr || fopAcM_GetProfName(dst) != fpcNm_B_TN_e) {
        return HOOK_CONTINUE;
    }
    auto it = g_mem.find(dst);
    if (it == g_mem.end() || it->second.armed <= 0) {
        return HOOK_CONTINUE;
    }
    auto* t = static_cast<TN*>(dst);
    const bool cancel = h_mode(t) ? (t->mNextBreakPart >= 11) : (g_combo >= cfg::kComboCancel);
    if (cancel) {
        g_stats.negated++;
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

void tick() {
    fopAc_ac_c* plr = dComIfGp_getPlayer(0);
    if (plr == nullptr || dComIfGp_event_runCheck()) {
        reset_link_state();
        g_mem.clear();
        return;
    }
    auto* py = static_cast<daPy_py_c*>(plr);
    auto* link = static_cast<daAlink_c*>(plr);
    g_tick++;

    // ---- What is Link doing? ----
    const bool swinging = py->getCutAtFlg();
    const bool jumpStrike = py->checkCutJumpMode();
    const u16 proc = link->mProcID;
    const AType atype = attack_type_of(proc);
    const bool linkAtk = swinging || jumpStrike || atype != AType::None;
    g_sinceAtkStart++;
    g_sinceAtkEnd++;
    const bool procStart = atype != AType::None && proc != g_prevProc;
    const bool flagStart = swinging && !g_prevSwinging && g_sinceAtkStart > 8;
    const bool attackStart = procStart || flagStart;
    const AType startType = (atype != AType::None) ? atype : AType::Slash;
    if (attackStart) {
        if (startType == AType::Slash) {
            g_combo = (g_sinceAtkStart <= cfg::kComboGap) ? g_combo + 1 : 1;
        } else {
            g_combo = 0;
        }
        g_sinceAtkStart = 0;
        g_lastAtype = startType;
        remember_attack(startType);
    } else if (g_sinceAtkStart > cfg::kComboGap + 20 && !swinging) {
        g_combo = 0;
    }
    const bool rollStart = (proc == daAlink_c::PROC_FRONT_ROLL || proc == daAlink_c::PROC_SIDE_ROLL ||
                            proc == daAlink_c::PROC_BACK_JUMP) && proc != g_prevProc;
    const bool charging = py->checkCutCharge();
    const bool ranged = proc == daAlink_c::PROC_BOW_MOVE || proc == daAlink_c::PROC_BOW_SUBJECT ||
                        proc == daAlink_c::PROC_BOOMERANG_MOVE || proc == daAlink_c::PROC_BOOMERANG_SUBJECT ||
                        proc == daAlink_c::PROC_HOOKSHOT_MOVE || proc == daAlink_c::PROC_HOOKSHOT_SUBJECT;
    {
        // pressure: long without being hit, or you are low on hearts
        const auto& sa = g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusA();
        const float maxLife = std::max<float>(1.0f, sa.getMaxLife());
        const float lifeRatio = clampf(sa.getLife() / maxLife, 0.0f, 1.0f);
        g_sinceLanded++;
        const int life = static_cast<int>(sa.getLife());
        if (g_prevLife >= 0 && life < g_prevLife) {
            g_linkHurt = cfg::kFinishTicks;  // you just took a hit: it goes for the kill
        } else if (g_linkHurt > 0) {
            g_linkHurt--;
        }
        g_prevLife = life;
        if (++g_landedAge > cfg::kLandedAge) {
            g_landedAge = 0;
            for (int& n : g_landedBy) {
                n = std::max(0, n - 1);
            }
        }
        g_heat = clampf(std::min(1.0f, g_sinceLanded / (cfg::kHeatSeconds * 30.0f)) * 0.6f +
                            std::max(0.0f, 0.5f - lifeRatio) * 1.2f,
                        0.0f, 1.0f);
    }
    g_readThisTick = false;
    const bool finish = g_linkHurt > 0;
    const bool attackEnd = !linkAtk && g_prevLinkAtk;
    if (attackEnd) {
        g_sinceAtkEnd = 0;
    }
    g_prevLinkAtk = linkAtk;
    g_prevProc = proc;
    g_prevSwinging = swinging;

    const float facing = plr->shape_angle.y * kAngleToRad;
    const float fx = std::sin(facing);  // your forward direction on the ground
    const float fz = std::cos(facing);

    std::vector<fopAc_ac_c*> all;
    fopAcIt_Executor(collect_tn, &all);

    std::unordered_map<const void*, Mem> alive;
    std::vector<std::pair<TN*, float>> strikers;
    int engaged = 0;
    for (fopAc_ac_c* a : all) {
        const float dist = dist_xz(a, plr);
        if (dist > cfg::kEngageRange) {
            continue;
        }
        auto* en = static_cast<fopEn_enemy_c*>(a);
        if (en->checkDeadFlg() || en->checkDownFlg()) {
            continue;
        }
        auto* t = static_cast<TN*>(a);
        if (t->mInactive || t->mDisappear) {
            continue;
        }
        engaged++;

        auto it = g_mem.find(a);
        Mem m = (it != g_mem.end()) ? it->second : Mem{};
        if (!m.seen) {
            m.seen = true;
            mods::log::info("darknut: found one (type {}, state {}/{}, {:.0f} units away)", int(t->mType),
                            t->mActionMode1, t->mActionMode2, dist);
        }

        // timers
        if (m.armed > 0) {
            m.armed--;
        } else if (m.expose > 0) {
            m.expose--;
        }
        if (m.readCd > 0) m.readCd--;
        if (m.hot > 0) m.hot--;
        if (m.atkCd > 0) m.atkCd--;
        if (m.leapCd > 0) m.leapCd--;
        if (m.zoneCd > 0) m.zoneCd--;
        if (m.sinceDodge < 1000) m.sinceDodge++;
        if (m.streak > 0 && ++m.streakAge > cfg::kStreakForget) {
            m.streak = 0;
            m.streakAge = 0;
        }

        bool shookOff = false;
        bool guardOff = false;
        bool yorokeOff = false;
        // Armor-off: the stagger from your jump strike / 4th hit (the game's long "knocked back" animation) is cut
        // short. Your hit still connects; it just does not give you a free window afterwards.
        if (t->mActionMode1 == TN::ACT_YOROKE && t->mActionMode2 == TN::ACTION2_10_e && t->mpModelMorf2 != nullptr &&
            t->mpModelMorf2->getFrame() >= cfg::kYorokeCancelFrame) {
            enter(t, TN::ACT_CHASEL, TN::ACTION2_0_e);  // the same transition the game makes when that animation ends
            t->field_0xa91 = true;
            yorokeOff = true;
        }
        // Armor-off: it shakes off a hit early instead of staying staggered for your combo.
        if (t->mActionMode1 == TN::ACT_DAMAGEL && t->mActionMode2 >= 2 && t->mpModelMorf2 != nullptr &&
            t->mpModelMorf2->getFrame() >= cfg::kStunCancelFrameL -
                                               (cfg::kStunCancelFrameL - cfg::kStunCancelMinL) * rage_of(t, m)) {
            enter(t, TN::ACT_CHASEL, TN::ACTION2_0_e);
            t->field_0xa91 = true;
            shookOff = true;
        }
        // Armor-off: once your swing has been absorbed it leaves its guard animation early, ready to counter.
        if (t->mActionMode1 == TN::ACT_GUARDL && t->mActionMode2 == TN::ACTION2_1_e && !linkAtk &&
            t->mpModelMorf2 != nullptr && t->mpModelMorf2->getFrame() >= cfg::kGuardCancelFrameL) {
            enter(t, TN::ACT_CHASEL, TN::ACTION2_0_e);
            t->field_0xa91 = true;
            guardOff = true;
        }

        const St st = classify(t);
        const bool lmode = !h_mode(t);
        const s16 toLink = angle_to(t, plr);
        bool changed = false;

        if (lmode && !m.rageInit) {
            m.rageInit = true;
            m.hits = std::max(m.hits, cfg::kRageStartHits);
        }
        // Recovered from the stagger: it jumps aside (agile) or swings straight back.
        if (yorokeOff && lmode && rand01() < cfg::kYorokeDodge &&
            do_dodge(t, pick_side(m), m)) {
            g_stats.evade++;
            changed = true;
        }
        // It took your hit / blocked your swing and is out of that state: it swings back at once.
        if (!changed && (shookOff || guardOff || yorokeOff) && lmode && dist < cfg::kRiposteRange && angle_gap(t, toLink) < 0x5000 &&
            rand01() < (shookOff ? cfg::kShakeCounter : cfg::kGuardCounter) + 0.3f * rage_of(t, m)) {
            start_attack(t, m, dist);
            g_stats.counter++;
            changed = true;
        }
        // It just landed from a dodge-jump or a leap (animation over): it swings at once instead of resetting.
        if (!changed && lmode && st == St::Ready && t->mpModelMorf2 != nullptr && t->mpModelMorf2->isStop()) {
            const int m2 = t->mActionMode2;
            if ((m2 == TN::ACTION2_7_e || m2 == TN::ACTION2_8_e || m2 == TN::ACTION2_12_e || m2 == TN::ACTION2_15_e ||
                 m2 == TN::ACTION2_17_e) &&
                dist < cfg::kRiposteRange && angle_gap(t, toLink) < 0x5000 && rand01() < cfg::kPostMoveStrike) {
                start_attack(t, m, dist);
                g_stats.landing++;
                changed = true;
            }
        }

        // It got hit: it starts over.
        const bool hurt = (st == St::Hurt);
        if (hurt && !m.lastHurt) {
            g_stats.landed++;
            g_sinceLanded = 0;
            if (lmode) {
                m.hits++;
            }
            g_landedBy[std::clamp(static_cast<int>(g_lastAtype), 0, 7)] =
                std::min(3, g_landedBy[std::clamp(static_cast<int>(g_lastAtype), 0, 7)] + 1);
            m.streak = 0;
            m.armed = 0;
            m.expose = 0;
        }
        m.lastHurt = hurt;

        if (st == St::Attack) {
            m.chainAge = 0;
        } else {
            if (m.hold > 0 && t->mpModelMorf2 != nullptr && t->mpModelMorf2->getPlaySpeed() < 0.1f) {
                t->mpModelMorf2->setPlaySpeed(1.0f);  // the swing was interrupted mid-hold: never leave it frozen
            }
            m.holdDone = false;
            m.hold = 0;
            m.speeded = false;     // the game started its next swing by itself: boost that one too
            m.chainRolled = false;
            if (++m.chainAge > 40) {
                m.chain = 0;
            }
        }

        if (lmode) {
            g_rageSeen = std::max(g_rageSeen, rage_of(t, m));
        }

        if (rollStart) {
            const s16 rel = static_cast<s16>(plr->current.angle.y - toLink);
            const float side = std::sin(rel * kAngleToRad);
            if (std::abs(side) > 0.4f) {
                m.bias = std::clamp(m.bias + (side > 0.0f ? 1 : -1), -cfg::kBiasMax, cfg::kBiasMax);
                m.biasAge = 0;
            }
        }
        if (m.bias != 0 && ++m.biasAge > cfg::kBiasAge) {
            m.bias += (m.bias > 0 ? -1 : 1);
            m.biasAge = 0;
        }

        // ---- 1. read the move you just started ----
        if (attackStart) {
            if (try_read(t, m, plr, startType, st, dist, fx, fz)) {
                changed = true;
            }
        }

        if (attackStart && !changed && st == St::Ready && dist < cfg::kRiposteRange) {
            strikers.emplace_back(t, dist);  // did not read it: a pack mate may swing while you are committed
        }

        // ---- 2. keep the read going: face you, guard from every angle; or open up after the streak ----
        if (!changed && (st == St::Ready || st == St::Guard)) {
            if (m.armed > 0) {
                raise_guard(t, plr, 6);
            } else if (m.expose > 0) {
                t->field_0xa91 = false;
                t->mTimer10 = 0;
            }
        }

        // ---- 3. riposte: your swing just ended ----
        const bool landing = (g_lastAtype == AType::Jump || g_lastAtype == AType::Head || g_lastAtype == AType::Large);
        const bool sure = m.hot > 0 || landing;  // a jump strike / helm splitter leaves you open: it never misses that
        if (attackEnd && dist < cfg::kRiposteRange + (landing ? cfg::kLandingRange : 0) &&
            rand01() < (sure ? 1.0f : cfg::kRiposteChance)) {
            m.riposteIn = sure ? 1 : cfg::kRiposteDelay;
            m.riposteTry = 0;
        }
        if (m.riposteIn > 0 && --m.riposteIn == 0) {
            m.riposteTry = cfg::kRiposteWindow;
            m.riposteIn = -1;
        }
        if (m.riposteTry > 0) {
            m.riposteTry--;
            if (m.armed > 0 || linkAtk) {
                m.riposteTry = 0;  // you are swinging again: it reads instead
            } else if (!changed && st == St::Ready && dist < cfg::kRiposteRange + (landing ? cfg::kLandingRange : 0) &&
                       angle_gap(t, toLink) < 0x4000) {
                start_attack(t, m, dist);
                g_stats.riposte++;
                m.riposteTry = 0;
                changed = true;
            }
        }

        // ---- 3a. you rolled / backflipped out of reach, or charge a spin from afar: it leaps in and swings on landing ----
        if (!changed && st == St::Ready && lmode && g_jumpHook && TnInitChaseL::g_orig != nullptr && m.leapCd == 0 &&
            dist > cfg::kRiposteRange * 0.8f && dist < 800.0f && angle_gap(t, toLink) < 0x4000 &&
            ((rollStart && rand01() < cfg::kRollRush) || (charging && rand01() < cfg::kChargeRush))) {
            enter(t, TN::ACT_CHASEL, TN::ACTION2_0_e);
            TnInitChaseL::g_orig(t, dist < 700.0f ? 8 : 17);  // the game's own forward leap
            m.leapCd = cfg::kLeapCooldown / 2;
            g_stats.rush++;
            changed = true;
        }

        // ---- 3b. punish a roll / backflip, and interrupt a spin charge (both phases) ----
        if (!changed && st == St::Ready && dist < cfg::kRiposteRange && angle_gap(t, toLink) < 0x4000) {
            if ((rollStart && rand01() < cfg::kPunishChance) ||
                (charging && dist < 350.0f && rand01() < (lmode ? cfg::kChargePunishL : cfg::kChargePunish))) {
                start_attack(t, m, dist);
                g_stats.punish++;
                changed = true;
            }
        }

        // ---- 4. chain: swing again without waiting out its recovery ----
        if (!changed && st == St::Attack && m.armed == 0 && in_recovery(t) && !m.chainRolled) {
            m.chainRolled = true;
            const bool full = lmode && rage_of(t, m) >= 0.99f;  // full rage: a flurry
            const int chainMax = (lmode ? cfg::kChainMaxL + int(cfg::kRageChain * rage_of(t, m)) : cfg::kChainMax) +
                                 (finish ? cfg::kFinishChain : 0) + (full ? cfg::kFlurryChain : 0);
            if (m.chain < chainMax && dist < cfg::kChainRange && angle_gap(t, toLink) < 0x3800 &&
                (finish || rand01() < cfg::kChainChance)) {
                m.chain++;
                start_attack(t, m, dist);
                g_stats.chain++;
                changed = true;
            } else if (lmode && !finish && rand01() < (full ? 1.0f : cfg::kHitRunChance) &&
                       do_dodge(t, pick_side(m), m)) {
                // chain over: instead of standing in your counter it jumps aside, then strikes again on landing
                m.chain = 0;
                g_stats.hitrun++;
                changed = true;
            }
        }

        // ---- 4b. you just got hit: it does not wait, it swings again immediately ----
        if (!changed && finish && st == St::Ready && dist < (lmode ? cfg::kPressureRangeL : cfg::kPressureRange) + 80.0f &&
            angle_gap(t, toLink) < 0x4000) {
            start_attack(t, m, dist);
            m.chain = 0;
            g_stats.finish++;
            changed = true;
        }

        // ---- 4c. zoning: you walk up to it without swinging; it steps back and strikes as you follow ----
        if (!changed && lmode && st == St::Ready && g_jumpHook && !linkAtk && m.zoneCd == 0 && m.armed == 0 &&
            m.prevDist > 0.0f && dist > cfg::kZoneMin && dist < cfg::kZoneMax &&
            (m.prevDist - dist) > cfg::kZoneApproach && rand01() < cfg::kZoneChance) {
            if (do_dodge(t, 6, m)) {
                m.zoneCd = cfg::kZoneCooldown;
                g_stats.zone++;
                changed = true;
            }
        }

        // ---- 5. pressure: you are in reach and not swinging, so it attacks ----
        if (!changed && st == St::Ready) {
            if (t->mTimer3 > cfg::kMaxGameCooldown) {
                t->mTimer3 = cfg::kMaxGameCooldown;
            }
            const bool linkReaching = linkAtk && dist < cfg::kLinkReach;
            if (py->checkPlayerGuard() && m.atkCd == 0 && m.armed == 0 && dist < cfg::kBreakerRange &&
                angle_gap(t, toLink) < 0x3800 && rand01() < cfg::kBreakerChance) {
                // Your shield is up: it kicks it down instead of swinging a sword into it.
                if (!lmode) {
                    enter(t, t->mNextBreakPart < 11 ? TN::ACT_ATTACKSHIELDH : TN::ACT_ATTACKH, TN::ACTION2_0_e);
                } else {
                    enter(t, TN::ACT_ATTACKSHIELDL, TN::ACTION2_0_e);
                }
                m.atkCd = lmode ? cfg::kAttackGapL : cfg::kAttackGapH;
                m.chainRolled = false;
                m.speeded = false;
                g_stats.breaker++;
                changed = true;
            } else if (m.atkCd == 0 && m.armed == 0 && (!linkReaching || lmode) &&
                       dist < (lmode ? cfg::kPressureRangeL : cfg::kPressureRange) && angle_gap(t, toLink) < 0x3800 &&
                       rand01() < (lmode ? cfg::kPressureChanceL : cfg::kPressureChance) + 0.2f * g_heat) {
                start_attack(t, m, dist);
                m.chain = 0;
                g_stats.pressure++;
                changed = true;
            } else if (lmode && g_jumpHook && TnInitChaseL::g_orig != nullptr && m.leapCd == 0 && m.atkCd == 0 &&
                       m.armed == 0 && dist > cfg::kLeapMin && dist < cfg::kLeapMax &&
                       angle_gap(t, toLink) < 0x2000 &&
                       rand01() < cfg::kLeapChance * (ranged ? cfg::kRangedLeapChance : cfg::kLeapIdleL)) {
                enter(t, TN::ACT_CHASEL, TN::ACTION2_0_e);
                TnInitChaseL::g_orig(t, dist < 700.0f ? 8 : 17);  // the game's own forward leap
                m.leapCd = ranged ? cfg::kLeapCooldown / 2 : cfg::kLeapCooldown;
                g_stats.leap++;
                changed = true;
            }
        }

        // ---- 6. footwork: turn to you quickly, walk and circle faster (absolute speeds, not stacking) ----
        if (!changed && (st == St::Ready || st == St::Guard) && dist < cfg::kTurnRange && m.expose == 0) {
            turn_toward(t, toLink, cfg::kTurnStep);
        }
        if (!changed && st == St::Ready) {
            const int m2 = t->mActionMode2;
            if (!lmode) {
                if (m2 == TN::ACTION2_9_e && dist > 450.0f) {
                    t->speedF = ranged ? cfg::kRangedWalkH : cfg::kWalkSpeedH;
                } else if (m2 == TN::ACTION2_2_e && dist > 320.0f) {
                    t->speedF = cfg::kApproachSpeedH;
                }
            } else {
                const bool walking = m2 == TN::ACTION2_3_e || m2 == TN::ACTION2_16_e;
                const bool strafing = m2 == TN::ACTION2_4_e || m2 == TN::ACTION2_5_e;
                if (!walking) {
                    m.stutter = 0;  // only the straight approach is broken up; any other state cancels it
                    m.burst = 0;
                }
                if (m.stutter > 0) {
                    // rhythm breaker, part 1: it stops dead
                    t->speedF = 0.0f;
                    if (--m.stutter == 0) {
                        m.burst = cfg::kBurstTicks;
                    }
                } else if (m.burst > 0) {
                    // part 2: it surges at you
                    t->speedF = cfg::kBurstSpeedL;
                    m.burst--;
                } else if (walking && dist > cfg::kStutterMinDist && dist < cfg::kStutterMaxDist &&
                           rand01() < cfg::kStutterChance) {
                    m.stutter = cfg::kStutterMin + static_cast<int>(rand01() * float(cfg::kStutterMax - cfg::kStutterMin + 1));
                    t->speedF = 0.0f;
                    g_stats.stutter++;
                } else if (m2 == TN::ACTION2_3_e && dist > 450.0f) {
                    t->speedF = cfg::kWalkSpeedL;
                } else if (m2 == TN::ACTION2_16_e) {
                    t->speedF = cfg::kWalkSpeedL;
                } else if (strafing) {
                    t->speedF = cfg::kStrafeSpeedL;
                }
            }
        }

        // ---- 6b. harder hits (sword damage only; its health is never touched) ----
        if (st != St::Hurt && st != St::Other) {
            const int atp = std::max(1, static_cast<int>(std::lround(
                lmode ? 2.0f * (cfg::kDamageMultL + cfg::kRageDamage * rage_of(t, m)) : 4.0f * cfg::kDamageMultH)));
            for (int i = 0; i < 4; i++) {
                t->mSwordSphs[i].SetAtAtp(atp);
            }
            t->mCps.SetAtAtp(atp);
        }

        // ---- 6c. no built-in pauses: the game freezes its swing pose for a moment at the end of each attack; skipped ----
        if (lmode && st == St::Attack && (t->mActionMode2 == 101 || t->mActionMode2 == 102) && t->mTimer1 > 0) {
            t->mTimer1 = 0;
        }

        // ---- 7. faster wind-up, once per swing ----
        if (st == St::Attack && !m.speeded && !changed && t->mpModelMorf2 != nullptr &&
            (t->mActionMode2 == 1 || t->mActionMode2 == 2)) {
            m.speeded = true;
            const float speedMult = (lmode ? cfg::kAttackSpeedMultL + cfg::kRageAttackSpeed * rage_of(t, m) : cfg::kAttackSpeedMult) +
                                0.25f * g_heat;
            // mixed timing: some swings come out extra fast, some half a beat late (the hold below)
            const float variant = (lmode && rand01() < cfg::kFastSwingChance) ? cfg::kFastSwingMult : 1.0f;
            t->mpModelMorf2->setPlaySpeed(t->mpModelMorf2->getPlaySpeed() * speedMult * variant);
            // Bait: sometimes hold the swing back in its wind-up (almost stopped, never exactly 0: the game
            // treats a rate of exactly 0 as "animation finished").
            if (!m.holdDone && t->mpModelMorf2->getFrame() < 6.0f &&
                rand01() < (lmode ? cfg::kHoldChanceL : cfg::kHoldChance)) {
                m.holdDone = true;
                m.savedRate = t->mpModelMorf2->getPlaySpeed();
                const int hMin = lmode ? cfg::kHoldMinL : cfg::kHoldMin;
                const int hMax = lmode ? cfg::kHoldMaxL : cfg::kHoldMax;
                m.hold = hMin + static_cast<int>(rand01() * float(hMax - hMin + 1));
                t->mpModelMorf2->setPlaySpeed(0.05f);
                g_stats.held++;
            }
        } else if (st == St::Attack && m.hold > 0 && t->mpModelMorf2 != nullptr) {
            if (--m.hold == 0) {
                t->mpModelMorf2->setPlaySpeed(m.savedRate);
            }
        }

        m.prevDist = dist;
        alive[a] = m;
    }
    if (g_readThisTick && engaged >= 2) {
        for (auto& sp : strikers) {
            auto ai = alive.find(sp.first);
            if (ai != alive.end() && classify(sp.first) == St::Ready && rand01() < cfg::kPackChance) {
                start_attack(sp.first, ai->second, sp.second);
                g_stats.pack++;
            }
        }
    }
    g_mem.swap(alive);

    if (engaged > 0 && ++g_statTicks >= cfg::kStatusTicks) {
        g_statTicks = 0;
        mods::log::info(
            "darknut: n={} reads={} guard={} dodge={} cancelled={} opened={} | its attacks: riposte={} pressure={} "
            "chain={} leap={} counter={} landing={} finish={} zone={} hitrun={} rush={} doubled={} jumpdodge={} | mind games: held={} breaker={} habit={} punish={} pack={} heat={:.2f} rage={:.2f} | your hits landed={} | hooks: hit={} dodge={}",
            engaged, g_stats.read, g_stats.guard, g_stats.evade, g_stats.negated, g_stats.opening,
            g_stats.riposte, g_stats.pressure, g_stats.chain, g_stats.leap, g_stats.counter, g_stats.landing, g_stats.finish, g_stats.zone, g_stats.hitrun, g_stats.rush, g_stats.doubled, g_stats.jumpdodge, g_stats.held, g_stats.breaker,
            g_stats.habit, g_stats.punish, g_stats.pack, g_heat, g_rageSeen, g_stats.landed,
            g_hitHook ? "ON" : "OFF", g_jumpHook ? "ON" : "OFF");
        g_stats = Stats{};
        g_rageSeen = 0.0f;
    } else if (engaged == 0) {
        g_statTicks = 0;
    }
}

}  // namespace

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    if (cfg::kUseHitHook) {
        const ModResult hr = mods::hook::add_pre<AtTgHit>(on_at_tg_hit);
        g_hitHook = (hr == MOD_OK);
    }
    if (cfg::kUseDodgeJumps) {
        const ModResult jr = mods::hook::install<TnInitChaseL>();
        g_jumpHook = (jr == MOD_OK) && TnInitChaseL::g_orig != nullptr;
    }
    mods::log::info(
        "darknut_ruthless v12 initialized | hit-cancel hook {} | dodge-jump hook {} | status line every 5 s "
        "near a Darknut{}",
        cfg::kUseHitHook ? (g_hitHook ? "ON" : "UNAVAILABLE") : "OFF (every hit connects)",
        g_jumpHook ? "ON" : "UNAVAILABLE",
        g_jumpHook ? ""
                   : " | the dodge-jump hook is UNAVAILABLE: your game build has no symbol manifest. Armor-off dodges, "
                     "leaps, zoning, hit-and-run and landing strikes need it; guarding, counters and chaining still work.");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    tick();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_mem.clear();
    g_stats = Stats{};
    g_statTicks = 0;
    g_tick = 0;
    g_histCount = 0;
    g_histHead = 0;
    reset_link_state();
    return MOD_OK;
}
}
