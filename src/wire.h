/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * Wire format shared between fc_client_region_create()'s client half (in
 * client.c) and the server's accept loop (in server.c).
 * Internal to this repo's build only -- not installed, not part of the
 * public API.
 *
 * A region is created in two round trips over the same SOCK_SEQPACKET
 * connection, both blocking, single-message exchanges:
 *
 *   1. Resolve: the client sends only the descriptor -- it doesn't know
 *      the chunk layout yet, since that's decided entirely by the
 *      server's factory now (see faultcache-server.h). The server
 *      replies with the chunk layout, or rejects the descriptor.
 *   2. Attach: now knowing the total size, the client creates its local
 *      memfd/mmap/uffd mapping, then sends its uffd (via SCM_RIGHTS) and
 *      base address, re-stating the same descriptor bytes so the server
 *      can find the region resolve() just found or created. The server
 *      acks/nacks with a single reply byte.
 *
 * Neither round trip is on any fault-handling hot path -- both happen
 * once per region, at creation time.
 */
#ifndef FAULTCACHE_WIRE_H
#define FAULTCACHE_WIRE_H

#include <stdint.h>

/* Resolve request: header immediately followed by descriptor_size bytes. */
struct fc_resolve_req_hdr {
    uint32_t descriptor_size;
    uint32_t reserved;
};

/*
 * Resolve response: header immediately followed by trailing bytes whose
 * meaning depends on status. status == 0 means accepted: nchunks *
 * sizeof(uint64_t) bytes of chunk sizes follow. Nonzero means the
 * descriptor was rejected (by the factory, or a resource failure),
 * reported to the caller as errno = ECONNREFUSED: error_len bytes of a
 * (not necessarily NUL-terminated) UTF-8 error message follow, which may
 * be empty (error_len == 0) if the factory/library had nothing to say.
 */
struct fc_resolve_resp_hdr {
    int32_t status;
    uint32_t nchunks;
    uint32_t error_len;
};

/*
 * Attach request: header immediately followed by descriptor_size bytes
 * (the same descriptor just resolved), with the client's uffd fd
 * attached as SCM_RIGHTS ancillary data. Acked/nacked with a single
 * reply byte (0 = accepted, nonzero = rejected).
 */
struct fc_attach_req_hdr {
    uint64_t base; /* client's region base address -- opaque to the
                     * server, used only for fault-address arithmetic
                     * against the same registered mm, never dereferenced
                     * by the server itself */
    uint32_t descriptor_size;
    uint32_t reserved;
};

/* Generous cap on a single message (header + chunk sizes/descriptor), so
 * both sides can use a fixed-size receive buffer. */
#define FC_WIRE_MSG_MAX ((size_t)1 << 20)

#endif /* FAULTCACHE_WIRE_H */

