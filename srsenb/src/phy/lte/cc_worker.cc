/**
 * Copyright 2013-2021 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */
#include <iomanip>

#include "srsran/common/threads.h"
#include "srsran/srsran.h"

#include "srsenb/hdr/phy/lte/cc_worker.h"

#define Error(fmt, ...)                                                                                                \
  if (SRSRAN_DEBUG_ENABLED)                                                                                            \
  logger.error(fmt, ##__VA_ARGS__)
#define Warning(fmt, ...)                                                                                              \
  if (SRSRAN_DEBUG_ENABLED)                                                                                            \
  logger.warning(fmt, ##__VA_ARGS__)
#define Info(fmt, ...)                                                                                                 \
  if (SRSRAN_DEBUG_ENABLED)                                                                                            \
  logger.info(fmt, ##__VA_ARGS__)
#define Debug(fmt, ...)                                                                                                \
  if (SRSRAN_DEBUG_ENABLED)                                                                                            \
  logger.debug(fmt, ##__VA_ARGS__)

using namespace std;

// Enable this to log SI
//#define LOG_THIS(a) 1

// Enable this one to skip SI-RNTI
#define LOG_THIS(rnti) (rnti != 0xFFFF)

/*********************************************/

using namespace asn1::rrc;

//#define DEBUG_WRITE_FILE

namespace srsenb {
namespace lte {

cc_worker::cc_worker(srslog::basic_logger& logger) : logger(logger)
{
  reset();
}

cc_worker::~cc_worker()
{
  srsran_softbuffer_tx_free(&temp_mbsfn_softbuffer);
  srsran_enb_dl_free(&enb_dl);
  srsran_enb_ul_free(&enb_ul);

  for (int p = 0; p < SRSRAN_MAX_PORTS; p++) {
    if (signal_buffer_rx[p]) {
      free(signal_buffer_rx[p]);
    }
    if (signal_buffer_tx[p]) {
      free(signal_buffer_tx[p]);
    }
  }

  // Delete all users
  for (auto& it : ue_db) {
    delete it.second;
  }
}

#ifdef DEBUG_WRITE_FILE
FILE* f;
#endif

void cc_worker::init(phy_common* phy_, uint32_t cc_idx_)
{
  phy                   = phy_;
  cc_idx                = cc_idx_;
  srsran_cell_t cell    = phy_->get_cell(cc_idx);
  uint32_t      nof_prb = phy_->get_nof_prb(cc_idx);
  uint32_t      sf_len  = SRSRAN_SF_LEN_PRB(nof_prb);

  // Init cell here
  for (uint32_t p = 0; p < phy->get_nof_ports(cc_idx); p++) {
    signal_buffer_rx[p] = srsran_vec_cf_malloc(2 * sf_len);
    if (!signal_buffer_rx[p]) {
      ERROR("Error allocating memory");
      return;
    }
    srsran_vec_cf_zero(signal_buffer_rx[p], 2 * sf_len);
    signal_buffer_tx[p] = srsran_vec_cf_malloc(2 * sf_len);
    if (!signal_buffer_tx[p]) {
      ERROR("Error allocating memory");
      return;
    }
    srsran_vec_cf_zero(signal_buffer_tx[p], 2 * sf_len);
  }
  if (srsran_enb_dl_init(&enb_dl, signal_buffer_tx, nof_prb)) {
    ERROR("Error initiating ENB DL (cc=%d)", cc_idx);
    return;
  }
  if (srsran_enb_dl_set_cell(&enb_dl, cell)) {
    ERROR("Error initiating ENB DL (cc=%d)", cc_idx);
    return;
  }
  if (srsran_enb_ul_init(&enb_ul, signal_buffer_rx[0], nof_prb)) {
    ERROR("Error initiating ENB UL");
    return;
  }

  if (srsran_enb_ul_set_cell(&enb_ul, cell, &phy->dmrs_pusch_cfg, nullptr)) {
    ERROR("Error initiating ENB UL");
    return;
  }

  /* Setup SI-RNTI in PHY */
  add_rnti(SRSRAN_SIRNTI);

  /* Setup P-RNTI in PHY */
  add_rnti(SRSRAN_PRNTI);

  /* Setup RA-RNTI in PHY */
  for (int i = SRSRAN_RARNTI_START; i <= SRSRAN_RARNTI_END; i++) {
    add_rnti(i);
  }

  if (srsran_softbuffer_tx_init(&temp_mbsfn_softbuffer, nof_prb)) {
    ERROR("Error initiating soft buffer");
    exit(-1);
  }

  srsran_softbuffer_tx_reset(&temp_mbsfn_softbuffer);

  Info("Component Carrier Worker %d configured cell %d PRB", cc_idx, nof_prb);

  if (phy->params.pusch_8bit_decoder) {
    enb_ul.pusch.llr_is_8bit        = true;
    enb_ul.pusch.ul_sch.llr_is_8bit = true;
  }
  initiated = true;

#ifdef DEBUG_WRITE_FILE
  f = fopen("test.dat", "w");
#endif
}

void cc_worker::reset()
{
  initiated = false;
  ue_db.clear();
}

cf_t* cc_worker::get_buffer_rx(uint32_t antenna_idx)
{
  return signal_buffer_rx[antenna_idx];
}

cf_t* cc_worker::get_buffer_tx(uint32_t antenna_idx)
{
  return signal_buffer_tx[antenna_idx];
}

void cc_worker::set_tti(uint32_t tti_)
{
  tti_rx    = tti_;
  tti_tx_dl = TTI_ADD(tti_rx, FDD_HARQ_DELAY_UL_MS);
  tti_tx_ul = TTI_RX_ACK(tti_rx);
}

int cc_worker::add_rnti(uint16_t rnti)
{
  std::unique_lock<std::mutex> lock(mutex);

  // Create user unless already exists
  if (ue_db.count(rnti) == 0) {
    ue_db[rnti] = new ue(rnti);
  }
  return SRSRAN_SUCCESS;
}

void cc_worker::rem_rnti(uint16_t rnti)
{
  std::lock_guard<std::mutex> lock(mutex);
  if (ue_db.count(rnti)) {
    delete ue_db[rnti];
    ue_db.erase(rnti);
  } else {
    Error("Removing user: rnti=0x%x does not exist\n", rnti);
  }
}

uint32_t cc_worker::get_nof_rnti()
{
  std::lock_guard<std::mutex> lock(mutex);
  return ue_db.size();
}

void cc_worker::work_ul(const srsran_ul_sf_cfg_t& ul_sf_cfg, stack_interface_phy_lte::ul_sched_t& ul_grants)
{
  std::lock_guard<std::mutex> lock(mutex);
  ul_sf = ul_sf_cfg;
  logger.set_context(ul_sf.tti);

  // Process UL signal
  srsran_enb_ul_fft(&enb_ul);

  // Decode pending UL grants for the tti they were scheduled
  decode_pusch(ul_grants.pusch, ul_grants.nof_grants);

  // Decode remaining PUCCH ACKs not associated with PUSCH transmission and SR signals
  decode_pucch();
}

void cc_worker::work_dl(const srsran_dl_sf_cfg_t&            dl_sf_cfg,
                        stack_interface_phy_lte::dl_sched_t& dl_grants,
                        stack_interface_phy_lte::ul_sched_t& ul_grants,
                        srsran_mbsfn_cfg_t*                  mbsfn_cfg)
{
  std::lock_guard<std::mutex> lock(mutex);
  dl_sf = dl_sf_cfg;

  // Put base signals (references, PBCH, PCFICH and PSS/SSS) into the resource grid
  srsran_enb_dl_put_base(&enb_dl, &dl_sf);

  // Put DL grants to resource grid. PDSCH data will be encoded as well.
  if (dl_sf_cfg.sf_type == SRSRAN_SF_NORM) {
    encode_pdcch_dl(dl_grants.pdsch, dl_grants.nof_grants);
    encode_pdsch(dl_grants.pdsch, dl_grants.nof_grants);
  } else {
    if (mbsfn_cfg->enable) {
      encode_pmch(dl_grants.pdsch, mbsfn_cfg);
    }
  }

  // Put UL grants to resource grid.
  encode_pdcch_ul(ul_grants.pusch, ul_grants.nof_grants);

  // Put pending PHICH HARQ ACK/NACK indications into subframe
  encode_phich(ul_grants.phich, ul_grants.nof_phich);

  // Generate signal and transmit
  srsran_enb_dl_gen_signal(&enb_dl);

  // Scale if cell gain is set
  float cell_gain_db = phy->get_cell_gain(cc_idx);
  if (std::isnormal(cell_gain_db)) {
    float    scale  = srsran_convert_dB_to_amplitude(cell_gain_db);
    uint32_t sf_len = SRSRAN_SF_LEN_PRB(enb_dl.cell.nof_prb);
    for (uint32_t i = 0; i < enb_dl.cell.nof_ports; i++) {
      srsran_vec_sc_prod_cfc(signal_buffer_tx[i], scale, signal_buffer_tx[i], sf_len);
    }
  }

  // Measure if flag was triggered
  bool cell_meas_flag = phy->get_cell_measure_trigger(cc_idx);
  if (cell_meas_flag == true) {
    uint32_t sf_len = SRSRAN_SF_LEN_PRB(enb_dl.cell.nof_prb);
    for (uint32_t i = 0; i < enb_dl.cell.nof_ports; i++) {
      // PAPR measure
      std::vector<float> sigSq(sf_len);
      srsran_vec_abs_square_cf(signal_buffer_tx[i], sigSq.data(), sf_len);
      float sigPMax  = sigSq[srsran_vec_max_abs_fi(sigSq.data(), sf_len)];
      float sigPMean = srsran_vec_acc_ff(sigSq.data(), sf_len) / float(sf_len);
      float PAPRdB   = 10.0f * log10(sigPMax / sigPMean);
      std::cout << "Cell #" << cc_idx << " port #" << i << " PAPR = " << std::setprecision(4) << PAPRdB << " dB "
                << std::endl;
    }
    // clear measurement flag on cell
    phy->clear_cell_measure_trigger(cc_idx);
  }
}

void cc_worker::decode_pusch_rnti(stack_interface_phy_lte::ul_sched_grant_t& ul_grant,
                                  srsran_ul_cfg_t&                           ul_cfg,
                                  srsran_pusch_res_t&                        pusch_res)
{
  uint16_t rnti = ul_grant.dci.rnti;

  // Invalid RNTI
  if (rnti == SRSRAN_INVALID_RNTI) {
    return;
  }

  // RNTI does not exist
  if (ue_db.count(rnti) == 0) {
    return;
  }

  // Get UE configuration
  if (phy->ue_db.get_ul_config(rnti, cc_idx, ul_cfg) < SRSRAN_SUCCESS) {
    Error("Error retrieving UL configuration for RNTI %x and CC %d", rnti, cc_idx);
    return;
  }

  // Fill UCI configuration
  bool uci_required =
      phy->ue_db.fill_uci_cfg(tti_rx, cc_idx, rnti, ul_grant.dci.cqi_request, true, ul_cfg.pusch.uci_cfg);

  // Compute UL grant
  srsran_pusch_grant_t& grant = ul_cfg.pusch.grant;
  if (srsran_ra_ul_dci_to_grant(&enb_ul.cell, &ul_sf, &ul_cfg.hopping, &ul_grant.dci, &grant)) {
    Error("Computing PUSCH dci for RNTI %x", rnti);
    return;
  }

  // Handle Format0 adaptive retx
  // Use last TBS for this TB in case of mcs>28
  if (ul_grant.dci.tb.mcs_idx > 28) {
    int rv_idx = grant.tb.rv;
    if (phy->ue_db.get_last_ul_tb(rnti, cc_idx, ul_grant.pid, grant.tb) < SRSRAN_SUCCESS) {
      Error("Error retrieving last UL TB for RNTI %x, CC %d, PID %d", rnti, cc_idx, ul_grant.pid);
      return;
    }
    grant.tb.rv = rv_idx;
    Info("Adaptive retx: rnti=0x%x, pid=%d, rv_idx=%d, mcs=%d, old_tbs=%d",
         rnti,
         ul_grant.pid,
         grant.tb.rv,
         ul_grant.dci.tb.mcs_idx,
         grant.tb.tbs / 8);
  }

  if (phy->ue_db.set_last_ul_tb(rnti, cc_idx, ul_grant.pid, grant.tb) < SRSRAN_SUCCESS) {
    Error("Error setting last UL TB for RNTI %x, CC %d, PID %d", rnti, cc_idx, ul_grant.pid);
  }

  // Run PUSCH decoder
  ul_cfg.pusch.softbuffers.rx = ul_grant.softbuffer_rx;
  pusch_res.data              = ul_grant.data;
  if (pusch_res.data) {
    if (srsran_enb_ul_get_pusch(&enb_ul, &ul_sf, &ul_cfg.pusch, &pusch_res)) {
      Error("Decoding PUSCH for RNTI %x", rnti);
      return;
    }
  }
  // Save PHICH scheduling for this user. Each user can have just 1 PUSCH dci per TTI
  ue_db[rnti]->phich_grant.n_prb_lowest = grant.n_prb_tilde[0];
  ue_db[rnti]->phich_grant.n_dmrs       = ul_grant.dci.n_dmrs;

  float snr_db = enb_ul.chest_res.snr_db;

  // Notify MAC of RL status
  if (snr_db >= PUSCH_RL_SNR_DB_TH) {
    // Notify MAC UL channel quality
    phy->stack->snr_info(ul_sf.tti, rnti, cc_idx, snr_db, mac_interface_phy_lte::PUSCH);

    // Notify MAC of Time Alignment only if it enabled and valid measurement, ignore value otherwise
    if (ul_cfg.pusch.meas_ta_en and not std::isnan(enb_ul.chest_res.ta_us) and not std::isinf(enb_ul.chest_res.ta_us)) {
      phy->stack->ta_info(ul_sf.tti, rnti, enb_ul.chest_res.ta_us);
    }
  }

  // Send UCI data to MAC
  if (uci_required) {
    phy->ue_db.send_uci_data(tti_rx, rnti, cc_idx, ul_cfg.pusch.uci_cfg, pusch_res.uci);
  }

  // Save statistics only if data was provided
  if (ul_grant.data != nullptr) {
    // Save metrics stats
    ue_db[rnti]->metrics_ul(ul_grant.dci.tb.mcs_idx, 0, enb_ul.chest_res.snr_db, pusch_res.avg_iterations_block);
  }
}

void cc_worker::decode_pusch(stack_interface_phy_lte::ul_sched_grant_t* grants, uint32_t nof_pusch)
{
  // Iterate over all the grants, all the grants need to report MAC the CRC status
  for (uint32_t i = 0; i < nof_pusch; i++) {
    // Get grant itself and RNTI
    stack_interface_phy_lte::ul_sched_grant_t& ul_grant = grants[i];
    uint16_t                                   rnti     = ul_grant.dci.rnti;

    srsran_pusch_res_t pusch_res = {};
    srsran_ul_cfg_t    ul_cfg    = {};

    // Decodes PUSCH for the given grant
    decode_pusch_rnti(ul_grant, ul_cfg, pusch_res);

    // Notify MAC new received data and HARQ Indication value
    if (ul_grant.data != nullptr) {
      // Inform MAC about the CRC result
      phy->stack->crc_info(tti_rx, rnti, cc_idx, ul_cfg.pusch.grant.tb.tbs / 8, pusch_res.crc);
      // Push PDU buffer
      phy->stack->push_pdu(tti_rx, rnti, cc_idx, ul_cfg.pusch.grant.tb.tbs / 8, pusch_res.crc);
      // Logging
      if (logger.info.enabled()) {
        char str[512];
        srsran_pusch_rx_info(&ul_cfg.pusch, &pusch_res, &enb_ul.chest_res, str, sizeof(str));
        logger.info("PUSCH: cc=%d, %s", cc_idx, str);
      }
    }
  }
}

int cc_worker::decode_pucch()
{
  srsran_pucch_res_t pucch_res = {};

  for (auto& iter : ue_db) {
    uint16_t rnti = iter.first;

    // If it's a User RNTI and doesn't have PUSCH grant in this TTI
    if (SRSRAN_RNTI_ISUSER(rnti) and phy->ue_db.is_pcell(rnti, cc_idx)) {
      srsran_ul_cfg_t ul_cfg = {};

      if (phy->ue_db.get_ul_config(rnti, cc_idx, ul_cfg) < SRSRAN_SUCCESS) {
        Error("Error retrieving last UL configuration for RNTI %x, CC %d", rnti, cc_idx);
        continue;
      }

      // Check if user needs to receive PUCCH
      int ret = phy->ue_db.fill_uci_cfg(tti_rx, cc_idx, rnti, false, false, ul_cfg.pucch.uci_cfg);
      if (ret < SRSRAN_SUCCESS) {
        Error("Error retrieving UCI configuration for RNTI %x, CC %d", rnti, cc_idx);
        continue;
      }

      // If ret is more than success, UCI is present
      if (ret > SRSRAN_SUCCESS) {
        // Decode PUCCH
        if (srsran_enb_ul_get_pucch(&enb_ul, &ul_sf, &ul_cfg.pucch, &pucch_res)) {
          Error("Error getting PUCCH");
          continue;
        }

        // Send UCI data to MAC
        if (phy->ue_db.send_uci_data(tti_rx, rnti, cc_idx, ul_cfg.pucch.uci_cfg, pucch_res.uci_data) < SRSRAN_SUCCESS) {
          Error("Error sending UCI data for RNTI %x, CC %d", rnti, cc_idx);
          continue;
        }

        if (pucch_res.detected and pucch_res.ta_valid) {
          phy->stack->ta_info(tti_rx, rnti, pucch_res.ta_us);
          phy->stack->snr_info(tti_rx, rnti, cc_idx, pucch_res.snr_db, mac_interface_phy_lte::PUCCH);
        }

        // Logging
        if (logger.info.enabled()) {
          char str[512];
          srsran_pucch_rx_info(&ul_cfg.pucch, &pucch_res, str, sizeof(str));
          logger.info("PUCCH: cc=%d; %s", cc_idx, str);
        }

        // Save metrics
        ue_db[rnti]->metrics_ul_pucch(pucch_res.snr_db);
      }
    }
  }
  return 0;
}

int cc_worker::encode_phich(stack_interface_phy_lte::ul_sched_ack_t* acks, uint32_t nof_acks)
{
  for (uint32_t i = 0; i < nof_acks; i++) {
    if (acks[i].rnti && ue_db.count(acks[i].rnti)) {
      srsran_enb_dl_put_phich(&enb_dl, &ue_db[acks[i].rnti]->phich_grant, acks[i].ack);

      Info("PHICH: rnti=0x%x, hi=%d, I_lowest=%d, n_dmrs=%d, tti_tx_dl=%d",
           acks[i].rnti,
           acks[i].ack,
           ue_db[acks[i].rnti]->phich_grant.n_prb_lowest,
           ue_db[acks[i].rnti]->phich_grant.n_dmrs,
           tti_tx_dl);
    }
  }
  return SRSRAN_SUCCESS;
}

int cc_worker::encode_pdcch_ul(stack_interface_phy_lte::ul_sched_grant_t* grants, uint32_t nof_grants)
{
  for (uint32_t i = 0; i < nof_grants; i++) {
    if (grants[i].needs_pdcch) {
      srsran_dci_cfg_t dci_cfg = {};

      if (phy->ue_db.get_dci_ul_config(grants[i].dci.rnti, cc_idx, dci_cfg) < SRSRAN_SUCCESS) {
        Error("Error retrieving DCI UL configuration for RNTI %x, CC %d", grants[i].dci.rnti, cc_idx);
        continue;
      }

      if (SRSRAN_RNTI_ISUSER(grants[i].dci.rnti)) {
        if (srsran_enb_dl_location_is_common_ncce(&enb_dl, grants[i].dci.location.ncce) &&
            phy->ue_db.is_pcell(grants[i].dci.rnti, cc_idx)) {
          // Disable extended CSI request and SRS request in common SS
          srsran_dci_cfg_set_common_ss(&dci_cfg);
        }
      }

      if (srsran_enb_dl_put_pdcch_ul(&enb_dl, &dci_cfg, &grants[i].dci)) {
        Error("Error putting PUSCH %d", i);
        return SRSRAN_ERROR;
      }

      // Logging
      if (logger.info.enabled()) {
        char str[512];
        srsran_dci_ul_info(&grants[i].dci, str, 512);
        logger.info("PDCCH: cc=%d, %s, tti_tx_dl=%d", cc_idx, str, tti_tx_dl);
      }
    }
  }
  return SRSRAN_SUCCESS;
}

int cc_worker::encode_pdcch_dl(stack_interface_phy_lte::dl_sched_grant_t* grants, uint32_t nof_grants)
{
  for (uint32_t i = 0; i < nof_grants; i++) {
    uint16_t rnti = grants[i].dci.rnti;
    if (rnti) {
      srsran_dci_cfg_t dci_cfg = {};

      if (phy->ue_db.get_dci_dl_config(grants[i].dci.rnti, cc_idx, dci_cfg) < SRSRAN_SUCCESS) {
        Error("Error retrieving DCI DL configuration for RNTI %x, CC %d", grants[i].dci.rnti, cc_idx);
        continue;
      }

      if (SRSRAN_RNTI_ISUSER(grants[i].dci.rnti) && grants[i].dci.format == SRSRAN_DCI_FORMAT1A) {
        if (srsran_enb_dl_location_is_common_ncce(&enb_dl, grants[i].dci.location.ncce) &&
            phy->ue_db.is_pcell(grants[i].dci.rnti, cc_idx)) {
          srsran_dci_cfg_set_common_ss(&dci_cfg);
        }
      }

      if (srsran_enb_dl_put_pdcch_dl(&enb_dl, &dci_cfg, &grants[i].dci)) {
        ERROR("Error putting PDCCH %d", i);
        return SRSRAN_ERROR;
      }

      if (LOG_THIS(rnti) and logger.info.enabled()) {
        // Logging
        char str[512];
        srsran_dci_dl_info(&grants[i].dci, str, 512);
        logger.info("PDCCH: cc=%d, %s, tti_tx_dl=%d", cc_idx, str, tti_tx_dl);
      }
    }
  }
  return 0;
}

int cc_worker::encode_pmch(stack_interface_phy_lte::dl_sched_grant_t* grant, srsran_mbsfn_cfg_t* mbsfn_cfg)
{
  srsran_pmch_cfg_t pmch_cfg;
  ZERO_OBJECT(pmch_cfg);
  srsran_configure_pmch(&pmch_cfg, &enb_dl.cell, mbsfn_cfg);
  srsran_ra_dl_compute_nof_re(&enb_dl.cell, &dl_sf, &pmch_cfg.pdsch_cfg.grant);

  // Set soft buffer
  pmch_cfg.pdsch_cfg.softbuffers.tx[0] = &temp_mbsfn_softbuffer;

  // Encode PMCH
  if (srsran_enb_dl_put_pmch(&enb_dl, &pmch_cfg, grant->data[0])) {
    Error("Error putting PMCH");
    return SRSRAN_ERROR;
  }

  // Logging
  if (logger.info.enabled()) {
    char str[512];
    srsran_pdsch_tx_info(&pmch_cfg.pdsch_cfg, str, 512);
    logger.info("PMCH: %s", str);
  }

  // Save metrics stats
  if (ue_db.count(SRSRAN_MRNTI)) {
    ue_db[SRSRAN_MRNTI]->metrics_dl(mbsfn_cfg->mbsfn_mcs);
  }
  return SRSRAN_SUCCESS;
}

int cc_worker::encode_pdsch(stack_interface_phy_lte::dl_sched_grant_t* grants, uint32_t nof_grants)
{
  /* Scales the Resources Elements affected by the power allocation (p_b) */
  // srsran_enb_dl_prepare_power_allocation(&enb_dl);
  for (uint32_t i = 0; i < nof_grants; i++) {
    uint16_t rnti = grants[i].dci.rnti;

    if (rnti && ue_db.count(rnti)) {
      srsran_dl_cfg_t dl_cfg = {};

      if (phy->ue_db.get_dl_config(rnti, cc_idx, dl_cfg) < SRSRAN_SUCCESS) {
        Error("Error retrieving DCI DL configuration for RNTI %x, CC %d", grants[i].dci.rnti, cc_idx);
        continue;
      }

      // Compute DL grant
      if (srsran_ra_dl_dci_to_grant(
              &enb_dl.cell, &dl_sf, dl_cfg.tm, dl_cfg.pdsch.use_tbs_index_alt, &grants[i].dci, &dl_cfg.pdsch.grant)) {
        Error("Computing DL grant");
        continue;
      }

      // Set soft buffer
      for (uint32_t j = 0; j < SRSRAN_MAX_CODEWORDS; j++) {
        dl_cfg.pdsch.softbuffers.tx[j] = grants[i].softbuffer_tx[j];
      }

      // Encode PDSCH
      if (srsran_enb_dl_put_pdsch(&enb_dl, &dl_cfg.pdsch, grants[i].data)) {
        Error("Error putting PDSCH %d", i);
        return SRSRAN_ERROR;
      }

      // Save pending ACK
      if (SRSRAN_RNTI_ISUSER(rnti)) {
        // Push whole DCI
        phy->ue_db.set_ack_pending(tti_tx_ul, cc_idx, grants[i].dci);
      }

      if (LOG_THIS(rnti) and logger.info.enabled()) {
        // Logging
        char str[512];
        srsran_pdsch_tx_info(&dl_cfg.pdsch, str, 512);
        logger.info("PDSCH: cc=%d, %s, tti_tx_dl=%d", cc_idx, str, tti_tx_dl);
      }

      // Save metrics stats
      ue_db[rnti]->metrics_dl(grants[i].dci.tb[0].mcs_idx);
    } else {
      Error("User rnti=0x%x not found in cc_worker=%d", rnti, cc_idx);
    }
  }

  // srsran_enb_dl_apply_power_allocation(&enb_dl);

  return SRSRAN_SUCCESS;
}

/************ METRICS interface ********************/
uint32_t cc_worker::get_metrics(std::vector<phy_metrics_t>& metrics)
{
  std::lock_guard<std::mutex> lock(mutex);
  uint32_t                    cnt = 0;
  metrics.resize(ue_db.size());
  for (auto& ue : ue_db) {
    if ((SRSRAN_RNTI_ISUSER(ue.first) || ue.first == SRSRAN_MRNTI)) {
      ue.second->metrics_read(&metrics[cnt++]);
    }
  }
  metrics.resize(cnt);
  return cnt;
}

void cc_worker::ue::metrics_read(phy_metrics_t* metrics_)
{
  if (metrics_) {
    *metrics_ = metrics;
  }
  bzero(&metrics, sizeof(phy_metrics_t));
}

void cc_worker::ue::metrics_dl(uint32_t mcs)
{
  metrics.dl.mcs = SRSRAN_VEC_CMA(mcs, metrics.dl.mcs, metrics.dl.n_samples);
  metrics.dl.n_samples++;
}

void cc_worker::ue::metrics_ul(uint32_t mcs, float rssi, float sinr, float turbo_iters)
{
  metrics.ul.mcs         = SRSRAN_VEC_CMA((float)mcs, metrics.ul.mcs, metrics.ul.n_samples);
  metrics.ul.pusch_sinr  = SRSRAN_VEC_CMA((float)sinr, metrics.ul.pusch_sinr, metrics.ul.n_samples);
  metrics.ul.rssi        = SRSRAN_VEC_CMA((float)rssi, metrics.ul.rssi, metrics.ul.n_samples);
  metrics.ul.turbo_iters = SRSRAN_VEC_CMA((float)turbo_iters, metrics.ul.turbo_iters, metrics.ul.n_samples);
  metrics.ul.n_samples++;
}

void cc_worker::ue::metrics_ul_pucch(float sinr)
{
  metrics.ul.pucch_sinr = SRSRAN_VEC_CMA((float)sinr, metrics.ul.pucch_sinr, metrics.ul.n_samples_pucch);
  metrics.ul.n_samples_pucch++;
}

int cc_worker::read_ce_abs(float* ce_abs)
{
  int sz = srsran_symbol_sz(phy->get_nof_prb(cc_idx));
  srsran_vec_f_zero(ce_abs, sz);
  int g = (sz - SRSRAN_NRE * phy->get_nof_prb(cc_idx)) / 2;
  srsran_vec_abs_dB_cf(enb_ul.chest_res.ce, -80.0f, &ce_abs[g], SRSRAN_NRE * phy->get_nof_prb(cc_idx));
  return sz;
}

int cc_worker::read_ce_arg(float* ce_arg)
{
  int sz = srsran_symbol_sz(phy->get_nof_prb(cc_idx));
  srsran_vec_f_zero(ce_arg, sz);
  int g = (sz - SRSRAN_NRE * phy->get_nof_prb(cc_idx)) / 2;
  srsran_vec_arg_deg_cf(enb_ul.chest_res.ce, -80.0f, &ce_arg[g], SRSRAN_NRE * phy->get_nof_prb(cc_idx));
  return sz;
}

int cc_worker::read_pusch_d(cf_t* pdsch_d)
{
  int nof_re = enb_ul.pusch.max_re;
  memcpy(pdsch_d, enb_ul.pusch.d, nof_re * sizeof(cf_t));
  return nof_re;
}

int cc_worker::read_pucch_d(cf_t* pdsch_d)
{
  int nof_re = SRSRAN_PUCCH_MAX_BITS / 2;
  memcpy(pdsch_d, enb_ul.pucch.z_tmp, nof_re * sizeof(cf_t));
  return nof_re;
}

} // namespace lte
} // namespace srsenb
