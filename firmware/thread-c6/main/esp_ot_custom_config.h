/*
 * Custom OpenThread configuration for the Keystone Thread host.
 *
 * The IDF build of the OpenThread core leaves the SLAAC module out:
 * OPENTHREAD_CONFIG_IP6_SLAAC_ENABLE defaults to 0 in
 * openthread/src/core/config/ip6.h, so neither slaac_address.cpp nor the
 * notifier hook in core/common/notifier.cpp is compiled in.
 *
 * A Border Router needs it. The OMR prefix the BR puts into the network data
 * is flagged "slaac" (core/border_router/routing_manager.cpp: config.mSlaac =
 * true), and the SLAAC module is what gives the "ot" netif an address of that
 * prefix. Without such an address lwIP has no on-link route into the mesh:
 * the gateway hook of the BR only serves the routes learnt from the router
 * advertisements on the backbone, and the "ot" netif has no default router —
 * so nd6_get_next_hop_entry() returns ERR_RTE and every backbone packet to a
 * mesh address dies in lwIP, before it ever reaches the OpenThread stack (the
 * BR counters stay at in_unicast=0).
 *
 * Upstream builds the Border Router the same way:
 * openthread/examples/config/ot-core-config-check-size-br.h has
 * OPENTHREAD_CONFIG_IP6_SLAAC_ENABLE 1.
 */
#pragma once

#define OPENTHREAD_CONFIG_IP6_SLAAC_ENABLE 1
