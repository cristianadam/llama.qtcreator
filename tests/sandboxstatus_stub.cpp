// Test stub: the sandbox availability warning needs the real ProjectManager
// and TaskHub, which the unit tests replace with stubs.
#include "../src/sandboxstatus.h"

namespace LlamaCpp::SandboxStatus {

QString unavailableReason()
{
    return {};
}

void setup() {}

void check() {}

} // namespace LlamaCpp::SandboxStatus
