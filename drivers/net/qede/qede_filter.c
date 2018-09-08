/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2017 Cavium Inc.
 * All rights reserved.
 * www.cavium.com
 */

#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_sctp.h>
#include <rte_errno.h>

#include "qede_ethdev.h"

/* VXLAN tunnel classification mapping */
const struct _qede_udp_tunn_types {
	uint16_t rte_filter_type;
	enum ecore_filter_ucast_type qede_type;
	enum ecore_tunn_clss qede_tunn_clss;
	const char *string;
} qede_tunn_types[] = {
	{
		ETH_TUNNEL_FILTER_OMAC,
		ECORE_FILTER_MAC,
		ECORE_TUNN_CLSS_MAC_VLAN,
		"outer-mac"
	},
	{
		ETH_TUNNEL_FILTER_TENID,
		ECORE_FILTER_VNI,
		ECORE_TUNN_CLSS_MAC_VNI,
		"vni"
	},
	{
		ETH_TUNNEL_FILTER_IMAC,
		ECORE_FILTER_INNER_MAC,
		ECORE_TUNN_CLSS_INNER_MAC_VLAN,
		"inner-mac"
	},
	{
		ETH_TUNNEL_FILTER_IVLAN,
		ECORE_FILTER_INNER_VLAN,
		ECORE_TUNN_CLSS_INNER_MAC_VLAN,
		"inner-vlan"
	},
	{
		ETH_TUNNEL_FILTER_OMAC | ETH_TUNNEL_FILTER_TENID,
		ECORE_FILTER_MAC_VNI_PAIR,
		ECORE_TUNN_CLSS_MAC_VNI,
		"outer-mac and vni"
	},
	{
		ETH_TUNNEL_FILTER_OMAC | ETH_TUNNEL_FILTER_IMAC,
		ECORE_FILTER_UNUSED,
		MAX_ECORE_TUNN_CLSS,
		"outer-mac and inner-mac"
	},
	{
		ETH_TUNNEL_FILTER_OMAC | ETH_TUNNEL_FILTER_IVLAN,
		ECORE_FILTER_UNUSED,
		MAX_ECORE_TUNN_CLSS,
		"outer-mac and inner-vlan"
	},
	{
		ETH_TUNNEL_FILTER_TENID | ETH_TUNNEL_FILTER_IMAC,
		ECORE_FILTER_INNER_MAC_VNI_PAIR,
		ECORE_TUNN_CLSS_INNER_MAC_VNI,
		"vni and inner-mac",
	},
	{
		ETH_TUNNEL_FILTER_TENID | ETH_TUNNEL_FILTER_IVLAN,
		ECORE_FILTER_UNUSED,
		MAX_ECORE_TUNN_CLSS,
		"vni and inner-vlan",
	},
	{
		ETH_TUNNEL_FILTER_IMAC | ETH_TUNNEL_FILTER_IVLAN,
		ECORE_FILTER_INNER_PAIR,
		ECORE_TUNN_CLSS_INNER_MAC_VLAN,
		"inner-mac and inner-vlan",
	},
	{
		ETH_TUNNEL_FILTER_OIP,
		ECORE_FILTER_UNUSED,
		MAX_ECORE_TUNN_CLSS,
		"outer-IP"
	},
	{
		ETH_TUNNEL_FILTER_IIP,
		ECORE_FILTER_UNUSED,
		MAX_ECORE_TUNN_CLSS,
		"inner-IP"
	},
	{
		RTE_TUNNEL_FILTER_IMAC_IVLAN,
		ECORE_FILTER_UNUSED,
		MAX_ECORE_TUNN_CLSS,
		"IMAC_IVLAN"
	},
	{
		RTE_TUNNEL_FILTER_IMAC_IVLAN_TENID,
		ECORE_FILTER_UNUSED,
		MAX_ECORE_TUNN_CLSS,
		"IMAC_IVLAN_TENID"
	},
	{
		RTE_TUNNEL_FILTER_IMAC_TENID,
		ECORE_FILTER_UNUSED,
		MAX_ECORE_TUNN_CLSS,
		"IMAC_TENID"
	},
	{
		RTE_TUNNEL_FILTER_OMAC_TENID_IMAC,
		ECORE_FILTER_UNUSED,
		MAX_ECORE_TUNN_CLSS,
		"OMAC_TENID_IMAC"
	},
};

#define IP_VERSION				(0x40)
#define IP_HDRLEN				(0x5)
#define QEDE_FDIR_IP_DEFAULT_VERSION_IHL	(IP_VERSION | IP_HDRLEN)
#define QEDE_FDIR_TCP_DEFAULT_DATAOFF		(0x50)
#define QEDE_FDIR_IPV4_DEF_TTL			(64)
#define QEDE_FDIR_IPV6_DEFAULT_VTC_FLOW		(0x60000000)
/* Sum of length of header types of L2, L3, L4.
 * L2 : ether_hdr + vlan_hdr + vxlan_hdr
 * L3 : ipv6_hdr
 * L4 : tcp_hdr
 */
#define QEDE_MAX_FDIR_PKT_LEN			(86)

#ifndef IPV6_ADDR_LEN
#define IPV6_ADDR_LEN				(16)
#endif

static inline bool qede_valid_flow(uint16_t flow_type)
{
	return  ((flow_type == RTE_ETH_FLOW_NONFRAG_IPV4_TCP) ||
		 (flow_type == RTE_ETH_FLOW_NONFRAG_IPV4_UDP) ||
		 (flow_type == RTE_ETH_FLOW_NONFRAG_IPV6_TCP) ||
		 (flow_type == RTE_ETH_FLOW_NONFRAG_IPV6_UDP));
}

