#include "lizalfos_ai.hpp"

#include "mods/svc/hook.hpp"

#include "d/actor/d_a_e_dn.h"
#include "d/actor/d_a_player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace nightmare_lizalfos {
namespace {

// These values are the E_DN action IDs in the pinned d_a_e_dn.cpp implementation.
// Reusing stock actions keeps animation IDs, attack spheres, collision and death logic intact.
constexpr s16 kActionFightRun = 3;
constexpr s16 kActionAttack0 = 4;
constexpr s16 kActionGuard = 7;

constexpr std::size_t kTrackedActors = 48;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kAngleToRadians = (2.0f * kPi) / 65536.0f;
constexpr float kDetectionSoftCap = 4200.0f;
constexpr float kCounterRange = 850.0f;
constexpr float kGuardRange = 560.0f;
constexpr uint32_t kCounterWindowTicks = 60;
constexpr uint32_t kAttackWindowMaxTicks = 48;

struct ActorMemory {
    e_dn_class* actor = nullptr;
    uint32_t tick = 0;
    uint32_t last_seen_global_tick = 0;
    uint32_t attack_start_tick = 0;
    uint32_t counter_pending_until = 0;
    s16 last_health = 0;
    u8 current_cut = daPy_py_c::CUT_TYPE_NONE;
    u8 last_completed_cut = daPy_py_c::CUT_TYPE_NONE;
    u8 same_cut_streak = 0;
    u8 roll_pressure = 0;
    u8 shield_pressure = 0;
    bool initialized = false;
    bool cut_active = false;
    bool attack_connected = false;
    bool roll_active = false;
};

std::array<ActorMemory, kTrackedActors> g_actor_memory{};
uint32_t g_global_tick = 0;

bool is_sword_cut(u8 cut) noexcept {
    // The player header's sword/finisher/dash cut types occupy 0x01..0x29.
    // Wolf attack types begin at 0x2C and are deliberately left to stock handling.
    return cut > daPy_py_c::CUT_TYPE_NONE && cut <= daPy_py_c::CUT_TYPE_GUARD_ATTACK;
}

ActorMemory* find_memory(e_dn_class* actor, bool create) noexcept {
    if (actor == nullptr) {
        return nullptr;
    }

    ActorMemory* free_slot = nullptr;
    ActorMemory* oldest_slot = nullptr;
    uint32_t greatest_age = 0;

    for (auto& slot : g_actor_memory) {
        if (slot.actor == actor) {
            slot.last_seen_global_tick = g_global_tick;
            return &slot;
        }
        if (slot.actor == nullptr) {
            if (free_slot == nullptr) {
                free_slot = &slot;
            }
            continue;
        }

        const uint32_t age = g_global_tick - slot.last_seen_global_tick;
        if (age > greatest_age) {
            greatest_age = age;
            oldest_slot = &slot;
        }
    }

    if (!create) {
        return nullptr;
    }

    // Prefer a free entry. Reclaim old entries after an actor has stopped executing;
    // this avoids heap allocation and prevents stale actor pointers from accumulating.
    ActorMemory* slot = free_slot;
    if (slot == nullptr && oldest_slot != nullptr && greatest_age > 1800) {
        slot = oldest_slot;
    }
    if (slot == nullptr) {
        return nullptr;
    }

    *slot = ActorMemory{};
    slot->actor = actor;
    slot->last_health = actor->actor.health;
    slot->initialized = true;
    slot->last_seen_global_tick = g_global_tick;
    return slot;
}

void erase_memory(e_dn_class* actor) noexcept {
    for (auto& slot : g_actor_memory) {
        if (slot.actor == actor) {
            slot = ActorMemory{};
            return;
        }
    }
}

float distance_squared_xz(const e_dn_class* lizalfos, const daPy_py_c* player) noexcept {
    if (lizalfos == nullptr || player == nullptr) {
        return 1.0e30f;
    }
    const float dx = player->current.pos.x - lizalfos->actor.current.pos.x;
    const float dz = player->current.pos.z - lizalfos->actor.current.pos.z;
    return dx * dx + dz * dz;
}

bool player_is_in_front(const e_dn_class* lizalfos, const daPy_py_c* player) noexcept {
    if (lizalfos == nullptr || player == nullptr) {
        return false;
    }

    const float dx = player->current.pos.x - lizalfos->actor.current.pos.x;
    const float dz = player->current.pos.z - lizalfos->actor.current.pos.z;
    const float distance_sq = dx * dx + dz * dz;
    if (distance_sq < 1.0f) {
        return true;
    }

    const float yaw = static_cast<float>(lizalfos->actor.shape_angle.y) * kAngleToRadians;
    const float forward_x = std::sin(yaw);
    const float forward_z = std::cos(yaw);
    const float forward_dot = dx * forward_x + dz * forward_z;

    // A 0.35 threshold creates a generous frontal cone without granting rear blocking.
    return forward_dot > std::sqrt(distance_sq) * 0.35f;
}

void update_memory(e_dn_class* lizalfos) noexcept {
    if (lizalfos == nullptr || lizalfos->status != 0) {
        return;
    }

    ++g_global_tick;
    ActorMemory* memory = find_memory(lizalfos, true);
    if (memory == nullptr) {
        return;
    }

    ++memory->tick;
    memory->last_seen_global_tick = g_global_tick;

    // A health decrease confirms that an attack connected. It remains ordinary damage;
    // this record is only used to avoid countering a swing that actually hit.
    if (lizalfos->actor.health < memory->last_health) {
        memory->attack_connected = true;
        memory->counter_pending_until = 0;
    }
    memory->last_health = lizalfos->actor.health;

    daPy_py_c* player = daPy_getPlayerActorClass();
    if (player == nullptr) {
        memory->cut_active = false;
        memory->roll_active = false;
        return;
    }

    const u8 cut = player->getCutType();
    const bool cut_active = is_sword_cut(cut);
    if (cut_active && (!memory->cut_active || cut != memory->current_cut)) {
        if (memory->last_completed_cut == cut) {
            memory->same_cut_streak = static_cast<u8>(
                std::min<int>(8, static_cast<int>(memory->same_cut_streak) + 1));
        } else {
            memory->same_cut_streak = 1;
        }

        memory->current_cut = cut;
        memory->last_completed_cut = cut;
        memory->attack_start_tick = memory->tick;
        memory->attack_connected = false;
    } else if (!cut_active && memory->cut_active) {
        const uint32_t attack_age = memory->tick - memory->attack_start_tick;
        const float distance_sq = distance_squared_xz(lizalfos, player);
        if (!memory->attack_connected && attack_age >= 3 &&
            attack_age <= kAttackWindowMaxTicks &&
            distance_sq <= kCounterRange * kCounterRange) {
            // This is a missed/blocked swing window, not input prediction. The counter
            // can only happen later in stock E_DN combat positioning and remains escapable.
            memory->counter_pending_until = memory->tick + kCounterWindowTicks;
        }
    }
    memory->cut_active = cut_active;

    const bool rolling = player->checkFrontRoll() || player->checkSideStep();
    if (rolling && !memory->roll_active) {
        memory->roll_pressure = static_cast<u8>(
            std::min<int>(8, static_cast<int>(memory->roll_pressure) + 1));
    }
    memory->roll_active = rolling;

    if (player->checkPlayerGuard()) {
        memory->shield_pressure = static_cast<u8>(
            std::min<int>(8, static_cast<int>(memory->shield_pressure) + 1));
    } else if ((memory->tick & 3u) == 0u && memory->shield_pressure > 0) {
        --memory->shield_pressure;
    }

    // Adaptive memory decays during long encounters instead of becoming permanent.
    if (memory->tick != 0 && (memory->tick % 90u) == 0u) {
        if (memory->same_cut_streak > 0) {
            --memory->same_cut_streak;
        }
        if (memory->roll_pressure > 0) {
            --memory->roll_pressure;
        }
        if (memory->shield_pressure > 0) {
            --memory->shield_pressure;
        }
    }
}

// File-local statics in d_a_e_dn.cpp are intentionally addressed using the SDK's
// translation-unit alias format. Signatures below match the pinned v2.0.2 source.
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_e_dn.cpp#daE_DN_Execute",
    int(e_dn_class*), LizalfosExecute);
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_e_dn.cpp#daE_DN_Delete",
    int(e_dn_class*), LizalfosDelete);
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_e_dn.cpp#pl_check",
    int(e_dn_class*, f32, s16), LizalfosPlayerCheck);
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_e_dn.cpp#e_dn_fight_run",
    void(e_dn_class*), LizalfosFightRun);
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_e_dn.cpp#small_damage",
    void(e_dn_class*), LizalfosSmallDamage);
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_e_dn.cpp#big_damage",
    void(e_dn_class*), LizalfosBigDamage);

