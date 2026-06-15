#ifndef SCANNER_H
#define SCANNER_H

#include <stdint.h>

/* Start a LAN scan for devices on the given port.
   Uses UDP broadcast + TCP connect scan.
   Non-blocking: spawns background thread. Results via SDL_UserEvent. */
void scanner_start(uint16_t port);

#endif /* SCANNER_H */
