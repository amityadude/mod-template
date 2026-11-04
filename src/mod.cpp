// Enemy Tactics v5 ("ruthless, but safe"): smarter and harder enemies, no extra health.
// v5: every enemy actor was audited by name; Bulblins, Shadow Bulblins, Poes, Poison Mites and bees are now
//     included (gently, with extra gates); newly found bosses and wall/segmented crawlers are excluded.
//
// What changed from v3 (v3 broke some enemies):
//   * No more pushing enemies around. Every movement rule now only amplifies or bends the movement the enemy
//     JUST made on its own (it already passed the game's own wall/floor collision). An enemy that is not
//     moving toward you is never touched, so plants, statues, scripted enemies and knockbacks stay intact.
//   * The Bokoblin "never gives up" leash edit, the Stalfos invulnerability write, and waking sleeping enemies
//     are gone (waking is behind kEnableWake, off by default).
//   * Every enemy in the "enemy" group is included, except the bosses, projectiles, mounted riders and scripted
//     fight actors listed in is_excluded().
//   * The first time each enemy type appears, the log prints its id, so a broken one can be named exactly.
//
// Full tactics (the enemy's own attack/back-off actions): Bokoblin, Lizalfos, Dynalfos, Stalfos.
// Movement rules: every other included enemy.
// 30 ticks = 1 second. All tunables are in namespace cfg.

#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

// Game includes
#include "SSystem/SComponent/c_cc_d.h"
#include "SSystem/SComponent/c_cc_s.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_e_db.h"
#include "d/actor/d_a_e_hb.h"
#include "d/actor/d_a_e_dn.h"
#include "d/actor/d_a_e_mf.h"
#include "d/actor/d_a_e_oc.h"
#include "d/actor/d_a_e_rd.h"
#include "d/actor/d_a_e_sf.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);

// Called by the game for every confirmed overlap of an attack collider and a hurt collider.
DEFINE_HOOK(&cCcS::SetAtTgCommonHitInf, AtTgHit);

namespace cfg {
// --- global
constexpr float kEngageRange = 1500.0f;    // enemies farther than this from Link are ignored
constexpr float kIntensity = 1.0f;         // multiplies every movement rule (0.5 = half as strong)
constexpr bool kEnableWake = false;        // true: idle Bokoblin/Lizalfos/Dynalfos/Stalfos notice Link (see v3)
constexpr int kDesperationLife = 12;       // Link at or below this many life units (12 = 3 hearts)...
constexpr float kDesperationBoost = 1.5f;  // ...makes every rule this much stronger

// --- how movement rules read an enemy's own motion (units per tick)
constexpr float kMinOwnStep = 0.8f;        // slower than this = standing still, never touched
constexpr float kMaxOwnStep = 40.0f;       // faster than this = teleport / projectile, never touched
constexpr float kTowardLinkDot = 0.4f;     // only enemies heading toward Link (within ~66 degrees) are touched
constexpr float kMaxExtra = 8.0f;          // the most any rule set adds to one enemy in one tick

// --- escalation: the longer a fight lasts, the harder they press
constexpr int kEscalationStartTicks = 300; // starts after 10 s of fighting
constexpr int kEscalationFullTicks = 1800; // full strength after 60 s
constexpr float kEscalationMax = 0.5f;     // +50%
constexpr int kCalmResetTicks = 150;       // 5 s with nobody chasing resets it

// --- speed: chasing enemies move this much faster (0.25 = +25%)
constexpr float kSpeedBonus = 0.25f;

// --- lunge: short burst at mid range (their own movement doubled for a moment)
constexpr float kLungeMinDist = 350.0f;
constexpr float kLungeMaxDist = 800.0f;
constexpr float kLungeBonus = 1.0f;        // +100% movement during the burst
constexpr int kLungeTicks = 8;
constexpr float kLungeChance = 0.5f;
constexpr int kLungeCooldown = 150;

// --- anti-flee: you retreat (roll, backflip, run) and they speed up after you
constexpr float kFleeMinSpeed = 3.0f;
constexpr float kFleeMaxSpeed = 40.0f;
constexpr float kFleeBonus = 0.3f;
constexpr float kFleeMinDist = 200.0f;
constexpr float kFleeRange = 700.0f;

// --- you use a ranged item or stand still: they close in faster
constexpr float kRangedBonus = 0.5f;
constexpr float kPlayerStillSpeed = 1.0f;
constexpr int kCampTicks = 60;
constexpr float kCampBonus = 0.3f;

// --- steering: how far an enemy's own heading is bent (radians), and how strongly it is applied
constexpr float kSteerStrength = 0.6f;     // 0.6 * (rotated step - step)
constexpr float kCircleAngle = 0.8f;       // shield up: circle to your side
constexpr float kCircleMinDist = 150.0f;
constexpr float kCircleMaxDist = 600.0f;
constexpr float kAimCosine = 0.94f;        // ranged aim: enemy within 20 degrees of your aim strafes out
constexpr float kAimRange = 1200.0f;
constexpr float kAimAngle = 0.8f;
constexpr float kFlankRadius = 1000.0f;    // 2+ chasers spread around you; one takes your back
constexpr float kFlankAngle = 0.6f;
constexpr float kRearPull = 0.75f;
constexpr float kMaxSteer = 0.9f;

// --- you charge a spin attack -> they interrupt (attack) or back off, 50/50 (4 full-tactics enemies)
constexpr float kChargeReactRange = 450.0f;
constexpr int kChargeCooldown = 150;

// --- opening: your swing, roll or shield drop just ended -> they counter at once
constexpr int kOpenWindow = 20;
constexpr float kCounterRange = 330.0f;
constexpr int kCounterCooldown = 120;

// --- you start a sword swing or jump strike -> they dodge (back-hop)
constexpr float kDodgeRange = 280.0f;
constexpr float kDodgeChance = 0.5f;
constexpr int kDodgeCooldown = 120;

// --- hit and run: after their own attack ends they hop out of your counter range
constexpr float kRetreatChance = 0.3f;
constexpr float kRetreatMaxDist = 300.0f;
constexpr int kRetreatCooldown = 90;

// --- group alert and sense (only when kEnableWake)
constexpr float kAlertRadius = 1000.0f;
constexpr float kAlertMaxHeightDiff = 300.0f;
constexpr float kSenseRadius = 500.0f;
constexpr float kSenseMaxHeightDiff = 200.0f;
constexpr int kWakeCooldown = 30;

// --- evade: an enemy that has noticed you lets some of your hits pass through it (the game's own hit
// detection is told "no hit"; nothing about the enemy is modified)
constexpr float kEvadeMelee = 0.30f;       // chance per sword swing / bash / spin / jump strike
constexpr float kEvadeRanged = 0.20f;      // chance per arrow / boomerang
constexpr float kEvadeMaxChance = 0.6f;    // cap after escalation and desperation boosts
constexpr int kEvadeWindow = 12;           // ticks the whole swing passes through (0.4 s)
constexpr int kEvadeCooldown = 90;         // an enemy cannot dodge twice within 3 s
constexpr int kEvadeDecisionTicks = 20;    // one roll per attack, not one per tick of the swing
constexpr int kComboWindow = 30;           // after one of your hits lands, 1 s with no dodging (combos work)
constexpr float kEvadeAwareRange = 250.0f; // movement-only enemies count as aware inside this range or when chasing
constexpr float kEvadeRootedRange = 350.0f;

// --- plants (Deku Baba, Hebi Baba): real counters using their own attack and hide actions
constexpr float kPlantCounterRange = 500.0f;
constexpr float kPlantMaxHeightDiff = 150.0f;
constexpr short kPlantAttackTimer = 8;     // their attack timer is capped to this (vanilla 30-60)
constexpr int kGuardWaitCooldown = 150;    // shield up: Lizalfos/Dynalfos/Stalfos back off and wait it out

// --- faster attacks and shorter stagger (4 full-tactics enemies)
constexpr float kAttackAnimSpeed = 1.5f;   // 1.0 = vanilla speed
constexpr float kStaggerAnimSpeed = 2.0f;
constexpr short kAttackDecisionTimer = 8;  // Lizalfos/Stalfos attack timer is capped to this (vanilla 10-30)
}  // namespace cfg