HookAction on_execute_pre(ModContext*, void* args, void*, void*) {
    update_memory(mods::arg<e_dn_class*>(args, 0));
    return HOOK_CONTINUE;
}

HookAction on_delete_pre(ModContext*, void* args, void*, void*) {
    erase_memory(mods::arg<e_dn_class*>(args, 0));
    return HOOK_CONTINUE;
}

HookAction on_player_check_pre(ModContext*, void* args, void*, void*) {
    e_dn_class* lizalfos = mods::arg<e_dn_class*>(args, 0);
    if (lizalfos == nullptr || lizalfos->status != 0) {
        return HOOK_CONTINUE;
    }

    f32& search_area = mods::arg_ref<f32>(args, 1);
    s16& search_angle = mods::arg_ref<s16>(args, 2);
    const ActorMemory* memory = find_memory(lizalfos, false);

    float multiplier = 1.28f;
    if (memory != nullptr) {
        multiplier += 0.025f * static_cast<float>(std::min<u8>(memory->same_cut_streak, 4));
        if (memory->roll_pressure >= 2) {
            multiplier += 0.04f;
        }
    }

    // Increase ordinary sight checks while retaining the game's wall/line-of-sight test.
    // Existing large scripted ranges are left unchanged; no across-room sensing is added.
    if (search_area > 0.0f && search_area < kDetectionSoftCap) {
        search_area = std::min(search_area * multiplier, kDetectionSoftCap);
    }
    if (search_angle > 0 && search_angle < 0x7000) {
        search_angle = static_cast<s16>(std::min<int>(0x7000, search_angle + 0x0800));
    }

    return HOOK_CONTINUE;
}