/* Note: Flowdir support is only partial.
 * For ex: drop_queue, FDIR masks, flex_conf are not supported.
 * Parameters like pballoc/status fields are irrelevant here.
 */
int qede_check_fdir_support(struct rte_eth_dev *eth_dev)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	struct rte_fdir_conf *fdir = &eth_dev->data->dev_conf.fdir_conf;

	/* check FDIR modes */
	switch (fdir->mode) {
	case RTE_FDIR_MODE_NONE:
		qdev->fdir_info.arfs.arfs_enable = false;
		DP_INFO(edev, "flowdir is disabled\n");
	break;
	case RTE_FDIR_MODE_PERFECT:
		if (ECORE_IS_CMT(edev)) {
			DP_ERR(edev, "flowdir is not supported in 100G mode\n");
			qdev->fdir_info.arfs.arfs_enable = false;
			return -ENOTSUP;
		}
		qdev->fdir_info.arfs.arfs_enable = true;
		DP_INFO(edev, "flowdir is enabled\n");
	break;
	case RTE_FDIR_MODE_PERFECT_TUNNEL:
	case RTE_FDIR_MODE_SIGNATURE:
	case RTE_FDIR_MODE_PERFECT_MAC_VLAN:
		DP_ERR(edev, "Unsupported flowdir mode %d\n", fdir->mode);
		return -ENOTSUP;
	}

	return 0;
}

void qede_fdir_dealloc_resc(struct rte_eth_dev *eth_dev)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct qede_fdir_entry *tmp = NULL;

	SLIST_FOREACH(tmp, &qdev->fdir_info.fdir_list_head, list) {
		if (tmp) {
			if (tmp->mz)
				rte_memzone_free(tmp->mz);
			SLIST_REMOVE(&qdev->fdir_info.fdir_list_head, tmp,
				     qede_fdir_entry, list);
			rte_free(tmp);
		}
	}
}

static int
qede_config_cmn_fdir_filter(struct rte_eth_dev *eth_dev,
			    struct rte_eth_fdir_filter *fdir_filter,
			    bool add)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	char mz_name[RTE_MEMZONE_NAMESIZE] = {0};
	struct qede_fdir_entry *tmp = NULL;
	struct qede_fdir_entry *fdir = NULL;
	const struct rte_memzone *mz;
	struct ecore_hwfn *p_hwfn;
	enum _ecore_status_t rc;
	uint16_t pkt_len;
	void *pkt;

	if (add) {
		if (qdev->fdir_info.filter_count == QEDE_RFS_MAX_FLTR - 1) {
			DP_ERR(edev, "Reached max flowdir filter limit\n");
			return -EINVAL;
		}
		fdir = rte_malloc(NULL, sizeof(struct qede_fdir_entry),
				  RTE_CACHE_LINE_SIZE);
		if (!fdir) {
			DP_ERR(edev, "Did not allocate memory for fdir\n");
			return -ENOMEM;
		}
	}
	/* soft_id could have been used as memzone string, but soft_id is
	 * not currently used so it has no significance.
	 */
	snprintf(mz_name, sizeof(mz_name) - 1, "%lx",
		 (unsigned long)rte_get_timer_cycles());
	mz = rte_memzone_reserve_aligned(mz_name, QEDE_MAX_FDIR_PKT_LEN,
					 SOCKET_ID_ANY, 0, RTE_CACHE_LINE_SIZE);
	if (!mz) {
		DP_ERR(edev, "Failed to allocate memzone for fdir, err = %s\n",
		       rte_strerror(rte_errno));
		rc = -rte_errno;
		goto err1;
	}

	pkt = mz->addr;
	memset(pkt, 0, QEDE_MAX_FDIR_PKT_LEN);
	pkt_len = qede_fdir_construct_pkt(eth_dev, fdir_filter, pkt,
					  &qdev->fdir_info.arfs);
	if (pkt_len == 0) {
		rc = -EINVAL;
		goto err2;
	}
	DP_INFO(edev, "pkt_len = %u memzone = %s\n", pkt_len, mz_name);
	if (add) {
		SLIST_FOREACH(tmp, &qdev->fdir_info.fdir_list_head, list) {
			if (memcmp(tmp->mz->addr, pkt, pkt_len) == 0) {
				DP_INFO(edev, "flowdir filter exist\n");
				rc = 0;
				goto err2;
			}
		}
	} else {
		SLIST_FOREACH(tmp, &qdev->fdir_info.fdir_list_head, list) {
			if (memcmp(tmp->mz->addr, pkt, pkt_len) == 0)
				break;
		}
		if (!tmp) {
			DP_ERR(edev, "flowdir filter does not exist\n");
			rc = -EEXIST;
			goto err2;
		}
	}
	p_hwfn = ECORE_LEADING_HWFN(edev);
	if (add) {
		if (!qdev->fdir_info.arfs.arfs_enable) {
			/* Force update */
			eth_dev->data->dev_conf.fdir_conf.mode =
						RTE_FDIR_MODE_PERFECT;
			qdev->fdir_info.arfs.arfs_enable = true;
			DP_INFO(edev, "Force enable flowdir in perfect mode\n");
		}
		/* Enable ARFS searcher with updated flow_types */
		ecore_arfs_mode_configure(p_hwfn, p_hwfn->p_arfs_ptt,
					  &qdev->fdir_info.arfs);
	}
	/* configure filter with ECORE_SPQ_MODE_EBLOCK */
	rc = ecore_configure_rfs_ntuple_filter(p_hwfn, NULL,
					       (dma_addr_t)mz->iova,
					       pkt_len,
					       fdir_filter->action.rx_queue,
					       0, add);
	if (rc == ECORE_SUCCESS) {
		if (add) {
			fdir->rx_queue = fdir_filter->action.rx_queue;
			fdir->pkt_len = pkt_len;
			fdir->mz = mz;
			SLIST_INSERT_HEAD(&qdev->fdir_info.fdir_list_head,
					  fdir, list);
			qdev->fdir_info.filter_count++;
			DP_INFO(edev, "flowdir filter added, count = %d\n",
				qdev->fdir_info.filter_count);
		} else {
			rte_memzone_free(tmp->mz);
			SLIST_REMOVE(&qdev->fdir_info.fdir_list_head, tmp,
				     qede_fdir_entry, list);
			rte_free(tmp); /* the node deleted */
			rte_memzone_free(mz); /* temp node allocated */
			qdev->fdir_info.filter_count--;
			DP_INFO(edev, "Fdir filter deleted, count = %d\n",
				qdev->fdir_info.filter_count);
		}
	} else {
		DP_ERR(edev, "flowdir filter failed, rc=%d filter_count=%d\n",
		       rc, qdev->fdir_info.filter_count);
	}

	/* Disable ARFS searcher if there are no more filters */
	if (qdev->fdir_info.filter_count == 0) {
		memset(&qdev->fdir_info.arfs, 0,
		       sizeof(struct ecore_arfs_config_params));
		DP_INFO(edev, "Disabling flowdir\n");
		qdev->fdir_info.arfs.arfs_enable = false;
		ecore_arfs_mode_configure(p_hwfn, p_hwfn->p_arfs_ptt,
					  &qdev->fdir_info.arfs);
	}
	return 0;

