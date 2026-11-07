#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

#include "lizalfos_ai.hpp"

DEFINE_MOD();

IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(LogService, svc_log);

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    const ModResult result = nightmare_lizalfos::install_lizalfos_ai_hooks(svc_hook);
    if (result != MOD_OK) {
        mods::log::error("NIGHTMARE LIZALFOS: failed to install actor hooks (ModResult {})",
            static_cast<int>(result));
        return mods::set_error(error, result,
            "NIGHTMARE LIZALFOS could not resolve one or more E_DN Lizalfos hook targets. "
            "Check that the installed Dusklight build matches the mod's pinned SDK version.");
    }

    mods::log::info("NIGHTMARE LIZALFOS initialized; adaptive E_DN combat hooks are active");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    nightmare_lizalfos::uninstall_lizalfos_ai_hooks(svc_hook);
    mods::log::info("NIGHTMARE LIZALFOS shut down; actor memory cleared");
    return MOD_OK;
}

}  // extern "C"
