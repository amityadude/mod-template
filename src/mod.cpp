// Enemy Tactics v3 ("ruthless"): makes mobile enemies smarter and harder without touching their health.
//
// Runs once per game tick from mod_update (no function hooks). 30 ticks = 1 second.
//
// Treatment per enemy (see kind_of):
//   Adapter  (Bokoblin, Lizalfos, Dynalfos, Stalfos): full tactics. The mod switches the enemy's own
//            actions (attack / draw back / wake up), so you get the game's real animations.
//   Generic  (27 other mobile enemies): movement rules only.
//   Ignored  (all bosses, plants, statues, projectiles, water and wall enemies).
//
// Every tunable number is in namespace cfg. Distances are game units (Link is ~100 tall).
// All movement rules add up into one push per enemy per tick, capped at cfg::kMaxStep.

#include "mods/service.hpp"
#include "mods/svc/log.hpp"

// Game includes
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_e_dn.h"
#include "d/actor/d_a_e_mf.h"
#include "d/actor/d_a_e_oc.h"
#include "d/actor/d_a_e_sf.h"
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

namespace cfg {
// --- global
constexpr float kEngageRange = 1500.0f;   // enemies farther than this from Link are ignored
constexpr float kMovingSpeed = 1.0f;      // generic enemies count as "chasing" at |speedF| >= this
constexpr float kIntensity = 1.0f;        // multiplies every movement step below (1.0 = as written)
constexpr float kMaxStep = 14.0f;         // total extra movement per enemy per tick is capped here
constexpr int kDesperationLife = 12;      // Link at or below this many life units (12 = 3 hearts)...
constexpr float kDesperationBoost = 1.5f; // ...makes steps, chances and lunges this much stronger

// --- escalation: the longer a fight lasts, the harder they press
constexpr int kEscalationStartTicks = 300;   // pressure starts rising after 10 s of fighting
constexpr int kEscalationFullTicks = 1800;   // and is at full strength after 60 s
constexpr float kEscalationMax = 0.5f;       // +50% at full strength
constexpr int kCalmResetTicks = 150;         // 5 s with nobody chasing resets it

// --- speed: chasing enemies move this much faster (0.25 = +25%)
constexpr float kSpeedBonus = 0.25f;

// --- lunge: a short burst toward Link when he is at mid range
constexpr float kLungeMinDist = 350.0f;
constexpr float kLungeMaxDist = 800.0f;
constexpr float kLungeStep = 12.0f;       // units per tick during a lunge
constexpr int kLungeTicks = 8;
constexpr float kLungeChance = 0.5f;      // checked once the cooldown is over
constexpr int kLungeCooldown = 150;

// --- sense: idle fighters this close notice Link at once, no line of sight needed
constexpr float kSenseRadius = 500.0f;
constexpr float kSenseMaxHeightDiff = 200.0f;
constexpr int kWakeCooldown = 30;

// --- lead pursuit: while Link moves, enemies run to where he is going, not to where he was
constexpr float kLeadTicks = 12.0f;
constexpr float kLeadStep = 3.0f;
constexpr float kLeadMinDist = 250.0f;

// --- anti-flee: Link retreating (roll, backflip, run) makes them speed up after him
constexpr float kFleeMinSpeed = 3.0f;     // Link must move at least this much per tick
constexpr float kFleeMaxSpeed = 40.0f;    // faster than this (horse) is ignored
constexpr float kFleeStep = 7.0f;
constexpr float kFleeMinDist = 200.0f;
constexpr float kFleeRange = 700.0f;

// --- surround: 2+ chasers spread evenly around Link, one of them takes his back
constexpr float kFlankRadius = 1000.0f;
constexpr float kFlankStep = 4.0f;
constexpr float kRearPull = 0.75f;        // extra push for the enemy nearest Link's back
constexpr float kSpaceRadius = 160.0f;    // enemies closer than this to each other push apart
constexpr float kSpaceStep = 3.0f;

// --- you hold your shield up -> they circle to your side
constexpr float kCircleStep = 5.0f;
constexpr float kCircleMinDist = 150.0f;
constexpr float kCircleMaxDist = 600.0f;

// --- you charge a spin attack -> they interrupt (attack) or back off, 50/50
constexpr float kChargeReactRange = 450.0f;
constexpr int kChargeCooldown = 150;

// --- opening: your swing, roll or shield drop just ended -> they counter at once
constexpr int kOpenWindow = 20;
constexpr float kCounterRange = 330.0f;
constexpr int kCounterCooldown = 120;

// --- you start a sword swing or a jump strike -> they dodge (draw back)
constexpr float kDodgeRange = 280.0f;
constexpr float kDodgeChance = 0.5f;
constexpr int kDodgeCooldown = 120;

// --- hit and run: after their own attack ends they hop back out of your counter range
constexpr float kRetreatChance = 0.4f;
constexpr float kRetreatMaxDist = 300.0f;
constexpr int kRetreatCooldown = 90;

// --- you aim a ranged item: they strafe out of the line of fire and close in
constexpr float kAimCosine = 0.94f;       // cos(20 degrees): enemy within 20 degrees of where you aim
constexpr float kAimRange = 1200.0f;
constexpr float kAimDodgeStep = 6.0f;

// --- you stand still (ranged item or camping) -> they close the distance
constexpr float kAdvanceStep = 6.0f;
constexpr float kAdvanceMinDist = 350.0f;
constexpr float kPlayerStillSpeed = 1.0f;
constexpr int kCampTicks = 60;            // standing still this long counts as camping (half-speed advance)

// --- group alert: one enemy fighting wakes idle enemies of the four adapter types nearby
constexpr float kAlertRadius = 1000.0f;
constexpr float kAlertMaxHeightDiff = 300.0f;

// --- faster attacks and shorter stagger (adapter enemies)
constexpr float kAttackAnimSpeed = 1.75f;  // 1.0 = vanilla speed
constexpr float kStaggerAnimSpeed = 2.0f;
constexpr short kAttackDecisionTimer = 8;  // Lizalfos/Stalfos attack timer is capped to this (vanilla 10-30)
}  // namespace cfg