err2:
	rte_memzone_free(mz);
err1:
	if (add)
		rte_free(fdir);
	return rc;
}

static int
qede_fdir_filter_add(struct rte_eth_dev *eth_dev,
		     struct rte_eth_fdir_filter *fdir,
		     bool add)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);

	if (!qede_valid_flow(fdir->input.flow_type)) {
		DP_ERR(edev, "invalid flow_type input\n");
		return -EINVAL;
	}

	if (fdir->action.rx_queue >= QEDE_RSS_COUNT(qdev)) {
		DP_ERR(edev, "invalid queue number %u\n",
		       fdir->action.rx_queue);
		return -EINVAL;
	}

	if (fdir->input.flow_ext.is_vf) {
		DP_ERR(edev, "flowdir is not supported over VF\n");
		return -EINVAL;
	}

	return qede_config_cmn_fdir_filter(eth_dev, fdir, add);
}

/* Fills the L3/L4 headers and returns the actual length  of flowdir packet */
uint16_t
qede_fdir_construct_pkt(struct rte_eth_dev *eth_dev,
			struct rte_eth_fdir_filter *fdir,
			void *buff,
			struct ecore_arfs_config_params *params)

{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	uint16_t *ether_type;
	uint8_t *raw_pkt;
	struct rte_eth_fdir_input *input;
	static uint8_t vlan_frame[] = {0x81, 0, 0, 0};
	struct ipv4_hdr *ip;
	struct ipv6_hdr *ip6;
	struct udp_hdr *udp;
	struct tcp_hdr *tcp;
	uint16_t len;
	static const uint8_t next_proto[] = {
		[RTE_ETH_FLOW_NONFRAG_IPV4_TCP] = IPPROTO_TCP,
		[RTE_ETH_FLOW_NONFRAG_IPV4_UDP] = IPPROTO_UDP,
		[RTE_ETH_FLOW_NONFRAG_IPV6_TCP] = IPPROTO_TCP,
		[RTE_ETH_FLOW_NONFRAG_IPV6_UDP] = IPPROTO_UDP,
	};
	raw_pkt = (uint8_t *)buff;
	input = &fdir->input;
	DP_INFO(edev, "flow_type %d\n", input->flow_type);

	len =  2 * sizeof(struct ether_addr);
	raw_pkt += 2 * sizeof(struct ether_addr);
	if (input->flow_ext.vlan_tci) {
		DP_INFO(edev, "adding VLAN header\n");
		rte_memcpy(raw_pkt, vlan_frame, sizeof(vlan_frame));
		rte_memcpy(raw_pkt + sizeof(uint16_t),
			   &input->flow_ext.vlan_tci,
			   sizeof(uint16_t));
		raw_pkt += sizeof(vlan_frame);
		len += sizeof(vlan_frame);
	}
	ether_type = (uint16_t *)raw_pkt;
	raw_pkt += sizeof(uint16_t);
	len += sizeof(uint16_t);

