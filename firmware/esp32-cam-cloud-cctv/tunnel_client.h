#pragma once

void tunnelBegin();
void tunnelStopNow();
void handleTunnel();
// Returns true once the tunnel has finished its connect handshake and has a
// usable public URL.
bool isTunnelReady();
// Returns true when the bore tunnel has at least one proxy slot actively
// handling a remote connection. Use this to defer heap-intensive background
// tasks (e.g. idle cloud uploads) while the tunnel is serving a client.
bool isTunnelSlotBusy();