enum class Kind { None, Generic, Bokoblin, Lizalfos, Dynalfos, Stalfos };

static Kind kind_of(fopAc_ac_c* a) {
    if (a->group != fopAc_ENEMY_e) {
        return Kind::None;
    }
    switch (fopAcM_GetProfName(a)) {
    case fpcNm_E_OC_e: return Kind::Bokoblin;
    case fpcNm_E_DN_e: return Kind::Lizalfos;
    case fpcNm_E_MF_e: return Kind::Dynalfos;
    case fpcNm_E_SF_e: return Kind::Stalfos;
    case fpcNm_E_AI_e:   // Armos
    case fpcNm_E_BA_e:   // Keese
    case fpcNm_E_BS_e:   // Stalkin
    case fpcNm_E_BU_e:   // Bubble
    case fpcNm_E_BUG_e:  // Poison Mite
    case fpcNm_E_CR_e:   // Bombskit
    case fpcNm_E_DD_e:   // Dodongo
    case fpcNm_E_DK_e:   // Bari
    case fpcNm_E_GE_e:   // Guay
    case fpcNm_E_GI_e:   // Gibdo
    case fpcNm_E_GS_e:   // Ghost Soldier
    case fpcNm_E_HM_e:   // Torch Slug
    case fpcNm_E_KK_e:   // Chilfos
    case fpcNm_E_KR_e:   // Kargorok
    case fpcNm_E_MM_e:   // Helmasaur
    case fpcNm_E_NZ_e:   // Ghoul Rat
    case fpcNm_E_PH_e:   // Peahat
    case fpcNm_E_RB_e:   // Leever
    case fpcNm_E_S1_e:   // Shadow Beast
    case fpcNm_E_SH_e:   // Stalhound
    case fpcNm_E_SM2_e:  // Chuchu
    case fpcNm_E_TH_e:   // Darkhammer
    case fpcNm_E_TT_e:   // Tektite
    case fpcNm_E_WW_e:   // White Wolfos
    case fpcNm_E_YC_e:   // Twilight Kargorok
    case fpcNm_E_YK_e:   // Shadow Keese
    case fpcNm_E_ZS_e:   // Staltroop
        return Kind::Generic;
    default: return Kind::None;
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
    float px = 0.0f;  // movement requested this tick (applied once, at the end)
    float pz = 0.0f;
};

static std::unordered_map<const void*, EnemyMemory> g_memory;
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

static void add_push(EnemyMemory& m, float x, float z) {
    m.px += x;
    m.pz += z;
}

// True while the enemy is in its "run at Link" state, the safe moment to inject a new action.
static bool is_chasing(Kind k, fopAc_ac_c* a) {
    switch (k) {
    case Kind::Bokoblin: return reinterpret_cast<daE_OC_c*>(a)->mActionMode == oc::FIND;
    case Kind::Lizalfos: return reinterpret_cast<e_dn_class*>(a)->action == dn::FIGHT_RUN;
    case Kind::Dynalfos: return reinterpret_cast<e_mf_class*>(a)->mAction == mf::FIGHT_RUN;
    case Kind::Stalfos: return reinterpret_cast<e_sf_class*>(a)->mAction == sf::FIGHT_RUN;
    case Kind::Generic: return std::fabs(a->speedF) >= cfg::kMovingSpeed;
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

// Idle (not yet fighting) and safe to wake. Scripted/demo/sleeping states are not included.
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

static bool is_adapter(Kind k) {
    return k != Kind::None && k != Kind::Generic;
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
        e->mInvulnerabilityTimer = 6;
        break;
    }
    default: break;
    }
}

// Faster attacks, shorter stagger, faster attack decisions, no leash.
static void tune_reactions(Kind k, fopAc_ac_c* a) {
    switch (k) {
    case Kind::Bokoblin: {
        auto* e = reinterpret_cast<daE_OC_c*>(a);
        e->mMoveRange = 0.0f;  // never gives up and walks back to its post
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
    const float power = cfg::kIntensity * press;      // multiplies movement steps

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
        auto* en = static_cast<fopEn_enemy_c*>(a);
        if (en->checkDeadFlg() || en->checkDownFlg() || dist_xz(a, plr) > cfg::kEngageRange) {
            continue;
        }
        auto it = g_memory.find(a);
        alive[a] = (it != g_memory.end()) ? it->second : EnemyMemory{};
        engaged.push_back(a);
    }
    g_memory.swap(alive);

    // Sense + group alert: idle enemies of the four adapter types wake when Link is close, or when a
    // neighbour is already fighting.
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

    std::vector<Chaser> chasers;
    for (fopAc_ac_c* a : engaged) {
        const Kind k = kind_of(a);
        EnemyMemory& m = g_memory[a];
        m.px = 0.0f;
        m.pz = 0.0f;
        if (m.dodgeCd > 0) m.dodgeCd--;
        if (m.chargeCd > 0) m.chargeCd--;
        if (m.counterCd > 0) m.counterCd--;
        if (m.lungeCd > 0) m.lungeCd--;
        if (m.retreatCd > 0) m.retreatCd--;

        tune_reactions(k, a);

        const bool attacking = is_adapter(k) && is_attacking(k, a);
        const bool chasing = is_chasing(k, a);
        const float dist = dist_xz(a, plr);

        // Hit and run: their attack just ended and they are back in chase mode, hop out of range.
        if (m.wasAttacking && !attacking && chasing && can_back_off(k) && m.retreatCd == 0 &&
            dist < cfg::kRetreatMaxDist && rand01() < std::min(1.0f, cfg::kRetreatChance * press)) {
            m.wasAttacking = false;
            m.retreatCd = cfg::kRetreatCooldown;
            start_back_off(k, a);
            continue;
        }
        m.wasAttacking = attacking;

        if (!chasing) {
            m.lungeTicks = 0;
            continue;
        }
        g_anyChaser = true;

        // Adapter rules: use the enemy's own attack / draw-back actions.
        if (is_adapter(k)) {
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

        if (dist < 1.0f) {
            continue;
        }
        const float ux = (a->current.pos.x - plr->current.pos.x) / dist;  // Link -> enemy
        const float uz = (a->current.pos.z - plr->current.pos.z) / dist;
        const float side = ((reinterpret_cast<uintptr_t>(a) >> 4) & 1) ? 1.0f : -1.0f;  // fixed per enemy

        // Speed bonus along the enemy's own heading.
        if (a->speedF > cfg::kMovingSpeed) {
            const float heading = a->current.angle.y * kAngleToRad;
            const float extra = a->speedF * cfg::kSpeedBonus * power;
            add_push(m, std::sin(heading) * extra, std::cos(heading) * extra);
        }

        // Lunge: burst toward Link from mid range.
        if (m.lungeTicks > 0) {
            m.lungeTicks--;
            add_push(m, -ux * cfg::kLungeStep * power, -uz * cfg::kLungeStep * power);
        } else if (m.lungeCd == 0 && dist > cfg::kLungeMinDist && dist < cfg::kLungeMaxDist) {
            m.lungeCd = cfg::kLungeCooldown;
            if (rand01() < std::min(1.0f, cfg::kLungeChance * press)) {
                m.lungeTicks = cfg::kLungeTicks;
            }
        }

        // Lead pursuit: head for where Link is going.
        if (lspeed >= cfg::kFleeMinSpeed && dist > cfg::kLeadMinDist) {
            const float tx = plr->current.pos.x + vx * cfg::kLeadTicks - a->current.pos.x;
            const float tz = plr->current.pos.z + vz * cfg::kLeadTicks - a->current.pos.z;
            const float td = std::sqrt(tx * tx + tz * tz);
            if (td > 1.0f) {
                add_push(m, tx / td * cfg::kLeadStep * power, tz / td * cfg::kLeadStep * power);
            }
        }

        // Anti-flee: Link is moving away from this enemy, so it speeds up after him.
        if (lspeed >= cfg::kFleeMinSpeed && lspeed <= cfg::kFleeMaxSpeed && dist > cfg::kFleeMinDist &&
            dist < cfg::kFleeRange) {
            const float away = -(vx * ux + vz * uz);
            if (away > 0.5f * lspeed) {
                add_push(m, -ux * cfg::kFleeStep * power, -uz * cfg::kFleeStep * power);
            }
        }

        // You hold your shield up: circle to your side.
        if (guard && dist > cfg::kCircleMinDist && dist < cfg::kCircleMaxDist) {
            add_push(m, -uz * side * cfg::kCircleStep * power, ux * side * cfg::kCircleStep * power);
        }

        // You aim a ranged item: leave the line of fire sideways while closing in.
        if (ranged && dist < cfg::kAimRange && (ux * fx + uz * fz) > cfg::kAimCosine) {
            add_push(m, -uz * side * cfg::kAimDodgeStep * power, ux * side * cfg::kAimDodgeStep * power);
        }
        if ((ranged && still) && dist > cfg::kAdvanceMinDist) {
            add_push(m, -ux * cfg::kAdvanceStep * power, -uz * cfg::kAdvanceStep * power);
        } else if (camping && dist > cfg::kAdvanceMinDist) {
            add_push(m, -ux * cfg::kAdvanceStep * 0.5f * power, -uz * cfg::kAdvanceStep * 0.5f * power);
        }

        if (dist < cfg::kFlankRadius) {
            const float dx = a->current.pos.x - plr->current.pos.x;
            const float dz = a->current.pos.z - plr->current.pos.z;
            chasers.push_back({a, &m, std::atan2(dz, dx)});
        }
    }

    // Surround: spread chasers evenly around Link, and pull the one nearest his back all the way behind.
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
            const float dx = cur.actor->current.pos.x - plr->current.pos.x;
            const float dz = cur.actor->current.pos.z - plr->current.pos.z;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d > cfg::kCircleMinDist) {
                const float ux = dx / d;
                const float uz = dz / d;
                add_push(*cur.mem, -uz * push * cfg::kFlankStep * power, ux * push * cfg::kFlankStep * power);
            }
        }
    }

    // Spacing: push crowded pairs apart.
    for (size_t i = 0; i < chasers.size(); i++) {
        for (size_t j = i + 1; j < chasers.size(); j++) {
            fopAc_ac_c* a = chasers[i].actor;
            fopAc_ac_c* b = chasers[j].actor;
            const float dx = a->current.pos.x - b->current.pos.x;
            const float dz = a->current.pos.z - b->current.pos.z;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d > 1.0f && d < cfg::kSpaceRadius) {
                const float nx = dx / d;
                const float nz = dz / d;
                add_push(*chasers[i].mem, nx * cfg::kSpaceStep, nz * cfg::kSpaceStep);
                add_push(*chasers[j].mem, -nx * cfg::kSpaceStep, -nz * cfg::kSpaceStep);
            }
        }
    }

    // Apply each enemy's combined push once, capped.
    for (fopAc_ac_c* a : engaged) {
        EnemyMemory& m = g_memory[a];
        const float mag = std::sqrt(m.px * m.px + m.pz * m.pz);
        if (mag < 0.001f) {
            continue;
        }
        const float scale = mag > cfg::kMaxStep ? cfg::kMaxStep / mag : 1.0f;
        a->current.pos.x += m.px * scale;
        a->current.pos.z += m.pz * scale;
    }
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    mods::log::info("enemy_tactics v3 initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    tactics_tick();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_memory.clear();
    reset_player_state();
    g_openWindow = 0;
    g_stillTicks = 0;
    g_fightTicks = 0;
    g_calmTicks = 0;
    g_anyChaser = false;
    return MOD_OK;
}
}