HookAction on_fight_run_pre(ModContext*, void* args, void*, void*) {
    e_dn_class* lizalfos = mods::arg<e_dn_class*>(args, 0);
    if (lizalfos == nullptr || lizalfos->status != 0 || lizalfos->actor.health <= 0) {
        return HOOK_CONTINUE;
    }

    ActorMemory* memory = find_memory(lizalfos, false);
    daPy_py_c* player = daPy_getPlayerActorClass();
    if (memory == nullptr || player == nullptr) {
        return HOOK_CONTINUE;
    }

    const float distance_sq = distance_squared_xz(lizalfos, player);
    const bool in_front = player_is_in_front(lizalfos, player);
    const u8 cut = player->getCutType();
    const bool cut_active = is_sword_cut(cut);

    if (memory->counter_pending_until != 0) {
        if (memory->tick <= memory->counter_pending_until && !cut_active &&
            distance_sq <= kCounterRange * kCounterRange) {
            // Reuse the game's normal attack-0 state, animation and attack collider.
            lizalfos->action = kActionAttack0;
            lizalfos->mode = 0;
            lizalfos->timer[2] = 35;
            memory->counter_pending_until = 0;
            return HOOK_SKIP_ORIGINAL;
        }
        if (memory->tick > memory->counter_pending_until) {
            memory->counter_pending_until = 0;
        }
    }

    const bool dangerous_jump = cut == daPy_py_c::CUT_TYPE_JUMP ||
        cut == daPy_py_c::CUT_TYPE_HEAD_JUMP;
    const bool learned_repetition = memory->same_cut_streak >= 2;
    const bool close_commitment = distance_sq <= 250.0f * 250.0f;
    if (cut_active && in_front && distance_sq <= kGuardRange * kGuardRange &&
        (learned_repetition || dangerous_jump || close_commitment)) {
        // ACTION_GUARD runs the original ANM_GUARD state and enables the actual tate_sph
        // shield collider. No global projectile deletion or omnidirectional block is used.
        lizalfos->action = kActionGuard;
        lizalfos->mode = 0;
        lizalfos->timer[0] = 0;
        memory->counter_pending_until = 0;
        return HOOK_SKIP_ORIGINAL;
    }

    if (player->checkPlayerGuard() && distance_sq <= 1000.0f * 1000.0f) {
        // Nudge the stock pursuit heading off the shield's centerline to encourage a
        // real movement-based flank. It does not alter Link's guard or collision.
        const s16 side_step = ((memory->tick / 60u) & 1u) != 0u ? -0x0C00 : 0x0C00;
        lizalfos->search_angle_y = static_cast<s16>(lizalfos->search_angle_y + side_step);
        if (lizalfos->timer[2] > 14) {
            lizalfos->timer[2] = 14;
        }
    }

    if (!memory->roll_active && memory->roll_pressure >= 2 &&
        distance_sq <= 800.0f * 800.0f && lizalfos->timer[2] > 8) {
        // Shorten the normal attack decision delay after repeated roll/side-step habits.
        // Existing player invulnerability frames and stock hitboxes remain authoritative.
        lizalfos->timer[2] = 8;
    }

    if (lizalfos->actor.health <= 35 && lizalfos->timer[2] > 9) {
        lizalfos->timer[2] = 9;
    }

    return HOOK_CONTINUE;
}