	switch (input->flow_type) {
	case RTE_ETH_FLOW_NONFRAG_IPV4_TCP:
	case RTE_ETH_FLOW_NONFRAG_IPV4_UDP:
		/* fill the common ip header */
		ip = (struct ipv4_hdr *)raw_pkt;
		*ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
		ip->version_ihl = QEDE_FDIR_IP_DEFAULT_VERSION_IHL;
		ip->total_length = sizeof(struct ipv4_hdr);
		ip->next_proto_id = input->flow.ip4_flow.proto ?
				    input->flow.ip4_flow.proto :
				    next_proto[input->flow_type];
		ip->time_to_live = input->flow.ip4_flow.ttl ?
				   input->flow.ip4_flow.ttl :
				   QEDE_FDIR_IPV4_DEF_TTL;
		ip->type_of_service = input->flow.ip4_flow.tos;
		ip->dst_addr = input->flow.ip4_flow.dst_ip;
		ip->src_addr = input->flow.ip4_flow.src_ip;
		len += sizeof(struct ipv4_hdr);
		params->ipv4 = true;

		raw_pkt = (uint8_t *)buff;
		/* UDP */
		if (input->flow_type == RTE_ETH_FLOW_NONFRAG_IPV4_UDP) {
			udp = (struct udp_hdr *)(raw_pkt + len);
			udp->dst_port = input->flow.udp4_flow.dst_port;
			udp->src_port = input->flow.udp4_flow.src_port;
			udp->dgram_len = sizeof(struct udp_hdr);
			len += sizeof(struct udp_hdr);
			/* adjust ip total_length */
			ip->total_length += sizeof(struct udp_hdr);
			params->udp = true;
		} else { /* TCP */
			tcp = (struct tcp_hdr *)(raw_pkt + len);
			tcp->src_port = input->flow.tcp4_flow.src_port;
			tcp->dst_port = input->flow.tcp4_flow.dst_port;
			tcp->data_off = QEDE_FDIR_TCP_DEFAULT_DATAOFF;
			len += sizeof(struct tcp_hdr);
			/* adjust ip total_length */
			ip->total_length += sizeof(struct tcp_hdr);
			params->tcp = true;
		}
		break;
	case RTE_ETH_FLOW_NONFRAG_IPV6_TCP:
	case RTE_ETH_FLOW_NONFRAG_IPV6_UDP:
		ip6 = (struct ipv6_hdr *)raw_pkt;
		*ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv6);
		ip6->proto = input->flow.ipv6_flow.proto ?
					input->flow.ipv6_flow.proto :
					next_proto[input->flow_type];
		ip6->vtc_flow =
			rte_cpu_to_be_32(QEDE_FDIR_IPV6_DEFAULT_VTC_FLOW);

		rte_memcpy(&ip6->src_addr, &input->flow.ipv6_flow.src_ip,
			   IPV6_ADDR_LEN);
		rte_memcpy(&ip6->dst_addr, &input->flow.ipv6_flow.dst_ip,
			   IPV6_ADDR_LEN);
		len += sizeof(struct ipv6_hdr);
		params->ipv6 = true;

		raw_pkt = (uint8_t *)buff;
		/* UDP */
		if (input->flow_type == RTE_ETH_FLOW_NONFRAG_IPV6_UDP) {
			udp = (struct udp_hdr *)(raw_pkt + len);
			udp->src_port = input->flow.udp6_flow.src_port;
			udp->dst_port = input->flow.udp6_flow.dst_port;
			len += sizeof(struct udp_hdr);
			params->udp = true;
		} else { /* TCP */
			tcp = (struct tcp_hdr *)(raw_pkt + len);
			tcp->src_port = input->flow.tcp6_flow.src_port;
			tcp->dst_port = input->flow.tcp6_flow.dst_port;
			tcp->data_off = QEDE_FDIR_TCP_DEFAULT_DATAOFF;
			len += sizeof(struct tcp_hdr);
			params->tcp = true;
		}
		break;
	default:
		DP_ERR(edev, "Unsupported flow_type %u\n",
		       input->flow_type);
		return 0;
	}

	return len;
}

static int
qede_fdir_filter_conf(struct rte_eth_dev *eth_dev,
		      enum rte_filter_op filter_op,
		      void *arg)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	struct rte_eth_fdir_filter *fdir;
	int ret;

	fdir = (struct rte_eth_fdir_filter *)arg;
	switch (filter_op) {
	case RTE_ETH_FILTER_NOP:
		/* Typically used to query flowdir support */
		if (ECORE_IS_CMT(edev)) {
			DP_ERR(edev, "flowdir is not supported in 100G mode\n");
			return -ENOTSUP;
		}
		return 0; /* means supported */
	case RTE_ETH_FILTER_ADD:
		ret = qede_fdir_filter_add(eth_dev, fdir, 1);
	break;
	case RTE_ETH_FILTER_DELETE:
		ret = qede_fdir_filter_add(eth_dev, fdir, 0);
	break;
	case RTE_ETH_FILTER_FLUSH:
	case RTE_ETH_FILTER_UPDATE:
	case RTE_ETH_FILTER_INFO:
		return -ENOTSUP;
	break;
	default:
		DP_ERR(edev, "unknown operation %u", filter_op);
		ret = -EINVAL;
	}

	return ret;
}