// Plant = Deku Baba / Hebi Baba (real counters). Rooted = other rooted or wall enemies (dodge only, never moved).
enum class Kind { None, Generic, Bokoblin, Lizalfos, Dynalfos, Stalfos, Plant, Rooted };

// Things that must not be touched. Every enemy actor in the game was checked by name (see the table in the
// reply): bosses and mini-bosses, scripted fights, projectiles and props, plants that are rooted to the
// ground, and enemies that crawl on walls, rails or segmented bodies, where moving only the head would tear
// them apart.
static bool is_excluded(int prof) {
    switch (prof) {
    // bosses, mini-bosses and boss parts
    case fpcNm_E_GOB_e:      // Dangoro
    case fpcNm_E_FM_e:       // Fyrus
    case fpcNm_E_VT_e:       // Death Sword
    case fpcNm_E_MK_e:       // Ook
    case fpcNm_E_MK_BO_e:    // Ook's boomerang
    case fpcNm_E_MB_e:       // Ook (Diababa fight)
    case fpcNm_E_GM_e:       // Baby Gohma / Gohma Eye
    case fpcNm_E_IS_e:       // Armos Titan
    case fpcNm_E_FK_e:       // Phantom Rider
    case fpcNm_E_HP_e:       // Huge Poe
    case fpcNm_E_OctBg_e:    // Morpheel bomb fish
    case fpcNm_E_YR_e:       // Twilight Kargorok rider
    case fpcNm_E_FS_e:       // Wooden Puppet (Puppet Zelda fight)
    // segmented / part actors
    case fpcNm_E_SW_e:       // Moldorm
    case fpcNm_E_MM_MT_e:    // Helmasaur shell part
    case fpcNm_E_DF_e:       // Deku Flower
    case fpcNm_E_GA_e:       // Decorative Moth
    case fpcNm_B_BH_e:
    case fpcNm_B_BQ_e:
    case fpcNm_B_DRE_e:
    case fpcNm_B_DR_e:
    case fpcNm_B_DS_e:
    case fpcNm_B_GG_e:
    case fpcNm_B_GM_e:
    case fpcNm_B_GND_e:
    case fpcNm_B_GOS_e:
    case fpcNm_B_GO_e:
    case fpcNm_B_MGN_e:
    case fpcNm_B_OB_e:
    case fpcNm_B_OH2_e:
    case fpcNm_B_OH_e:
    case fpcNm_B_TN_e:
    case fpcNm_B_YOI_e:
    case fpcNm_B_YO_e:
    case fpcNm_B_ZANTM_e:
    case fpcNm_B_ZANTS_e:
    case fpcNm_B_ZANTZ_e:
    case fpcNm_B_ZANT_e:
    // projectiles, eggs, leaves, lines, balls, tags
    case fpcNm_E_ARROW_e:
    case fpcNm_E_BI_LEAF_e:
    case fpcNm_E_DB_LEAF_e:
    case fpcNm_E_HB_LEAF_e:
    case fpcNm_E_YD_LEAF_e:
    case fpcNm_E_CR_EGG_e:
    case fpcNm_E_NEST_e:
    case fpcNm_E_ST_LINE_e:
    case fpcNm_E_TH_BALL_e:
    case fpcNm_E_TK_BALL_e:
    case fpcNm_E_YM_TAG_e:
    case fpcNm_E_WAP_e:
    // King Bulblin and the boar (Bulblin riders E_RD and Shadow Bulblins E_RDY are included, see scale_of)
    case fpcNm_E_RDB_e:
    case fpcNm_E_WB_e:
    // scripted fight actors
    case fpcNm_E_HZELDA_e:
    case fpcNm_E_PM_e:
    case fpcNm_E_PZ_e:
    case fpcNm_E_ZM_e:
    case fpcNm_E_ZH_e:
    case fpcNm_E_MD_e:
    case fpcNm_E_YM_e:
    case fpcNm_E_YMB_e:
        return true;
    default: return false;
    }
}