void on_fight_run_post(ModContext*, void* args, void*, void*) {
    e_dn_class* lizalfos = mods::arg<e_dn_class*>(args, 0);
    if (lizalfos == nullptr || lizalfos->status != 0 ||
        lizalfos->actor.health <= 0 || lizalfos->action != kActionFightRun) {
        return;
    }

    // Low-health enrage is a restrained movement/tempo boost, not extra damage or health.
    if (lizalfos->actor.health <= 35 && lizalfos->actor.speedF > 0.1f) {
        lizalfos->actor.speedF = std::min(lizalfos->actor.speedF * 1.12f, 30.0f);
    }
}

HookAction on_small_damage_pre(ModContext*, void* args, void*, void*) {
    e_dn_class* lizalfos = mods::arg<e_dn_class*>(args, 0);
    // cc_at_check has already applied health damage before this callback. Suppress only
    // the nonlethal flinch routine; allow the stock routine when health reaches zero.
    if (lizalfos != nullptr && lizalfos->status == 0 && lizalfos->actor.health > 0) {
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

HookAction on_big_damage_pre(ModContext*, void* args, void*, void*) {
    e_dn_class* lizalfos = mods::arg<e_dn_class*>(args, 0);
    // Preserve lethal/death reactions and every part of cc_at_check damage processing.
    if (lizalfos != nullptr && lizalfos->status == 0 && lizalfos->actor.health > 0) {
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

}  // namespace

ModResult install_lizalfos_ai_hooks(const HookService* hooks) {
    if (hooks == nullptr) {
        return MOD_UNAVAILABLE;
    }

    ModResult result = mods::hook::add_pre<LizalfosExecute>(hooks, on_execute_pre);
    if (result != MOD_OK) {
        return result;
    }

    result = mods::hook::add_pre<LizalfosDelete>(hooks, on_delete_pre);
    if (result != MOD_OK) {
        uninstall_lizalfos_ai_hooks(hooks);
        return result;
    }

    result = mods::hook::add_pre<LizalfosPlayerCheck>(hooks, on_player_check_pre);
    if (result != MOD_OK) {
        uninstall_lizalfos_ai_hooks(hooks);
        return result;
    }

    result = mods::hook::add_pre<LizalfosFightRun>(hooks, on_fight_run_pre);
    if (result != MOD_OK) {
        uninstall_lizalfos_ai_hooks(hooks);
        return result;
    }

    result = mods::hook::add_post<LizalfosFightRun>(hooks, on_fight_run_post);
    if (result != MOD_OK) {
        uninstall_lizalfos_ai_hooks(hooks);
        return result;
    }

    result = mods::hook::add_pre<LizalfosSmallDamage>(hooks, on_small_damage_pre);
    if (result != MOD_OK) {
        uninstall_lizalfos_ai_hooks(hooks);
        return result;
    }

    result = mods::hook::add_pre<LizalfosBigDamage>(hooks, on_big_damage_pre);
    if (result != MOD_OK) {
        uninstall_lizalfos_ai_hooks(hooks);
        return result;
    }

    return MOD_OK;
}

void uninstall_lizalfos_ai_hooks(const HookService* hooks) {
    if (hooks == nullptr) {
        return;
    }

    // Uninstall is safe for unresolved/uninstalled entries and keeps reloads clean.
    mods::hook::uninstall<LizalfosBigDamage>(hooks);
    mods::hook::uninstall<LizalfosSmallDamage>(hooks);
    mods::hook::uninstall<LizalfosFightRun>(hooks);
    mods::hook::uninstall<LizalfosPlayerCheck>(hooks);
    mods::hook::uninstall<LizalfosDelete>(hooks);
    mods::hook::uninstall<LizalfosExecute>(hooks);

    for (auto& slot : g_actor_memory) {
        slot = ActorMemory{};
    }
    g_global_tick = 0;
}

}  // namespace nightmare_lizalfos
