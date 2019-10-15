// Copyright (c) 2019 Tigera, Inc. All rights reserved.

#include <asm/types.h>
#include <linux/bpf.h>
#include <linux/pkt_cls.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/icmp.h>
#include <linux/in.h>
#include <linux/udp.h>
#include <linux/if_ether.h>
#include <iproute2/bpf_elf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <linux/bpf.h>
#include "../include/bpf.h"
#include "../include/log.h"
#include "../include/policy.h"
#include "../include/conntrack.h"
#include "../include/nat.h"
#include "bpf_maps.h"

enum calico_policy_result {
	CALICO_POL_NO_MATCH,
	CALICO_POL_ALLOW,
	CALICO_POL_DENY
};

static CALICO_BPF_INLINE enum calico_policy_result execute_policy_norm(struct __sk_buff *skb,__u8 ip_proto, __u32 saddr, __u32 daddr, __u16 sport, __u16 dport, enum calico_tc_flags flags) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-label"

	// __NORMAL_POLICY__

	return CALICO_POL_NO_MATCH;
	deny:
	return CALICO_POL_DENY;
	allow:
	return CALICO_POL_ALLOW;
#pragma clang diagnostic pop
}

static CALICO_BPF_INLINE enum calico_policy_result execute_policy_aof(struct __sk_buff *skb,__u8 ip_proto, __u32 saddr, __u32 daddr, __u16 sport, __u16 dport, enum calico_tc_flags flags) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-label"

	// __AOF_POLICY__

	return CALICO_POL_NO_MATCH;
	deny:
	return CALICO_POL_DENY;
	allow:
	return CALICO_POL_ALLOW;
#pragma clang diagnostic pop
}

static CALICO_BPF_INLINE enum calico_policy_result maybe_execute_policy_pre_dnat(struct __sk_buff *skb, __u8 ip_proto, __u32 saddr, __u32 daddr, __u16 sport, __u16 dport, enum calico_tc_flags flags) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-label"

	// __PRE_DNAT_POLICY__

	return CALICO_POL_NO_MATCH;
	deny:
	return CALICO_POL_DENY;
	allow:
	return CALICO_POL_ALLOW;
#pragma clang diagnostic pop
}

static CALICO_BPF_INLINE enum calico_policy_result maybe_execute_policy_do_not_track(struct __sk_buff *skb, __u8 ip_proto, __u32 saddr, __u32 daddr, __u16 sport, __u16 dport, enum calico_tc_flags flags) {
	if (!(flags & CALICO_TC_HOST_EP) || !(flags & CALICO_TC_INGRESS)) {
		return CALICO_POL_NO_MATCH;
	}
	if ((skb->mark & CALICO_SKB_MARK_FROM_WORKLOAD_MASK) == CALICO_SKB_MARK_FROM_WORKLOAD) {
		return CALICO_POL_NO_MATCH;
	}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-label"

	// __DO_NOT_TRACK_POLICY__

	return CALICO_POL_NO_MATCH;
	deny:
	return CALICO_POL_DENY;
	allow:
	return CALICO_POL_ALLOW;
#pragma clang diagnostic pop
}