int qede_ntuple_filter_conf(struct rte_eth_dev *eth_dev,
			    enum rte_filter_op filter_op,
			    void *arg)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	struct rte_eth_ntuple_filter *ntuple;
	struct rte_eth_fdir_filter fdir_entry;
	struct rte_eth_tcpv4_flow *tcpv4_flow;
	struct rte_eth_udpv4_flow *udpv4_flow;
	bool add = false;

	switch (filter_op) {
	case RTE_ETH_FILTER_NOP:
		/* Typically used to query fdir support */
		if (ECORE_IS_CMT(edev)) {
			DP_ERR(edev, "flowdir is not supported in 100G mode\n");
			return -ENOTSUP;
		}
		return 0; /* means supported */
	case RTE_ETH_FILTER_ADD:
		add = true;
	break;
	case RTE_ETH_FILTER_DELETE:
	break;
	case RTE_ETH_FILTER_INFO:
	case RTE_ETH_FILTER_GET:
	case RTE_ETH_FILTER_UPDATE:
	case RTE_ETH_FILTER_FLUSH:
	case RTE_ETH_FILTER_SET:
	case RTE_ETH_FILTER_STATS:
	case RTE_ETH_FILTER_OP_MAX:
		DP_ERR(edev, "Unsupported filter_op %d\n", filter_op);
		return -ENOTSUP;
	}
	ntuple = (struct rte_eth_ntuple_filter *)arg;
	/* Internally convert ntuple to fdir entry */
	memset(&fdir_entry, 0, sizeof(fdir_entry));
	if (ntuple->proto == IPPROTO_TCP) {
		fdir_entry.input.flow_type = RTE_ETH_FLOW_NONFRAG_IPV4_TCP;
		tcpv4_flow = &fdir_entry.input.flow.tcp4_flow;
		tcpv4_flow->ip.src_ip = ntuple->src_ip;
		tcpv4_flow->ip.dst_ip = ntuple->dst_ip;
		tcpv4_flow->ip.proto = IPPROTO_TCP;
		tcpv4_flow->src_port = ntuple->src_port;
		tcpv4_flow->dst_port = ntuple->dst_port;
	} else {
		fdir_entry.input.flow_type = RTE_ETH_FLOW_NONFRAG_IPV4_UDP;
		udpv4_flow = &fdir_entry.input.flow.udp4_flow;
		udpv4_flow->ip.src_ip = ntuple->src_ip;
		udpv4_flow->ip.dst_ip = ntuple->dst_ip;
		udpv4_flow->ip.proto = IPPROTO_TCP;
		udpv4_flow->src_port = ntuple->src_port;
		udpv4_flow->dst_port = ntuple->dst_port;
	}

	fdir_entry.action.rx_queue = ntuple->queue;

	return qede_config_cmn_fdir_filter(eth_dev, &fdir_entry, add);
}

static int
qede_tunnel_update(struct qede_dev *qdev,
		   struct ecore_tunnel_info *tunn_info)
{
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	enum _ecore_status_t rc = ECORE_INVAL;
	struct ecore_hwfn *p_hwfn;
	struct ecore_ptt *p_ptt;
	int i;

	for_each_hwfn(edev, i) {
		p_hwfn = &edev->hwfns[i];
		if (IS_PF(edev)) {
			p_ptt = ecore_ptt_acquire(p_hwfn);
			if (!p_ptt) {
				DP_ERR(p_hwfn, "Can't acquire PTT\n");
				return -EAGAIN;
			}
		} else {
			p_ptt = NULL;
		}

		rc = ecore_sp_pf_update_tunn_cfg(p_hwfn, p_ptt,
				tunn_info, ECORE_SPQ_MODE_CB, NULL);
		if (IS_PF(edev))
			ecore_ptt_release(p_hwfn, p_ptt);

		if (rc != ECORE_SUCCESS)
			break;
	}

	return rc;
}

static int
qede_vxlan_enable(struct rte_eth_dev *eth_dev, uint8_t clss,
		  bool enable)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	enum _ecore_status_t rc = ECORE_INVAL;
	struct ecore_tunnel_info tunn;

	if (qdev->vxlan.enable == enable)
		return ECORE_SUCCESS;

	memset(&tunn, 0, sizeof(struct ecore_tunnel_info));
	tunn.vxlan.b_update_mode = true;
	tunn.vxlan.b_mode_enabled = enable;
	tunn.b_update_rx_cls = true;
	tunn.b_update_tx_cls = true;
	tunn.vxlan.tun_cls = clss;

	tunn.vxlan_port.b_update_port = true;
	tunn.vxlan_port.port = enable ? QEDE_VXLAN_DEF_PORT : 0;

	rc = qede_tunnel_update(qdev, &tunn);
	if (rc == ECORE_SUCCESS) {
		qdev->vxlan.enable = enable;
		qdev->vxlan.udp_port = (enable) ? QEDE_VXLAN_DEF_PORT : 0;
		DP_INFO(edev, "vxlan is %s, UDP port = %d\n",
			enable ? "enabled" : "disabled", qdev->vxlan.udp_port);
	} else {
		DP_ERR(edev, "Failed to update tunn_clss %u\n",
		       tunn.vxlan.tun_cls);
	}

	return rc;
}

static int
qede_geneve_enable(struct rte_eth_dev *eth_dev, uint8_t clss,
		  bool enable)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	enum _ecore_status_t rc = ECORE_INVAL;
	struct ecore_tunnel_info tunn;

	memset(&tunn, 0, sizeof(struct ecore_tunnel_info));
	tunn.l2_geneve.b_update_mode = true;
	tunn.l2_geneve.b_mode_enabled = enable;
	tunn.ip_geneve.b_update_mode = true;
	tunn.ip_geneve.b_mode_enabled = enable;
	tunn.l2_geneve.tun_cls = clss;
	tunn.ip_geneve.tun_cls = clss;
	tunn.b_update_rx_cls = true;
	tunn.b_update_tx_cls = true;

	tunn.geneve_port.b_update_port = true;
	tunn.geneve_port.port = enable ? QEDE_GENEVE_DEF_PORT : 0;

	rc = qede_tunnel_update(qdev, &tunn);
	if (rc == ECORE_SUCCESS) {
		qdev->geneve.enable = enable;
		qdev->geneve.udp_port = (enable) ? QEDE_GENEVE_DEF_PORT : 0;
		DP_INFO(edev, "GENEVE is %s, UDP port = %d\n",
			enable ? "enabled" : "disabled", qdev->geneve.udp_port);
	} else {
		DP_ERR(edev, "Failed to update tunn_clss %u\n",
		       clss);
	}

	return rc;
}

