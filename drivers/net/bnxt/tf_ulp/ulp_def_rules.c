/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019-2020 Broadcom
 * All rights reserved.
 */

#include "bnxt_tf_common.h"
#include "ulp_template_struct.h"
#include "ulp_template_db_enum.h"
#include "ulp_template_db_field.h"
#include "ulp_utils.h"
#include "ulp_port_db.h"
#include "ulp_flow_db.h"
#include "ulp_mapper.h"

#define BNXT_ULP_FREE_PARIF_BASE 11

struct bnxt_ulp_def_param_handler {
	int32_t (*vfr_func)(struct bnxt_ulp_context *ulp_ctx,
			    struct ulp_tlv_param *param,
			    struct bnxt_ulp_mapper_create_parms *mapper_params);
};

static int32_t
ulp_set_svif_in_comp_fld(struct bnxt_ulp_context *ulp_ctx,
			 uint32_t  ifindex, uint8_t svif_type,
			 struct bnxt_ulp_mapper_create_parms *mapper_params)
{
	uint16_t svif;
	uint8_t idx;
	int rc;

	rc = ulp_port_db_svif_get(ulp_ctx, ifindex, svif_type, &svif);
	if (rc)
		return rc;

	if (svif_type == BNXT_ULP_PHY_PORT_SVIF)
		idx = BNXT_ULP_CF_IDX_PHY_PORT_SVIF;
	else if (svif_type == BNXT_ULP_DRV_FUNC_SVIF)
		idx = BNXT_ULP_CF_IDX_DRV_FUNC_SVIF;
	else
		idx = BNXT_ULP_CF_IDX_VF_FUNC_SVIF;

	ULP_COMP_FLD_IDX_WR(mapper_params, idx, svif);

	return 0;
}

static int32_t
ulp_set_spif_in_comp_fld(struct bnxt_ulp_context *ulp_ctx,
			 uint32_t  ifindex, uint8_t spif_type,
			 struct bnxt_ulp_mapper_create_parms *mapper_params)
{
	uint16_t spif;
	uint8_t idx;
	int rc;

	rc = ulp_port_db_spif_get(ulp_ctx, ifindex, spif_type, &spif);
	if (rc)
		return rc;

	if (spif_type == BNXT_ULP_PHY_PORT_SPIF)
		idx = BNXT_ULP_CF_IDX_PHY_PORT_SPIF;
	else if (spif_type == BNXT_ULP_DRV_FUNC_SPIF)
		idx = BNXT_ULP_CF_IDX_DRV_FUNC_SPIF;
	else
		idx = BNXT_ULP_CF_IDX_VF_FUNC_SPIF;

	ULP_COMP_FLD_IDX_WR(mapper_params, idx, spif);

	return 0;
}

static int32_t
ulp_set_parif_in_comp_fld(struct bnxt_ulp_context *ulp_ctx,
			  uint32_t  ifindex, uint8_t parif_type,
			  struct bnxt_ulp_mapper_create_parms *mapper_params)
{
	uint16_t parif;
	uint8_t idx;
	int rc;

	rc = ulp_port_db_parif_get(ulp_ctx, ifindex, parif_type, &parif);
	if (rc)
		return rc;

	if (parif_type == BNXT_ULP_PHY_PORT_PARIF) {
		idx = BNXT_ULP_CF_IDX_PHY_PORT_PARIF;
	} else if (parif_type == BNXT_ULP_DRV_FUNC_PARIF) {
		idx = BNXT_ULP_CF_IDX_DRV_FUNC_PARIF;
		/* Parif needs to be reset to a free partition */
		parif += BNXT_ULP_FREE_PARIF_BASE;
	} else {
		idx = BNXT_ULP_CF_IDX_VF_FUNC_PARIF;
	}

	ULP_COMP_FLD_IDX_WR(mapper_params, idx, parif);

	return 0;
}

static int32_t
ulp_set_vport_in_comp_fld(struct bnxt_ulp_context *ulp_ctx, uint32_t ifindex,
			  struct bnxt_ulp_mapper_create_parms *mapper_params)
{
	uint16_t vport;
	int rc;

	rc = ulp_port_db_vport_get(ulp_ctx, ifindex, &vport);
	if (rc)
		return rc;

	ULP_COMP_FLD_IDX_WR(mapper_params, BNXT_ULP_CF_IDX_PHY_PORT_VPORT,
			    vport);
	return 0;
}

static int32_t
ulp_set_vnic_in_comp_fld(struct bnxt_ulp_context *ulp_ctx,
			 uint32_t  ifindex, uint8_t vnic_type,
			 struct bnxt_ulp_mapper_create_parms *mapper_params)
{
	uint16_t vnic;
	uint8_t idx;
	int rc;

	rc = ulp_port_db_default_vnic_get(ulp_ctx, ifindex, vnic_type, &vnic);
	if (rc)
		return rc;