// How strongly the movement rules apply to an enemy type (1.0 = as written). Bulblins, Poes and bees are
// included but gentler, because they are mounted, phase in and out, or come in swarms.
static float scale_of(int prof) {
    switch (prof) {
    case fpcNm_E_RD_e:   // Bulblin
    case fpcNm_E_RDY_e:  // Shadow Bulblin
    case fpcNm_E_PO_e:   // Poe
        return 0.6f;
    case fpcNm_E_BEE_e:  // Bee
        return 0.5f;
    default: return 1.0f;
    }
}

// Extra per-type gate on when the movement rules may touch an enemy.
static bool movement_ok(int prof, fopAc_ac_c* a) {
    if (prof == fpcNm_E_RD_e) {
        // Only while a Bulblin is running at Link on foot (never while riding a boar, or in scripted states).
        const int act = reinterpret_cast<e_rd_class*>(a)->action;
        return act == 3 || act == 5;  // ACTION_FIGHT_RUN, ACTION_BOW_RUN
    }
    return true;
}

static Kind kind_of(fopAc_ac_c* a) {
    const int prof = fopAcM_GetProfName(a);
    // Poison Mites and bees are not in the game's "enemy" group, but they are enemies.
    const bool actorGroupEnemy = a->group == fopAc_ACTOR_e && (prof == fpcNm_E_BUG_e || prof == fpcNm_E_BEE_e);
    if (a->group != fopAc_ENEMY_e && !actorGroupEnemy) {
        return Kind::None;
    }
    if (is_excluded(prof)) {
        return Kind::None;
    }
    switch (prof) {
    case fpcNm_E_OC_e: return Kind::Bokoblin;
    case fpcNm_E_DN_e: return Kind::Lizalfos;
    case fpcNm_E_MF_e: return Kind::Dynalfos;
    case fpcNm_E_SF_e: return Kind::Stalfos;
    case fpcNm_E_DB_e:   // Deku Baba
    case fpcNm_E_HB_e:   // Hebi Baba
        return Kind::Plant;
    case fpcNm_E_GB_e:   // Giant Baba
    case fpcNm_E_YD_e:   // Twilight Deku Baba
    case fpcNm_E_YH_e:   // Twilight Hebi Baba
    case fpcNm_E_WS_e:   // Wall Skulltula
    case fpcNm_E_SM_e:   // Chu Worm
        return Kind::Rooted;
    default: return Kind::Generic;
    }
}

// Types too big or too clumsy to dodge: they still get every other rule.
static bool can_evade_type(int prof) {
    switch (prof) {
    case fpcNm_E_AI_e:   // Armos
    case fpcNm_E_DD_e:   // Dodongo
    case fpcNm_E_TH_e:   // Darkhammer
    case fpcNm_E_FB_e:   // Freezard
    case fpcNm_E_BEE_e:  // Bee
        return false;
    default: return true;
    }
}

// Action ids copied from the game source (they are enums local to each .cpp, not in headers).
namespace oc {  // src/d/actor/d_a_e_oc.cpp (Bokoblin)
constexpr int WAIT = 0;
constexpr int WALK = 1;
constexpr int FIND = 3;
constexpr int ATTACK = 4;
constexpr int DAMAGE = 5;
constexpr int BIG_DAMAGE = 6;
constexpr int WATCH = 7;
constexpr int SOUND_WATCH = 8;
}  // namespace oc
namespace dn {  // src/d/actor/d_a_e_dn.cpp (Lizalfos)
constexpr int NORMAL = 0;
constexpr int FIGHT_RUN = 3;
constexpr int ATTACK_0 = 4;
constexpr int ATTACK = 5;
constexpr int TAIL_ATTACK = 6;
constexpr int GUARD = 7;
constexpr int DRAWBACK = 12;
constexpr int S_DAMAGE = 20;
constexpr int DAMAGE = 21;
}  // namespace dn
namespace mf {  // src/d/actor/d_a_e_mf.cpp (Dynalfos)
constexpr int NORMAL = 0;
constexpr int FIGHT_RUN = 3;
constexpr int ATTACK = 5;
constexpr int TAIL_ATTACK = 6;
constexpr int GUARD = 7;
constexpr int DRAWBACK = 12;
constexpr int S_DAMAGE = 20;
constexpr int DAMAGE = 21;
}  // namespace mf
namespace baba {  // src/d/actor/d_a_e_db.cpp and d_a_e_hb.cpp (Deku Baba, Hebi Baba share these)
constexpr int STAY = 0;    // hidden / hiding (mode 2 = start hiding)
constexpr int WAIT = 3;    // swaying; mode 1 = ready to attack
constexpr int ATTACK = 4;  // lunge (mode 0 = start)
constexpr int CHANCE = 6;
}  // namespace baba
namespace sf {  // src/d/actor/d_a_e_sf.cpp (Stalfos)
constexpr int NORMAL = 0;
constexpr int FIGHT_RUN = 3;
constexpr int ATTACK_0 = 4;
constexpr int ATTACK = 5;
constexpr int GUARD = 7;
constexpr int DRAWBACK = 8;
constexpr int S_DAMAGE = 20;
}  // namespace sf

struct EnemyMemory {
    int dodgeCd = 0;
    int chargeCd = 0;
    int counterCd = 0;
    int lungeCd = 0;
    int lungeTicks = 0;
    int retreatCd = 0;
    int wakeCd = 0;
    bool wasAttacking = false;
    int guardCd = 0;