static int
qede_ipgre_enable(struct rte_eth_dev *eth_dev, uint8_t clss,
		  bool enable)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	enum _ecore_status_t rc = ECORE_INVAL;
	struct ecore_tunnel_info tunn;

	memset(&tunn, 0, sizeof(struct ecore_tunnel_info));
	tunn.ip_gre.b_update_mode = true;
	tunn.ip_gre.b_mode_enabled = enable;
	tunn.ip_gre.tun_cls = clss;
	tunn.ip_gre.tun_cls = clss;
	tunn.b_update_rx_cls = true;
	tunn.b_update_tx_cls = true;

	rc = qede_tunnel_update(qdev, &tunn);
	if (rc == ECORE_SUCCESS) {
		qdev->ipgre.enable = enable;
		DP_INFO(edev, "IPGRE is %s\n",
			enable ? "enabled" : "disabled");
	} else {
		DP_ERR(edev, "Failed to update tunn_clss %u\n",
		       clss);
	}

	return rc;
}

int
qede_udp_dst_port_del(struct rte_eth_dev *eth_dev,
		      struct rte_eth_udp_tunnel *tunnel_udp)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	struct ecore_tunnel_info tunn; /* @DPDK */
	uint16_t udp_port;
	int rc;

	PMD_INIT_FUNC_TRACE(edev);

	memset(&tunn, 0, sizeof(tunn));

	switch (tunnel_udp->prot_type) {
	case RTE_TUNNEL_TYPE_VXLAN:
		if (qdev->vxlan.udp_port != tunnel_udp->udp_port) {
			DP_ERR(edev, "UDP port %u doesn't exist\n",
				tunnel_udp->udp_port);
			return ECORE_INVAL;
		}
		udp_port = 0;

		tunn.vxlan_port.b_update_port = true;
		tunn.vxlan_port.port = udp_port;

		rc = qede_tunnel_update(qdev, &tunn);
		if (rc != ECORE_SUCCESS) {
			DP_ERR(edev, "Unable to config UDP port %u\n",
			       tunn.vxlan_port.port);
			return rc;
		}

		qdev->vxlan.udp_port = udp_port;
		/* If the request is to delete UDP port and if the number of
		 * VXLAN filters have reached 0 then VxLAN offload can be be
		 * disabled.
		 */
		if (qdev->vxlan.enable && qdev->vxlan.num_filters == 0)
			return qede_vxlan_enable(eth_dev,
					ECORE_TUNN_CLSS_MAC_VLAN, false);

		break;
	case RTE_TUNNEL_TYPE_GENEVE:
		if (qdev->geneve.udp_port != tunnel_udp->udp_port) {
			DP_ERR(edev, "UDP port %u doesn't exist\n",
				tunnel_udp->udp_port);
			return ECORE_INVAL;
		}

		udp_port = 0;

		tunn.geneve_port.b_update_port = true;
		tunn.geneve_port.port = udp_port;

		rc = qede_tunnel_update(qdev, &tunn);
		if (rc != ECORE_SUCCESS) {
			DP_ERR(edev, "Unable to config UDP port %u\n",
			       tunn.vxlan_port.port);
			return rc;
		}

		qdev->vxlan.udp_port = udp_port;
		/* If the request is to delete UDP port and if the number of
		 * GENEVE filters have reached 0 then GENEVE offload can be be
		 * disabled.
		 */
		if (qdev->geneve.enable && qdev->geneve.num_filters == 0)
			return qede_geneve_enable(eth_dev,
					ECORE_TUNN_CLSS_MAC_VLAN, false);

		break;

	default:
		return ECORE_INVAL;
	}

	return 0;
}

