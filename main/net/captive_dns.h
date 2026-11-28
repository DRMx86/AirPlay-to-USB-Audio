#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Tiny captive-portal DNS responder.
 *
 * Answers every A query with one IPv4 address (the soft-AP's own address) so
 * phones and laptops that join the setup network are sent to the Wi-Fi page.
 * Other record types (AAAA, ...) get an empty answer, which makes clients fall
 * back to IPv4.
 */

/**
 * Start answering on UDP port 53.
 *
 * @param ip_be  address to hand out, in network byte order (as in lwip)
 */
esp_err_t captive_dns_start(uint32_t ip_be);

/**
 * Stop the responder.  The task ends within about a second.
 */
void captive_dns_stop(void);

/**
 * Turn a DNS query packet into its reply, in place.
 *
 * @param pkt     query on input, reply on output
 * @param len     query length
 * @param cap     size of the pkt buffer
 * @param ip_be   address for A answers, network byte order
 * @return reply length, or 0 if the packet should be ignored
 */
size_t captive_dns_build_reply(uint8_t *pkt, size_t len, size_t cap,
                               uint32_t ip_be);