    // evade (see on_at_tg_pre)
    bool canEvade = false;     // aware of Link, evadable type
    int evadeTicks = 0;        // > 0: your hits pass through this enemy
    int evadeCd = 0;
    int noEvadeTicks = 0;      // > 0: just got hit, no dodging
    int decisionCd = 0;
    bool pendingEvade = false; // a dodge just happened: play the enemy's own hop / hide
    int awareTicks = 0;

    bool haveLast = false;  // position after our last edit, to measure the enemy's own step
    float lastX = 0.0f;
    float lastZ = 0.0f;

    bool active = false;    // this tick: moving toward Link on its own, so movement rules apply
    float dx = 0.0f;        // its own step this tick
    float dz = 0.0f;
    float scale = 1.0f;     // per-type strength (see scale_of)
    float speedK = 0.0f;    // extra fraction of that step to add
    float steer = 0.0f;     // radians to bend that step
};

static std::unordered_map<const void*, EnemyMemory> g_memory;
static std::unordered_set<int> g_seenProcs;
static bool g_prevSwinging = false;
static bool g_prevRolling = false;
static bool g_prevJumpStrike = false;
static bool g_prevGuard = false;
static int g_openWindow = 0;
static int g_stillTicks = 0;
static int g_fightTicks = 0;
static int g_calmTicks = 0;
static bool g_anyChaser = false;
static bool g_havePrevPos = false;
static float g_prevX = 0.0f;
static float g_prevZ = 0.0f;
static uint32_t g_rng = 0x2545F491u;
static fopAc_ac_c* g_linkActor = nullptr;  // null while events run: no dodging then
static float g_press = 1.0f;               // escalation * desperation, read by the hit hook

constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = 6.28318531f;
constexpr float kAngleToRad = kPi / 32768.0f;  // game angles: 65536 = full circle

static float rand01() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (g_rng & 0xFFFFFF) / float(0x1000000);
}

static float clampf(float x, float lo, float hi) {
    return std::max(lo, std::min(hi, x));
}

static float wrap_pi(float x) {
    while (x > kPi) x -= kTwoPi;
    while (x <= -kPi) x += kTwoPi;
    return x;
}

static float dist_xz(const fopAc_ac_c* a, const fopAc_ac_c* b) {
    const float dx = a->current.pos.x - b->current.pos.x;
    const float dz = a->current.pos.z - b->current.pos.z;
    return std::sqrt(dx * dx + dz * dz);
}

// True while the enemy is in its "run at Link" state, the safe moment to inject a new action.
static bool is_chasing(Kind k, fopAc_ac_c* a) {
    switch (k) {
    case Kind::Bokoblin: return reinterpret_cast<daE_OC_c*>(a)->mActionMode == oc::FIND;
    case Kind::Lizalfos: return reinterpret_cast<e_dn_class*>(a)->action == dn::FIGHT_RUN;
    case Kind::Dynalfos: return reinterpret_cast<e_mf_class*>(a)->mAction == mf::FIGHT_RUN;
    case Kind::Stalfos: return reinterpret_cast<e_sf_class*>(a)->mAction == sf::FIGHT_RUN;
    default: return false;
    }
}

// Running one of its own attacks.
static bool is_attacking(Kind k, fopAc_ac_c* a) {
    switch (k) {
    case Kind::Bokoblin: return reinterpret_cast<daE_OC_c*>(a)->mActionMode == oc::ATTACK;
    case Kind::Lizalfos: {
        const int m = reinterpret_cast<e_dn_class*>(a)->action;
        return m == dn::ATTACK_0 || m == dn::ATTACK || m == dn::TAIL_ATTACK;
    }
    case Kind::Dynalfos: {
        const int m = reinterpret_cast<e_mf_class*>(a)->mAction;
        return m == mf::ATTACK || m == mf::TAIL_ATTACK;
    }
    case Kind::Stalfos: {
        const int m = reinterpret_cast<e_sf_class*>(a)->mAction;
        return m == sf::ATTACK_0 || m == sf::ATTACK;
    }
    default: return false;
    }
}

// Chasing or already attacking/guarding: this enemy is "in the fight".
static bool is_fighting(Kind k, fopAc_ac_c* a) {
    switch (k) {
    case Kind::Bokoblin: {
        const int m = reinterpret_cast<daE_OC_c*>(a)->mActionMode;
        return m == oc::FIND || m == oc::ATTACK;
    }
    case Kind::Lizalfos: {
        const int m = reinterpret_cast<e_dn_class*>(a)->action;
        return m == dn::FIGHT_RUN || m == dn::ATTACK_0 || m == dn::ATTACK || m == dn::TAIL_ATTACK ||
               m == dn::GUARD;
    }
    case Kind::Dynalfos: {
        const int m = reinterpret_cast<e_mf_class*>(a)->mAction;
        return m == mf::FIGHT_RUN || m == mf::ATTACK || m == mf::TAIL_ATTACK || m == mf::GUARD;
    }
    case Kind::Stalfos: {
        const int m = reinterpret_cast<e_sf_class*>(a)->mAction;
        return m == sf::FIGHT_RUN || m == sf::ATTACK_0 || m == sf::ATTACK || m == sf::GUARD;
    }
    default: return false;
    }
}

// Idle (not yet fighting). Only used when kEnableWake is on.
static bool is_passive(Kind k, fopAc_ac_c* a) {
    switch (k) {
    case Kind::Bokoblin: {
        const int m = reinterpret_cast<daE_OC_c*>(a)->mActionMode;
        return m == oc::WAIT || m == oc::WALK || m == oc::WATCH || m == oc::SOUND_WATCH;
    }
    case Kind::Lizalfos: return reinterpret_cast<e_dn_class*>(a)->action == dn::NORMAL;
    case Kind::Dynalfos: return reinterpret_cast<e_mf_class*>(a)->mAction == mf::NORMAL;
    case Kind::Stalfos: return reinterpret_cast<e_sf_class*>(a)->mAction == sf::NORMAL;
    default: return false;
    }
}

