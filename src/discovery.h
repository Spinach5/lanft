/* UDP Discovery Responder — makes this instance discoverable on LAN.
   Call discovery_start() at startup, discovery_stop() on exit. */
#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <stdint.h>

void discovery_start(uint16_t port);
void discovery_stop(void);

#endif