	if (vnic_type == BNXT_ULP_DRV_FUNC_VNIC)
		idx = BNXT_ULP_CF_IDX_DRV_FUNC_VNIC;
	else
		idx = BNXT_ULP_CF_IDX_VF_FUNC_VNIC;

	ULP_COMP_FLD_IDX_WR(mapper_params, idx, vnic);

	return 0;
}

static int32_t
ulp_set_vlan_in_act_prop(uint16_t port_id,
			 struct bnxt_ulp_mapper_create_parms *mapper_params)
{
	struct ulp_rte_act_prop *act_prop = mapper_params->act_prop;

	if (ULP_BITMAP_ISSET(mapper_params->act->bits,
			     BNXT_ULP_ACTION_BIT_SET_VLAN_VID)) {
		BNXT_TF_DBG(ERR,
			    "VLAN already set, multiple VLANs unsupported\n");
		return BNXT_TF_RC_ERROR;
	}

	port_id = rte_cpu_to_be_16(port_id);

	ULP_BITMAP_SET(mapper_params->act->bits,
		       BNXT_ULP_ACTION_BIT_SET_VLAN_VID);

	memcpy(&act_prop->act_details[BNXT_ULP_ACT_PROP_IDX_ENCAP_VTAG],
	       &port_id, sizeof(port_id));

	return 0;
}

static int32_t
ulp_set_mark_in_act_prop(uint16_t port_id,
			 struct bnxt_ulp_mapper_create_parms *mapper_params)
{
	if (ULP_BITMAP_ISSET(mapper_params->act->bits,
			     BNXT_ULP_ACTION_BIT_MARK)) {
		BNXT_TF_DBG(ERR,
			    "MARK already set, multiple MARKs unsupported\n");
		return BNXT_TF_RC_ERROR;
	}

	ULP_COMP_FLD_IDX_WR(mapper_params, BNXT_ULP_CF_IDX_DEV_PORT_ID,
			    port_id);

	return 0;
}

static int32_t
ulp_df_dev_port_handler(struct bnxt_ulp_context *ulp_ctx,
			struct ulp_tlv_param *param,
			struct bnxt_ulp_mapper_create_parms *mapper_params)
{
	uint16_t port_id;
	uint32_t ifindex;
	int rc;

	port_id = param->value[0] | param->value[1];

	rc = ulp_port_db_dev_port_to_ulp_index(ulp_ctx, port_id, &ifindex);
	if (rc) {
		BNXT_TF_DBG(ERR,
				"Invalid port id\n");
		return BNXT_TF_RC_ERROR;
	}

	/* Set port SVIF */
	rc = ulp_set_svif_in_comp_fld(ulp_ctx, ifindex, BNXT_ULP_PHY_PORT_SVIF,
				      mapper_params);
	if (rc)
		return rc;

	/* Set DRV Func SVIF */
	rc = ulp_set_svif_in_comp_fld(ulp_ctx, ifindex, BNXT_ULP_DRV_FUNC_SVIF,
				      mapper_params);
	if (rc)
		return rc;

	/* Set VF Func SVIF */
	rc = ulp_set_svif_in_comp_fld(ulp_ctx, ifindex, BNXT_ULP_VF_FUNC_SVIF,
				      mapper_params);
	if (rc)
		return rc;

	/* Set port SPIF */
	rc = ulp_set_spif_in_comp_fld(ulp_ctx, ifindex, BNXT_ULP_PHY_PORT_SPIF,
				      mapper_params);
	if (rc)
		return rc;

	/* Set DRV Func SPIF */
	rc = ulp_set_spif_in_comp_fld(ulp_ctx, ifindex, BNXT_ULP_DRV_FUNC_SPIF,
				      mapper_params);
	if (rc)
		return rc;

	/* Set VF Func SPIF */
	rc = ulp_set_spif_in_comp_fld(ulp_ctx, ifindex, BNXT_ULP_DRV_FUNC_SPIF,
				      mapper_params);
	if (rc)
		return rc;

	/* Set port PARIF */
	rc = ulp_set_parif_in_comp_fld(ulp_ctx, ifindex,
				       BNXT_ULP_PHY_PORT_PARIF, mapper_params);
	if (rc)
		return rc;

	/* Set DRV Func PARIF */
	rc = ulp_set_parif_in_comp_fld(ulp_ctx, ifindex,
				       BNXT_ULP_DRV_FUNC_PARIF, mapper_params);
	if (rc)
		return rc;

	/* Set VF Func PARIF */
	rc = ulp_set_parif_in_comp_fld(ulp_ctx, ifindex, BNXT_ULP_VF_FUNC_PARIF,
				       mapper_params);
	if (rc)
		return rc;