static bool can_back_off(Kind k) {
    return k == Kind::Lizalfos || k == Kind::Dynalfos || k == Kind::Stalfos;
}

// The four enemies with full action-level tactics.
static bool is_adapter(Kind k) {
    return k == Kind::Bokoblin || k == Kind::Lizalfos || k == Kind::Dynalfos || k == Kind::Stalfos;
}

// Deku Baba / Hebi Baba share this layout; both are plain structs starting with the enemy base.
struct BabaView {
    s16* action;
    s16* mode;
    s16* timers;  // timers[1] counts down to the next attack
};

static BabaView baba_of(fopAc_ac_c* a) {
    if (fopAcM_GetProfName(a) == fpcNm_E_DB_e) {
        auto* e = reinterpret_cast<e_db_class*>(a);
        return {&e->action, &e->mode, e->timers};
    }
    auto* e = reinterpret_cast<e_hb_class*>(a);
    return {&e->action, &e->mode, e->timers};
}

// Swaying and ready to attack: the only state we touch.
static bool baba_ready(fopAc_ac_c* a) {
    const BabaView v = baba_of(a);
    return *v.action == baba::WAIT && *v.mode == 1;
}

// Out and in the fight (not hiding, not appearing, not damaged).
static bool baba_fighting(fopAc_ac_c* a) {
    const int act = *baba_of(a).action;
    return act == baba::WAIT || act == baba::ATTACK || act == baba::CHANCE;
}

static bool baba_dk_caught(daPy_py_c* py) {
    return py->getDkCaught() || py->getDkCaught2();
}

// Put an idle enemy into its own chase state.
static void wake_up(Kind k, fopAc_ac_c* a) {
    switch (k) {
    case Kind::Bokoblin: {
        auto* e = reinterpret_cast<daE_OC_c*>(a);
        e->mOldActionMode = e->mActionMode;
        e->mActionMode = oc::FIND;
        e->mOcState = 0;
        e->field_0x6c0 = 0;
        e->field_0x6c2 = 0;
        break;
    }
    case Kind::Lizalfos: {
        auto* e = reinterpret_cast<e_dn_class*>(a);
        e->action = dn::FIGHT_RUN;
        e->mode = 0;
        break;
    }
    case Kind::Dynalfos: {
        auto* e = reinterpret_cast<e_mf_class*>(a);
        e->mAction = mf::FIGHT_RUN;
        e->field_0x5b4 = 0;
        break;
    }
    case Kind::Stalfos: {
        auto* e = reinterpret_cast<e_sf_class*>(a);
        e->mAction = sf::FIGHT_RUN;
        e->mActionPhase = 0;
        break;
    }
    default: break;
    }
}

// Make the enemy start its own attack aimed at Link.
static void start_attack(Kind k, fopAc_ac_c* a, float dist) {
    switch (k) {
    case Kind::Bokoblin: {
        auto* e = reinterpret_cast<daE_OC_c*>(a);
        e->mOldActionMode = e->mActionMode;
        e->mActionMode = oc::ATTACK;
        e->mOcState = 0;
        e->field_0x6c0 = 0;
        e->field_0x6c2 = 0;
        break;
    }
    case Kind::Lizalfos: {
        auto* e = reinterpret_cast<e_dn_class*>(a);
        e->action = dist < 250.0f ? dn::ATTACK_0 : dn::ATTACK;
        e->mode = 0;
        break;
    }
    case Kind::Dynalfos: {
        auto* e = reinterpret_cast<e_mf_class*>(a);
        e->mAction = mf::ATTACK;
        e->field_0x5b4 = 0;
        break;
    }
    case Kind::Stalfos: {
        auto* e = reinterpret_cast<e_sf_class*>(a);
        e->mAction = (e->mType == 0) ? sf::ATTACK : sf::ATTACK_0;
        e->mActionPhase = 0;
        break;
    }
    default: break;
    }
}

// Make the enemy do its own back-hop away from Link.
static void start_back_off(Kind k, fopAc_ac_c* a) {
    switch (k) {
    case Kind::Lizalfos: {
        auto* e = reinterpret_cast<e_dn_class*>(a);
        e->action = dn::DRAWBACK;
        e->mode = 0;
        break;
    }
    case Kind::Dynalfos: {
        auto* e = reinterpret_cast<e_mf_class*>(a);
        e->mAction = mf::DRAWBACK;
        e->field_0x5b4 = 0;
        break;
    }
    case Kind::Stalfos: {
        auto* e = reinterpret_cast<e_sf_class*>(a);
        e->mAction = sf::DRAWBACK;
        e->mActionPhase = 0;
        break;
    }
    default: break;
    }
}

