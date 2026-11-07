#pragma once

#include <mods/svc/hook.h>

namespace nightmare_lizalfos {

// Installs hooks only on the E_DN (Lizalfos) actor's own translation unit functions.
ModResult install_lizalfos_ai_hooks(const HookService* hooks);
void uninstall_lizalfos_ai_hooks(const HookService* hooks);

}  // namespace nightmare_lizalfos