	/* Set uplink VNIC */
	rc = ulp_set_vnic_in_comp_fld(ulp_ctx, ifindex, true, mapper_params);
	if (rc)
		return rc;

	/* Set VF VNIC */
	rc = ulp_set_vnic_in_comp_fld(ulp_ctx, ifindex, false, mapper_params);
	if (rc)
		return rc;

	/* Set VPORT */
	rc = ulp_set_vport_in_comp_fld(ulp_ctx, ifindex, mapper_params);
	if (rc)
		return rc;

	/* Set VLAN */
	rc = ulp_set_vlan_in_act_prop(port_id, mapper_params);
	if (rc)
		return rc;

	/* Set MARK */
	rc = ulp_set_mark_in_act_prop(port_id, mapper_params);
	if (rc)
		return rc;

	return 0;
}

struct bnxt_ulp_def_param_handler ulp_def_handler_tbl[] = {
	[BNXT_ULP_DF_PARAM_TYPE_DEV_PORT_ID] = {
			.vfr_func = ulp_df_dev_port_handler }
};

/*
 * Function to create default rules for the following paths
 * 1) Device PORT to DPDK App
 * 2) DPDK App to Device PORT
 * 3) VF Representor to VF
 * 4) VF to VF Representor
 *
 * eth_dev [in] Ptr to rte eth device.
 * param_list [in] Ptr to a list of parameters (Currently, only DPDK port_id).
 * ulp_class_tid [in] Class template ID number.
 * flow_id [out] Ptr to flow identifier.
 *
 * Returns 0 on success or negative number on failure.
 */
int32_t
ulp_default_flow_create(struct rte_eth_dev *eth_dev,
			struct ulp_tlv_param *param_list,
			uint32_t ulp_class_tid,
			uint32_t *flow_id)
{
	struct ulp_rte_hdr_field	hdr_field[BNXT_ULP_PROTO_HDR_MAX];
	uint32_t			comp_fld[BNXT_ULP_CF_IDX_LAST];
	struct bnxt_ulp_mapper_create_parms mapper_params = { 0 };
	struct ulp_rte_act_prop		act_prop;
	struct ulp_rte_act_bitmap	act = { 0 };
	struct bnxt_ulp_context		*ulp_ctx;
	uint32_t type;
	int rc;

	memset(&mapper_params, 0, sizeof(mapper_params));
	memset(hdr_field, 0, sizeof(hdr_field));
	memset(comp_fld, 0, sizeof(comp_fld));
	memset(&act_prop, 0, sizeof(act_prop));

	mapper_params.hdr_field = hdr_field;
	mapper_params.act = &act;
	mapper_params.act_prop = &act_prop;
	mapper_params.comp_fld = comp_fld;

	ulp_ctx = bnxt_ulp_eth_dev_ptr2_cntxt_get(eth_dev);
	if (!ulp_ctx) {
		BNXT_TF_DBG(ERR, "ULP context is not initialized. "
				 "Failed to create default flow.\n");
		return -EINVAL;
	}

	type = param_list->type;
	while (type != BNXT_ULP_DF_PARAM_TYPE_LAST) {
		if (ulp_def_handler_tbl[type].vfr_func) {
			rc = ulp_def_handler_tbl[type].vfr_func(ulp_ctx,
								param_list,
								&mapper_params);
			if (rc) {
				BNXT_TF_DBG(ERR,
					    "Failed to create default flow.\n");
				return rc;
			}
		}

		param_list++;
		type = param_list->type;
	}

	mapper_params.class_tid = ulp_class_tid;

	rc = ulp_mapper_flow_create(ulp_ctx, &mapper_params, flow_id);
	if (rc) {
		BNXT_TF_DBG(ERR, "Failed to create default flow.\n");
		return rc;
	}

	return 0;
}

/*
 * Function to destroy default rules for the following paths
 * 1) Device PORT to DPDK App
 * 2) DPDK App to Device PORT
 * 3) VF Representor to VF
 * 4) VF to VF Representor
 *
 * eth_dev [in] Ptr to rte eth device.
 * flow_id [in] Flow identifier.
 *
 * Returns 0 on success or negative number on failure.
 */
int32_t
ulp_default_flow_destroy(struct rte_eth_dev *eth_dev, uint32_t flow_id)
{
	struct bnxt_ulp_context *ulp_ctx;
	int rc;

	ulp_ctx = bnxt_ulp_eth_dev_ptr2_cntxt_get(eth_dev);
	if (!ulp_ctx) {
		BNXT_TF_DBG(ERR, "ULP context is not initialized\n");
		return -EINVAL;
	}

	rc = ulp_mapper_flow_destroy(ulp_ctx, flow_id,
				     BNXT_ULP_DEFAULT_FLOW_TABLE);
	if (rc)
		BNXT_TF_DBG(ERR, "Failed to destroy flow.\n");

	return rc;
}