// Faster attacks, shorter stagger, faster attack decisions.
static void tune_reactions(Kind k, fopAc_ac_c* a) {
    switch (k) {
    case Kind::Bokoblin: {
        auto* e = reinterpret_cast<daE_OC_c*>(a);
        if (e->mpMorf) {
            if (e->mActionMode == oc::ATTACK) {
                e->mpMorf->setPlaySpeed(cfg::kAttackAnimSpeed);
            } else if (e->mActionMode == oc::DAMAGE || e->mActionMode == oc::BIG_DAMAGE) {
                e->mpMorf->setPlaySpeed(cfg::kStaggerAnimSpeed);
            }
        }
        break;
    }
    case Kind::Lizalfos: {
        auto* e = reinterpret_cast<e_dn_class*>(a);
        if (e->anm_p) {
            if (e->action == dn::ATTACK_0 || e->action == dn::ATTACK || e->action == dn::TAIL_ATTACK) {
                e->anm_p->setPlaySpeed(cfg::kAttackAnimSpeed);
            } else if (e->action == dn::S_DAMAGE || e->action == dn::DAMAGE) {
                e->anm_p->setPlaySpeed(cfg::kStaggerAnimSpeed);
            }
        }
        if (e->action == dn::FIGHT_RUN && e->timer[2] > cfg::kAttackDecisionTimer) {
            e->timer[2] = cfg::kAttackDecisionTimer;
        }
        break;
    }
    case Kind::Dynalfos: {
        auto* e = reinterpret_cast<e_mf_class*>(a);
        if (e->mpModelMorf) {
            if (e->mAction == mf::ATTACK || e->mAction == mf::TAIL_ATTACK) {
                e->mpModelMorf->setPlaySpeed(cfg::kAttackAnimSpeed);
            } else if (e->mAction == mf::S_DAMAGE || e->mAction == mf::DAMAGE) {
                e->mpModelMorf->setPlaySpeed(cfg::kStaggerAnimSpeed);
            }
        }
        break;
    }
    case Kind::Stalfos: {
        auto* e = reinterpret_cast<e_sf_class*>(a);
        if (e->mpModelMorf) {
            if (e->mAction == sf::ATTACK || e->mAction == sf::ATTACK_0) {
                e->mpModelMorf->setPlaySpeed(cfg::kAttackAnimSpeed);
            } else if (e->mAction == sf::S_DAMAGE) {
                e->mpModelMorf->setPlaySpeed(cfg::kStaggerAnimSpeed);
            }
        }
        if (e->mAction == sf::FIGHT_RUN && e->mTimers[2] > cfg::kAttackDecisionTimer) {
            e->mTimers[2] = cfg::kAttackDecisionTimer;
        }
        break;
    }
    default: break;
    }
}

static int collect_enemies(void* actor, void* data) {
    auto* list = static_cast<std::vector<fopAc_ac_c*>*>(data);
    auto* a = static_cast<fopAc_ac_c*>(actor);
    if (kind_of(a) != Kind::None) {
        list->push_back(a);
    }
    return 1;
}

struct Chaser {
    fopAc_ac_c* actor;
    EnemyMemory* mem;
    float angle;  // angle around Link
};

static void reset_player_state() {
    g_prevSwinging = g_prevRolling = g_prevJumpStrike = g_prevGuard = false;
    g_havePrevPos = false;
}

