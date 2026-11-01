// pvai install-service: the daemon as a systemd user service, so it runs
// after every boot (with lingering) instead of only on demand.
#pragma once

namespace pvai {

int installService();
int uninstallService();
// When the user service is installed, starts the daemon through it; false
// when there is none or systemd would not start it (the caller then starts
// the daemon itself).
bool startServiceDaemon();

} // namespace pvai