int
qede_udp_dst_port_add(struct rte_eth_dev *eth_dev,
		      struct rte_eth_udp_tunnel *tunnel_udp)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	struct ecore_tunnel_info tunn; /* @DPDK */
	uint16_t udp_port;
	int rc;

	PMD_INIT_FUNC_TRACE(edev);

	memset(&tunn, 0, sizeof(tunn));

	switch (tunnel_udp->prot_type) {
	case RTE_TUNNEL_TYPE_VXLAN:
		if (qdev->vxlan.udp_port == tunnel_udp->udp_port) {
			DP_INFO(edev,
				"UDP port %u for VXLAN was already configured\n",
				tunnel_udp->udp_port);
			return ECORE_SUCCESS;
		}

		/* Enable VxLAN tunnel with default MAC/VLAN classification if
		 * it was not enabled while adding VXLAN filter before UDP port
		 * update.
		 */
		if (!qdev->vxlan.enable) {
			rc = qede_vxlan_enable(eth_dev,
				ECORE_TUNN_CLSS_MAC_VLAN, true);
			if (rc != ECORE_SUCCESS) {
				DP_ERR(edev, "Failed to enable VXLAN "
					"prior to updating UDP port\n");
				return rc;
			}
		}
		udp_port = tunnel_udp->udp_port;

		tunn.vxlan_port.b_update_port = true;
		tunn.vxlan_port.port = udp_port;

		rc = qede_tunnel_update(qdev, &tunn);
		if (rc != ECORE_SUCCESS) {
			DP_ERR(edev, "Unable to config UDP port %u for VXLAN\n",
			       udp_port);
			return rc;
		}

		DP_INFO(edev, "Updated UDP port %u for VXLAN\n", udp_port);

		qdev->vxlan.udp_port = udp_port;
		break;
	case RTE_TUNNEL_TYPE_GENEVE:
		if (qdev->geneve.udp_port == tunnel_udp->udp_port) {
			DP_INFO(edev,
				"UDP port %u for GENEVE was already configured\n",
				tunnel_udp->udp_port);
			return ECORE_SUCCESS;
		}

		/* Enable GENEVE tunnel with default MAC/VLAN classification if
		 * it was not enabled while adding GENEVE filter before UDP port
		 * update.
		 */
		if (!qdev->geneve.enable) {
			rc = qede_geneve_enable(eth_dev,
				ECORE_TUNN_CLSS_MAC_VLAN, true);
			if (rc != ECORE_SUCCESS) {
				DP_ERR(edev, "Failed to enable GENEVE "
					"prior to updating UDP port\n");
				return rc;
			}
		}
		udp_port = tunnel_udp->udp_port;

		tunn.geneve_port.b_update_port = true;
		tunn.geneve_port.port = udp_port;

		rc = qede_tunnel_update(qdev, &tunn);
		if (rc != ECORE_SUCCESS) {
			DP_ERR(edev, "Unable to config UDP port %u for GENEVE\n",
			       udp_port);
			return rc;
		}

		DP_INFO(edev, "Updated UDP port %u for GENEVE\n", udp_port);

		qdev->geneve.udp_port = udp_port;
		break;
	default:
		return ECORE_INVAL;
	}

	return 0;
}

static void qede_get_ecore_tunn_params(uint32_t filter, uint32_t *type,
				       uint32_t *clss, char *str)
{
	uint16_t j;
	*clss = MAX_ECORE_TUNN_CLSS;

	for (j = 0; j < RTE_DIM(qede_tunn_types); j++) {
		if (filter == qede_tunn_types[j].rte_filter_type) {
			*type = qede_tunn_types[j].qede_type;
			*clss = qede_tunn_types[j].qede_tunn_clss;
			strcpy(str, qede_tunn_types[j].string);
			return;
		}
	}
}

static int
qede_set_ucast_tunn_cmn_param(struct ecore_filter_ucast *ucast,
			      const struct rte_eth_tunnel_filter_conf *conf,
			      uint32_t type)
{
	/* Init commmon ucast params first */
	qede_set_ucast_cmn_params(ucast);

	/* Copy out the required fields based on classification type */
	ucast->type = type;

	switch (type) {
	case ECORE_FILTER_VNI:
		ucast->vni = conf->tenant_id;
	break;
	case ECORE_FILTER_INNER_VLAN:
		ucast->vlan = conf->inner_vlan;
	break;
	case ECORE_FILTER_MAC:
		memcpy(ucast->mac, conf->outer_mac.addr_bytes,
		       ETHER_ADDR_LEN);
	break;
	case ECORE_FILTER_INNER_MAC:
		memcpy(ucast->mac, conf->inner_mac.addr_bytes,
		       ETHER_ADDR_LEN);
	break;
	case ECORE_FILTER_MAC_VNI_PAIR:
		memcpy(ucast->mac, conf->outer_mac.addr_bytes,
			ETHER_ADDR_LEN);
		ucast->vni = conf->tenant_id;
	break;
	case ECORE_FILTER_INNER_MAC_VNI_PAIR:
		memcpy(ucast->mac, conf->inner_mac.addr_bytes,
			ETHER_ADDR_LEN);
		ucast->vni = conf->tenant_id;
	break;
	case ECORE_FILTER_INNER_PAIR:
		memcpy(ucast->mac, conf->inner_mac.addr_bytes,
			ETHER_ADDR_LEN);
		ucast->vlan = conf->inner_vlan;
	break;
	default:
		return -EINVAL;
	}

	return ECORE_SUCCESS;
}

static int
_qede_tunn_filter_config(struct rte_eth_dev *eth_dev,
			 const struct rte_eth_tunnel_filter_conf *conf,
			 __attribute__((unused)) enum rte_filter_op filter_op,
			 enum ecore_tunn_clss *clss,
			 bool add)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	struct ecore_filter_ucast ucast = {0};
	enum ecore_filter_ucast_type type;
	uint16_t filter_type = 0;
	char str[80];
	int rc;

	filter_type = conf->filter_type;
	/* Determine if the given filter classification is supported */
	qede_get_ecore_tunn_params(filter_type, &type, clss, str);
	if (*clss == MAX_ECORE_TUNN_CLSS) {
		DP_ERR(edev, "Unsupported filter type\n");
		return -EINVAL;
	}
	/* Init tunnel ucast params */
	rc = qede_set_ucast_tunn_cmn_param(&ucast, conf, type);
	if (rc != ECORE_SUCCESS) {
		DP_ERR(edev, "Unsupported Tunnel filter type 0x%x\n",
		conf->filter_type);
		return rc;
	}
	DP_INFO(edev, "Rule: \"%s\", op %d, type 0x%x\n",
		str, filter_op, ucast.type);

	ucast.opcode = add ? ECORE_FILTER_ADD : ECORE_FILTER_REMOVE;

	/* Skip MAC/VLAN if filter is based on VNI */
	if (!(filter_type & ETH_TUNNEL_FILTER_TENID)) {
		rc = qede_mac_int_ops(eth_dev, &ucast, add);
		if (rc == 0 && add) {
			/* Enable accept anyvlan */
			qede_config_accept_any_vlan(qdev, true);
		}
	} else {
		rc = qede_ucast_filter(eth_dev, &ucast, add);
		if (rc == 0)
			rc = ecore_filter_ucast_cmd(edev, &ucast,
					    ECORE_SPQ_MODE_CB, NULL);
	}

	return rc;
}