static void tactics_tick() {
    fopAc_ac_c* plr = dComIfGp_getPlayer(0);
    if (plr == nullptr || dComIfGp_event_runCheck()) {
        reset_player_state();
        g_memory.clear();  // positions change during events; measure fresh afterwards
        return;
    }

    auto* py = static_cast<daPy_py_c*>(plr);
    auto* link = static_cast<daAlink_c*>(plr);

    const bool guard = py->checkPlayerGuard();
    const bool charging = py->checkCutCharge() || py->checkCutTurnCharge();
    const bool swinging = py->getCutAtFlg();
    const bool rolling = py->checkFrontRoll();
    const bool jumpStrike = py->checkCutJumpMode();
    const u16 proc = link->mProcID;
    const bool ranged = proc == daAlink_c::PROC_BOW_SUBJECT || proc == daAlink_c::PROC_BOW_MOVE ||
                        proc == daAlink_c::PROC_BOOMERANG_SUBJECT ||
                        proc == daAlink_c::PROC_BOOMERANG_MOVE ||
                        proc == daAlink_c::PROC_HOOKSHOT_SUBJECT ||
                        proc == daAlink_c::PROC_HOOKSHOT_MOVE;
    const bool still = std::fabs(plr->speedF) < cfg::kPlayerStillSpeed;
    g_stillTicks = still ? g_stillTicks + 1 : 0;
    const bool camping = g_stillTicks >= cfg::kCampTicks;

    // Link's real movement this tick (works for every roll, flip and run, whatever the action).
    float vx = 0.0f;
    float vz = 0.0f;
    if (g_havePrevPos) {
        vx = plr->current.pos.x - g_prevX;
        vz = plr->current.pos.z - g_prevZ;
    }
    g_prevX = plr->current.pos.x;
    g_prevZ = plr->current.pos.z;
    g_havePrevPos = true;
    float lspeed = std::sqrt(vx * vx + vz * vz);
    if (lspeed > 100.0f) {  // teleport or room change, not movement
        vx = vz = lspeed = 0.0f;
    }

    // Escalation: the longer the fight lasts, the harder everyone presses.
    if (g_anyChaser) {
        g_fightTicks++;
        g_calmTicks = 0;
    } else if (++g_calmTicks >= cfg::kCalmResetTicks) {
        g_fightTicks = 0;
    }
    g_anyChaser = false;
    const float escalation =
        clampf(float(g_fightTicks - cfg::kEscalationStartTicks) /
                   float(cfg::kEscalationFullTicks - cfg::kEscalationStartTicks),
               0.0f, 1.0f) *
        cfg::kEscalationMax;

    // Desperation: Link is low on health, every enemy presses harder.
    const float boost = (dComIfGs_getLife() <= cfg::kDesperationLife) ? cfg::kDesperationBoost : 1.0f;
    const float press = boost * (1.0f + escalation);  // multiplies chances
    const float power = cfg::kIntensity * press;      // multiplies movement rules

    // Dodge triggers (rising edges) and counter window (falling edges of swing / roll / guard).
    const bool dodgeTrigger = (swinging && !g_prevSwinging) || (jumpStrike && !g_prevJumpStrike);
    if ((!swinging && g_prevSwinging) || (!rolling && g_prevRolling) || (!guard && g_prevGuard)) {
        g_openWindow = cfg::kOpenWindow;
    } else if (g_openWindow > 0) {
        g_openWindow--;
    }
    g_prevSwinging = swinging;
    g_prevRolling = rolling;
    g_prevJumpStrike = jumpStrike;
    g_prevGuard = guard;

    const float facing = plr->shape_angle.y * kAngleToRad;
    const float fx = std::sin(facing);  // Link's forward direction in the ground plane
    const float fz = std::cos(facing);

    std::vector<fopAc_ac_c*> all;
    fopAcIt_Executor(collect_enemies, &all);

    // Keep only living, standing enemies near Link; forget memory of enemies that are gone.
    std::vector<fopAc_ac_c*> engaged;
    std::unordered_map<const void*, EnemyMemory> alive;
    for (fopAc_ac_c* a : all) {
        if (dist_xz(a, plr) > cfg::kEngageRange) {
            continue;
        }
        // Poison Mites and bees are plain actors, not built on the enemy base class: no dead/down flags.
        if (a->group == fopAc_ENEMY_e) {
            auto* en = static_cast<fopEn_enemy_c*>(a);
            if (en->checkDeadFlg() || en->checkDownFlg()) {
                continue;
            }
        }
        auto it = g_memory.find(a);
        alive[a] = (it != g_memory.end()) ? it->second : EnemyMemory{};
        engaged.push_back(a);

        const int prof = fopAcM_GetProfName(a);
        if (g_seenProcs.insert(prof).second) {
            mods::log::info("enemy_tactics: new enemy type, proc id {} ({})", prof,
                            is_adapter(kind_of(a)) ? "full tactics" : "movement only");
        }
    }
    g_memory.swap(alive);

    // Optional: wake idle enemies of the four full-tactics types (off by default, see kEnableWake).
    if (cfg::kEnableWake) {
        for (fopAc_ac_c* p : engaged) {
            const Kind pk = kind_of(p);
            EnemyMemory& pm = g_memory[p];
            if (pm.wakeCd > 0) {
                pm.wakeCd--;
                continue;
            }
            if (!is_adapter(pk) || !is_passive(pk, p)) {
                continue;
            }
            bool wake = dist_xz(p, plr) < cfg::kSenseRadius &&
                        std::fabs(p->current.pos.y - plr->current.pos.y) < cfg::kSenseMaxHeightDiff;
            if (!wake) {
                for (fopAc_ac_c* f : engaged) {
                    if (f == p) {
                        continue;
                    }
                    const Kind fk = kind_of(f);
                    if (is_adapter(fk) && is_fighting(fk, f) && dist_xz(p, f) < cfg::kAlertRadius &&
                        std::fabs(p->current.pos.y - f->current.pos.y) < cfg::kAlertMaxHeightDiff) {
                        wake = true;
                        break;
                    }
                }
            }
            if (wake) {
                wake_up(pk, p);
                pm.wakeCd = cfg::kWakeCooldown;
            }
        }
    }

    std::vector<Chaser> chasers;
    for (fopAc_ac_c* a : engaged) {
        const Kind k = kind_of(a);
        EnemyMemory& m = g_memory[a];
        m.active = false;
        m.speedK = 0.0f;
        m.steer = 0.0f;
        if (m.dodgeCd > 0) m.dodgeCd--;
        if (m.chargeCd > 0) m.chargeCd--;
        if (m.counterCd > 0) m.counterCd--;
        if (m.lungeCd > 0) m.lungeCd--;
        if (m.retreatCd > 0) m.retreatCd--;

        const float dist = dist_xz(a, plr);
        const bool adapter = is_adapter(k);

        // ---- full-tactics enemies: use the enemy's own attack / back-off actions ----
        if (adapter) {
            tune_reactions(k, a);

            const bool attacking = is_attacking(k, a);
            const bool chasing = is_chasing(k, a);

            // Hit and run: their attack just ended and they are back in chase mode, hop out of range.
            if (m.wasAttacking && !attacking && chasing && can_back_off(k) && m.retreatCd == 0 &&
                dist < cfg::kRetreatMaxDist && rand01() < std::min(1.0f, cfg::kRetreatChance * press)) {
                m.wasAttacking = false;
                m.retreatCd = cfg::kRetreatCooldown;
                start_back_off(k, a);
                continue;
            }
            m.wasAttacking = attacking;

            if (chasing) {
                if (dodgeTrigger && can_back_off(k) && dist < cfg::kDodgeRange && m.dodgeCd == 0 &&
                    rand01() < std::min(1.0f, cfg::kDodgeChance * press)) {
                    start_back_off(k, a);
                    m.dodgeCd = cfg::kDodgeCooldown;
                    continue;
                }
                if (charging && dist < cfg::kChargeReactRange && m.chargeCd == 0) {
                    m.chargeCd = cfg::kChargeCooldown;
                    if (can_back_off(k) && rand01() < 0.5f) {
                        start_back_off(k, a);
                    } else {
                        start_attack(k, a, dist);
                    }
                    continue;
                }
                if (g_openWindow > 0 && dist < cfg::kCounterRange && m.counterCd == 0) {
                    m.counterCd = cfg::kCounterCooldown;
                    start_attack(k, a, dist);
                    continue;
                }
            }
            if (!is_fighting(k, a)) {
                m.lungeTicks = 0;
                continue;  // idle: movement rules never touch it
            }
        }

        // ---- movement rules (all enemies): read the enemy's own step since we last looked ----
        const int prof = fopAcM_GetProfName(a);
        if (!m.haveLast || dist < 1.0f || !movement_ok(prof, a)) {
            continue;
        }
        m.scale = scale_of(prof);
        const float sx = a->current.pos.x - m.lastX;
        const float sz = a->current.pos.z - m.lastZ;
        const float step = std::sqrt(sx * sx + sz * sz);
        if (step < cfg::kMinOwnStep || step > cfg::kMaxOwnStep) {
            m.lungeTicks = 0;
            continue;
        }
        const float ux = (a->current.pos.x - plr->current.pos.x) / dist;  // Link -> enemy
        const float uz = (a->current.pos.z - plr->current.pos.z) / dist;
        // Only an enemy that is walking/running toward Link is touched (not knockback, not wandering off).
        if ((sx * -ux + sz * -uz) / step < cfg::kTowardLinkDot) {
            m.lungeTicks = 0;
            continue;
        }
        m.active = true;
        m.dx = sx;
        m.dz = sz;
        g_anyChaser = true;
        const float side = ((reinterpret_cast<uintptr_t>(a) >> 4) & 1) ? 1.0f : -1.0f;  // fixed per enemy

        float k_speed = cfg::kSpeedBonus;

        // Lunge: a short burst from mid range.
        if (m.lungeTicks > 0) {
            m.lungeTicks--;
            k_speed += cfg::kLungeBonus;
        } else if (m.lungeCd == 0 && dist > cfg::kLungeMinDist && dist < cfg::kLungeMaxDist) {
            m.lungeCd = cfg::kLungeCooldown;
            if (rand01() < std::min(1.0f, cfg::kLungeChance * press)) {
                m.lungeTicks = cfg::kLungeTicks;
            }
        }

        // Anti-flee: Link is moving away from this enemy.
        if (lspeed >= cfg::kFleeMinSpeed && lspeed <= cfg::kFleeMaxSpeed && dist > cfg::kFleeMinDist &&
            dist < cfg::kFleeRange && -(vx * ux + vz * uz) > 0.5f * lspeed) {
            k_speed += cfg::kFleeBonus;
        }

        // Ranged item or camping: close in faster.
        if (ranged && still) {
            k_speed += cfg::kRangedBonus;
        } else if (camping) {
            k_speed += cfg::kCampBonus;
        }

        // Steering: shield up -> circle to your side; aiming -> strafe out of the line of fire.
        float steer = 0.0f;
        if (guard && dist > cfg::kCircleMinDist && dist < cfg::kCircleMaxDist) {
            steer = side * cfg::kCircleAngle;
        }
        if (ranged && dist < cfg::kAimRange && (ux * fx + uz * fz) > cfg::kAimCosine) {
            steer = side * cfg::kAimAngle;
        }

        m.speedK = k_speed;
        m.steer = steer;

        if (dist < cfg::kFlankRadius) {
            const float ddx = a->current.pos.x - plr->current.pos.x;
            const float ddz = a->current.pos.z - plr->current.pos.z;
            chasers.push_back({a, &m, std::atan2(ddz, ddx)});
        }
    }

    // Surround: bend the chasers' own headings so they spread around Link, one of them taking his back.
    if (chasers.size() >= 2) {
        std::sort(chasers.begin(), chasers.end(),
                  [](const Chaser& l, const Chaser& r) { return l.angle < r.angle; });
        const size_t n = chasers.size();
        const float ideal = kTwoPi / float(n);
        const float rearAngle = std::atan2(-fz, -fx);  // direction from Link to the point behind him
        size_t rearIdx = 0;
        float rearRel = kTwoPi;
        for (size_t i = 0; i < n; i++) {
            const float rel = wrap_pi(chasers[i].angle - rearAngle);
            if (std::fabs(rel) < std::fabs(rearRel)) {
                rearRel = rel;
                rearIdx = i;
            }
        }
        for (size_t i = 0; i < n; i++) {
            const Chaser& cur = chasers[i];
            const Chaser& next = chasers[(i + 1) % n];
            const Chaser& prev = chasers[(i + n - 1) % n];
            float gapNext = next.angle - cur.angle;
            if (gapNext <= 0.0f) gapNext += kTwoPi;
            float gapPrev = cur.angle - prev.angle;
            if (gapPrev <= 0.0f) gapPrev += kTwoPi;
            float push = clampf((gapNext - gapPrev) / (2.0f * ideal), -1.0f, 1.0f);
            if (i == rearIdx) {
                push += clampf(-rearRel, -1.0f, 1.0f) * cfg::kRearPull;
            }
            push = clampf(push, -1.5f, 1.5f);
            // Rotating a step that points at Link by +theta moves the enemy to a SMALLER angle around
            // him, so the sign is flipped: positive push = larger angle.
            if (cur.mem->steer == 0.0f) {
                cur.mem->steer = -push * cfg::kFlankAngle;
            }
        }
    }

    // Apply: each enemy that moved toward Link on its own gets a bounded extra share of that same step.
    for (fopAc_ac_c* a : engaged) {
        EnemyMemory& m = g_memory[a];
        if (m.active) {
            const float theta = clampf(m.steer, -cfg::kMaxSteer, cfg::kMaxSteer);
            const float c = std::cos(theta);
            const float s = std::sin(theta);
            // Own step, rotated by theta.
            const float rx = m.dx * c - m.dz * s;
            const float rz = m.dx * s + m.dz * c;
            const float p = power * m.scale;
            const float k = clampf(m.speedK * p, 0.0f, 2.0f);
            float ex = m.dx * k + (rx - m.dx) * cfg::kSteerStrength * p;
            float ez = m.dz * k + (rz - m.dz) * cfg::kSteerStrength * p;
            const float mag = std::sqrt(ex * ex + ez * ez);
            if (mag > cfg::kMaxExtra) {
                ex *= cfg::kMaxExtra / mag;
                ez *= cfg::kMaxExtra / mag;
            }
            a->current.pos.x += ex;
            a->current.pos.z += ez;
        }
        m.haveLast = true;
        m.lastX = a->current.pos.x;
        m.lastZ = a->current.pos.z;
    }
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    mods::log::info("enemy_tactics v5 initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    tactics_tick();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_memory.clear();
    g_seenProcs.clear();
    reset_player_state();
    g_openWindow = 0;
    g_stillTicks = 0;
    g_fightTicks = 0;
    g_calmTicks = 0;
    g_anyChaser = false;
    return MOD_OK;
}
}