static CALICO_BPF_INLINE int calico_tc(struct __sk_buff *skb, enum calico_tc_flags flags) {
	enum calico_reason reason = CALICO_REASON_UNKNOWN;
	uint64_t prog_start_time;
	if (CALICO_LOG_LEVEL >= CALICO_LOG_LEVEL_INFO) {
		prog_start_time = bpf_ktime_get_ns();
	}
	uint64_t timer_start_time = 0 , timer_end_time = 0;
	int rc = TC_ACT_UNSPEC;

	// Parse the packet.

	CALICO_DEBUG_AT("Packet, ingress iface %d\n", skb->ingress_ifindex);

    // TODO Do we need to handle any odd-ball frames here (e.g. with a 0 VLAN header)?
	if (skb->protocol != be16_to_host(ETH_P_IP)) {
		CALICO_DEBUG_AT("Skipping ethertype %x\n", skb->protocol);
		reason = CALICO_REASON_NOT_IP;
		goto allow_skip_fib;
	}
	CALICO_DEBUG_AT("Packet is IP\n");

    if ((void *)(long)skb->data + sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr) > (void *)(long)skb->data_end) {
		CALICO_DEBUG_AT("Too short\n");
		reason = CALICO_REASON_SHORT;
		goto deny;
	}

    struct ethhdr *eth_hdr = (void *)(long)skb->data;
    struct iphdr *ip_header = (void *)(eth_hdr+1);
	// TODO Deal with IP header with options.

    // Setting all of these up-front to keep the verifier happy.
    struct tcphdr *tcp_header = (void*)(ip_header+1);
	struct udphdr *udp_header = (void*)(ip_header+1);
	struct icmphdr *icmp_header = (void*)(ip_header+1);

    CALICO_DEBUG_AT("IP; s=%x d=%x\n", be32_to_host(ip_header->saddr), be32_to_host(ip_header->daddr));

    __u8 ip_proto = ip_header->protocol;

    struct bpf_fib_lookup fib_params = {
		.family = 2, /* AF_INET */
		.tot_len = be16_to_host(ip_header->tot_len),
		.ifindex = skb->ingress_ifindex,
    };

	__u16 sport;
	__u16 dport;

	switch (ip_proto) {
	case IPPROTO_TCP:
		// Re-check buffer space for TCP (has larger headers than UDP).
		CALICO_DEBUG_AT("Packet is TCP\n");
		if ((void*)(tcp_header+1) > (void *)(long)skb->data_end) {
			CALICO_DEBUG_AT("Too short for TCP: DROP\n");
			goto deny;
		}
		sport = be16_to_host(tcp_header->source);
		dport = be16_to_host(tcp_header->dest);
		CALICO_DEBUG_AT("TCP; ports: s=%d d=%d\n", sport, dport);
		break;
	case IPPROTO_UDP:
		udp_header = (void*)(ip_header+1);
		sport = be16_to_host(udp_header->source);
		dport = be16_to_host(udp_header->dest);
		CALICO_DEBUG_AT("UDP; ports: s=%d d=%d\n", sport, dport);
		break;
	case IPPROTO_ICMP:
		icmp_header = (void*)(ip_header+1);
		sport = 0;
		dport = 0;
		CALICO_DEBUG_AT("ICMP; ports: type=%d code=%d\n",
				icmp_header->type, icmp_header->code);
		break;
	default:
		CALICO_DEBUG_AT("Unknown protocol, unable to extract ports\n");
		sport = 0;
		dport = 0;
	}

	// For host endpoints, execute do-not-track policy (will be no-op for other endpoints).
	__be32 ip_src = ip_header->saddr;
	__be32 ip_dst = ip_header->daddr;
	enum calico_policy_result pol_rc = maybe_execute_policy_do_not_track(
			skb, ip_proto, ip_src, ip_dst, sport, dport, flags);
	switch (pol_rc) {
	case CALICO_POL_DENY:
		CALICO_DEBUG_AT("Denied by do-not-track policy: DROP\n");
		goto deny;
	case CALICO_POL_ALLOW:
		CALICO_DEBUG_AT("Allowed by do-not-track policy: ACCEPT\n");
		goto allow;
	default:
		break;
	}

	struct calico_ct_result ct_result;
	switch (ip_proto) {
	case IPPROTO_TCP:
		// Now, do conntrack lookup.
		ct_result = calico_ct_v4_tcp_lookup(ip_src, ip_dst, sport, dport, tcp_header, flags);
		break;
	case IPPROTO_UDP:
		ct_result = calico_ct_v4_udp_lookup(ip_src, ip_dst, sport, dport, flags);
		break;
	case IPPROTO_ICMP:
		ct_result = calico_ct_v4_icmp_lookup(ip_src, ip_dst, icmp_header, flags);
		break;
	default:
		goto deny;
	}

	size_t csum_offset;
	switch (ct_result.rc){
	case CALICO_CT_NEW:
		// New connection, apply policy.

		// Execute pre-DNAT policy.
		pol_rc = maybe_execute_policy_pre_dnat(skb, ip_proto, ip_src, ip_dst,  sport,  dport, flags);
		if (pol_rc == CALICO_POL_DENY) {
			CALICO_DEBUG_AT("Denied by do-not-track policy: DROP\n");
			goto deny;
		} // Other RCs handled below.

		// Do a NAT table lookup.
		struct calico_nat_dest *nat_dest = calico_v4_nat_lookup(ip_proto, ip_dst, dport, flags);
		__be32 post_nat_ip_dst;
		__u16 post_nat_dport;
		if (nat_dest != NULL) {
			// If the packet passes policy, we'll NAT it below, for now, just
			// update the dest IP/port for the policy lookup.
			post_nat_ip_dst = nat_dest->addr;
			post_nat_dport = nat_dest->port;
		} else {
			post_nat_ip_dst = ip_dst;
			post_nat_dport = dport;
		}

		if (pol_rc == CALICO_POL_NO_MATCH) {
			// No match in pre-DNAT policy, apply normal policy.
			// TODO apply-on-forward policy
			if (false) {
				pol_rc = execute_policy_aof(skb, ip_proto, ip_src, post_nat_ip_dst,  sport,  post_nat_dport, flags);
			}
			pol_rc = execute_policy_norm(skb, ip_proto, ip_src, post_nat_ip_dst,  sport,  post_nat_dport, flags);
		}
		switch (pol_rc) {
		case CALICO_POL_NO_MATCH:
			CALICO_DEBUG_AT("Implicitly denied by normal policy: DROP\n");
			goto deny;
		case CALICO_POL_DENY:
			CALICO_DEBUG_AT("Denied by normal policy: DROP\n");
			goto deny;
		case CALICO_POL_ALLOW:
			CALICO_DEBUG_AT("Allowed by normal policy: ACCEPT\n");
		}

		// If we get here, we've passed policy.
		if (nat_dest != NULL) {
			// Packet is to be NATted, need to record a NAT entry.
			switch (ip_proto) {
			case IPPROTO_TCP:
				if ((void*)(tcp_header+1) > (void *)(long)skb->data_end) {
					CALICO_DEBUG_AT("Too short for TCP: DROP\n");
					goto deny;
				}
				calico_ct_v4_tcp_create_nat(skb, ip_src, ip_dst, sport, dport, post_nat_ip_dst, post_nat_dport, tcp_header, flags);
				break;
			case IPPROTO_UDP:
				calico_ct_v4_udp_create_nat(skb, ip_src, ip_dst, sport, dport, post_nat_ip_dst, post_nat_dport, flags);
				break;
			case IPPROTO_ICMP:
				calico_ct_v4_icmp_create_nat(skb, ip_src, ip_dst, post_nat_ip_dst, flags);
				break;
			}

			// Actually do the NAT.
			ip_header->daddr = post_nat_ip_dst;
			size_t csum_offset;

			switch (ip_proto) {
			case IPPROTO_TCP:
				tcp_header->dest = post_nat_dport;
				csum_offset = sizeof(struct ethhdr) + sizeof(struct iphdr) + offsetof(struct tcphdr, check);
				bpf_l4_csum_replace(skb, csum_offset, ip_dst, post_nat_ip_dst, BPF_F_PSEUDO_HDR | 4);
				bpf_l4_csum_replace(skb, csum_offset, host_to_be16(dport),  post_nat_dport, 2);
				break;
			case IPPROTO_UDP:
				udp_header->dest = post_nat_dport;
				csum_offset = sizeof(struct ethhdr) + sizeof(struct iphdr) + offsetof(struct udphdr, check);
				bpf_l4_csum_replace(skb, csum_offset, ip_dst, post_nat_ip_dst, BPF_F_PSEUDO_HDR | 4);
				bpf_l4_csum_replace(skb, csum_offset, host_to_be16(dport),  post_nat_dport, 2);
				break;
			}

			bpf_l3_csum_replace(skb, sizeof(struct ethhdr) + offsetof(struct iphdr, check), ip_dst, post_nat_ip_dst, 4);
		} else {
			// No NAT for this packet, record a simple entry.
			switch (ip_proto) {
			case IPPROTO_TCP:
				if ((void*)(tcp_header+1) > (void *)(long)skb->data_end) {
					CALICO_DEBUG_AT("Too short for TCP: DROP\n");
					goto deny;
				}
				calico_ct_v4_tcp_create(skb, ip_src, ip_dst, sport, dport, tcp_header, flags);
				break;
			case IPPROTO_UDP:
				calico_ct_v4_udp_create(skb, ip_src, ip_dst, sport, dport, flags);
				break;
			case IPPROTO_ICMP:
				calico_ct_v4_icmp_create(skb, ip_src, ip_dst, flags);
				break;
			}
		}

		fib_params.sport = sport;
		fib_params.dport = post_nat_dport;
		fib_params.ipv4_src = ip_src;
		fib_params.ipv4_dst = post_nat_ip_dst;

		goto allow;
	case CALICO_CT_ESTABLISHED:
		fib_params.l4_protocol = ip_proto;
		fib_params.sport = sport;
		fib_params.dport = dport;
		fib_params.ipv4_src = ip_src;
		fib_params.ipv4_dst = ip_dst;

		goto allow;
	case CALICO_CT_ESTABLISHED_DNAT:
		CALICO_DEBUG_AT("CT: DNAT\n");

		// Actually do the NAT.
		ip_header->daddr = ct_result.nat_ip;

		switch (ip_proto) {
		case IPPROTO_TCP:
			tcp_header->dest = ct_result.nat_port;
			csum_offset = sizeof(struct ethhdr) + sizeof(struct iphdr) + offsetof(struct tcphdr, check);
			bpf_l4_csum_replace(skb, csum_offset, ip_dst, ct_result.nat_ip, BPF_F_PSEUDO_HDR | 4);
			bpf_l4_csum_replace(skb, csum_offset, host_to_be16(dport),  ct_result.nat_port, 2);
			break;
		case IPPROTO_UDP:
			udp_header->dest = ct_result.nat_port;
			csum_offset = sizeof(struct ethhdr) + sizeof(struct iphdr) + offsetof(struct udphdr, check);
			bpf_l4_csum_replace(skb, csum_offset, ip_dst, ct_result.nat_ip, BPF_F_PSEUDO_HDR | 4);
			bpf_l4_csum_replace(skb, csum_offset, host_to_be16(dport),  ct_result.nat_port, 2);
			break;
		default:
			// ICMP has no checksum.
			goto skip_l4_csum;
		}

		bpf_l3_csum_replace(skb, sizeof(struct ethhdr) + offsetof(struct iphdr, check), ip_dst, ct_result.nat_ip, 4);

		fib_params.sport = sport;
		fib_params.dport = ct_result.nat_port;
		fib_params.ipv4_src = ip_src;
		fib_params.ipv4_dst = ct_result.nat_ip;

		goto allow;
	case CALICO_CT_ESTABLISHED_SNAT:
		CALICO_DEBUG_AT("CT: SNAT\n");

		// Actually do the NAT.
		ip_header->saddr = ct_result.nat_ip;

		switch (ip_proto) {
		case IPPROTO_TCP:
			tcp_header->source = ct_result.nat_port;
			csum_offset = sizeof(struct ethhdr) + sizeof(struct iphdr) + offsetof(struct tcphdr, check);
			break;
		case IPPROTO_UDP:
			udp_header->source = ct_result.nat_port;
			csum_offset = sizeof(struct ethhdr) + sizeof(struct iphdr) + offsetof(struct udphdr, check);
			break;
		default:
			// ICMP has no checksum.
			goto skip_l4_csum;
		}

		bpf_l4_csum_replace(skb, csum_offset, ip_src, ct_result.nat_ip, BPF_F_PSEUDO_HDR | 4);
		bpf_l4_csum_replace(skb, csum_offset, host_to_be16(sport),  ct_result.nat_port, 2);

	skip_l4_csum:
		bpf_l3_csum_replace(skb, sizeof(struct ethhdr) + offsetof(struct iphdr, check), ip_src, ct_result.nat_ip, 4);

		fib_params.sport = ct_result.nat_port;
		fib_params.dport = dport;
		fib_params.ipv4_src = ct_result.nat_ip;
		fib_params.ipv4_dst = ip_dst;

		goto allow;
	default:
		goto deny;
	}

	// Try a short-circuit FIB lookup.
	allow:

	if (((flags & CALICO_TC_HOST_EP) && (flags & CALICO_TC_INGRESS)) ||
			(!(flags & CALICO_TC_HOST_EP) && !(flags & CALICO_TC_INGRESS))) {
		CALICO_DEBUG_AT("Traffic is towards the host namespace, doing Linux FIB lookup\n");
		fib_params.l4_protocol = ip_proto;
		rc =  bpf_fib_lookup(skb, &fib_params, sizeof(fib_params), 0);
		if (rc == 0) {
			CALICO_DEBUG_AT("FIB lookup succeeded\n");
			// Update the MACs.  NAT may have invalidated pointer into the packet so need to
			// revalidate.
		    if ((void *)(long)skb->data + sizeof(struct ethhdr) > (void *)(long)skb->data_end) {
				CALICO_DEBUG_AT("BUG: packet got shorter?\n");
				reason = CALICO_REASON_SHORT;
				goto deny;
			}
		    eth_hdr = (void *)(long)skb->data;
			__builtin_memcpy(&eth_hdr->h_source, &fib_params.smac, sizeof(eth_hdr->h_source));
			__builtin_memcpy(&eth_hdr->h_dest, &fib_params.dmac, sizeof(eth_hdr->h_dest));

			// Redirect the packet.
			CALICO_DEBUG_AT("Got Linux FIB hit, redirecting to iface %d.\n", fib_params.ifindex);
			rc = bpf_redirect(fib_params.ifindex, 0);
		} else if (rc < 0) {
			CALICO_DEBUG_AT("FIB lookup failed (bad input): %d.\n", rc);
			rc = TC_ACT_UNSPEC;
		} else {
			CALICO_DEBUG_AT("FIB lookup failed (FIB problem): %d.\n", rc);
			rc = TC_ACT_UNSPEC;
		}
	}

	allow_skip_fib:
	if (!(flags & CALICO_TC_HOST_EP) && !(flags & CALICO_TC_INGRESS)) {
		// Packet is leaving workload, mark it so any downstream programs know this traffic was from a workload.
		CALICO_DEBUG_AT("Traffic is from workload, applying packet mark.\n");
		// FIXME: this ignores the mask that we should be using.  However, if we mask off the bits, then
		// clang spots that it can do a 16-bit store instead of a 32-bit load/modify/store, which trips
		// up the validator.
		skb->mark = CALICO_SKB_MARK_FROM_WORKLOAD;
	}

	if (CALICO_LOG_LEVEL >= CALICO_LOG_LEVEL_INFO) {
		uint64_t prog_end_time = bpf_ktime_get_ns();
		CALICO_INFO_AT("Final result=ALLOW (%d). Program execution time: %lluns T: %lluns\n", rc, prog_end_time-prog_start_time, timer_end_time-timer_start_time);
	}
	return rc;

	deny:
	if (CALICO_LOG_LEVEL >= CALICO_LOG_LEVEL_INFO) {
		uint64_t prog_end_time = bpf_ktime_get_ns();
		CALICO_INFO_AT("Final result=DENY (%x). Program execution time: %lluns\n", reason, prog_end_time-prog_start_time);
	}
	return TC_ACT_SHOT;
}

// Handle packets that arrive at the host namespace from a workload.
__attribute__((section("calico_from_workload")))
int tc_calico_from_workload(struct __sk_buff *skb) {
	return calico_tc(skb, 0);
}

// Handle packets that going to a workload from the host namespace..
__attribute__((section("calico_to_workload")))
int tc_calico_to_workload(struct __sk_buff *skb) {
	return calico_tc(skb, CALICO_TC_INGRESS);
}

// Handle packets that arrive at the host namespace from a host endpoint.
__attribute__((section("calico_from_host_endpoint")))
int tc_calico_from_host_endpoint(struct __sk_buff *skb) {
	return calico_tc(skb, CALICO_TC_HOST_EP | CALICO_TC_INGRESS);
}

// Handle packets that are leaving a host towards a host endpoint.
__attribute__((section("calico_to_host_endpoint")))
int tc_calico_to_host_endpoint(struct __sk_buff *skb) {
	return calico_tc(skb, CALICO_TC_HOST_EP);
}


char ____license[] __attribute__((section("license"), used)) = "GPL";
