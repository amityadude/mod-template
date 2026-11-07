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