static int
qede_tunn_enable(struct rte_eth_dev *eth_dev, uint8_t clss,
		 enum rte_eth_tunnel_type tunn_type, bool enable)
{
	int rc = -EINVAL;

	switch (tunn_type) {
	case RTE_TUNNEL_TYPE_VXLAN:
		rc = qede_vxlan_enable(eth_dev, clss, enable);
		break;
	case RTE_TUNNEL_TYPE_GENEVE:
		rc = qede_geneve_enable(eth_dev, clss, enable);
		break;
	case RTE_TUNNEL_TYPE_IP_IN_GRE:
		rc = qede_ipgre_enable(eth_dev, clss, enable);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int
qede_tunn_filter_config(struct rte_eth_dev *eth_dev,
			enum rte_filter_op filter_op,
			const struct rte_eth_tunnel_filter_conf *conf)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	enum ecore_tunn_clss clss = MAX_ECORE_TUNN_CLSS;
	bool add;
	int rc;

	PMD_INIT_FUNC_TRACE(edev);

	switch (filter_op) {
	case RTE_ETH_FILTER_ADD:
		add = true;
		break;
	case RTE_ETH_FILTER_DELETE:
		add = false;
		break;
	default:
		DP_ERR(edev, "Unsupported operation %d\n", filter_op);
		return -EINVAL;
	}

	if (IS_VF(edev))
		return qede_tunn_enable(eth_dev,
					ECORE_TUNN_CLSS_MAC_VLAN,
					conf->tunnel_type, add);

	rc = _qede_tunn_filter_config(eth_dev, conf, filter_op, &clss, add);
	if (rc != ECORE_SUCCESS)
		return rc;

	if (add) {
		if (conf->tunnel_type == RTE_TUNNEL_TYPE_VXLAN) {
			qdev->vxlan.num_filters++;
			qdev->vxlan.filter_type = conf->filter_type;
		} else { /* GENEVE */
			qdev->geneve.num_filters++;
			qdev->geneve.filter_type = conf->filter_type;
		}

		if (!qdev->vxlan.enable || !qdev->geneve.enable ||
		    !qdev->ipgre.enable)
			return qede_tunn_enable(eth_dev, clss,
						conf->tunnel_type,
						true);
	} else {
		if (conf->tunnel_type == RTE_TUNNEL_TYPE_VXLAN)
			qdev->vxlan.num_filters--;
		else /*GENEVE*/
			qdev->geneve.num_filters--;

		/* Disable VXLAN if VXLAN filters become 0 */
		if (qdev->vxlan.num_filters == 0 ||
		    qdev->geneve.num_filters == 0)
			return qede_tunn_enable(eth_dev, clss,
						conf->tunnel_type,
						false);
	}

	return 0;
}

int qede_dev_filter_ctrl(struct rte_eth_dev *eth_dev,
			 enum rte_filter_type filter_type,
			 enum rte_filter_op filter_op,
			 void *arg)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct ecore_dev *edev = QEDE_INIT_EDEV(qdev);
	struct rte_eth_tunnel_filter_conf *filter_conf =
			(struct rte_eth_tunnel_filter_conf *)arg;

	switch (filter_type) {
	case RTE_ETH_FILTER_TUNNEL:
		switch (filter_conf->tunnel_type) {
		case RTE_TUNNEL_TYPE_VXLAN:
		case RTE_TUNNEL_TYPE_GENEVE:
		case RTE_TUNNEL_TYPE_IP_IN_GRE:
			DP_INFO(edev,
				"Packet steering to the specified Rx queue"
				" is not supported with UDP tunneling");
			return(qede_tunn_filter_config(eth_dev, filter_op,
						      filter_conf));
		case RTE_TUNNEL_TYPE_TEREDO:
		case RTE_TUNNEL_TYPE_NVGRE:
		case RTE_L2_TUNNEL_TYPE_E_TAG:
			DP_ERR(edev, "Unsupported tunnel type %d\n",
				filter_conf->tunnel_type);
			return -EINVAL;
		case RTE_TUNNEL_TYPE_NONE:
		default:
			return 0;
		}
		break;
	case RTE_ETH_FILTER_FDIR:
		return qede_fdir_filter_conf(eth_dev, filter_op, arg);
	case RTE_ETH_FILTER_NTUPLE:
		return qede_ntuple_filter_conf(eth_dev, filter_op, arg);
	case RTE_ETH_FILTER_MACVLAN:
	case RTE_ETH_FILTER_ETHERTYPE:
	case RTE_ETH_FILTER_FLEXIBLE:
	case RTE_ETH_FILTER_SYN:
	case RTE_ETH_FILTER_HASH:
	case RTE_ETH_FILTER_L2_TUNNEL:
	case RTE_ETH_FILTER_MAX:
	default:
		DP_ERR(edev, "Unsupported filter type %d\n",
			filter_type);
		return -EINVAL;
	}

	return 0;
}

/* RTE_FLOW */