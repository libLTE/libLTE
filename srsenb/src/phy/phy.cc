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

#include <pthread.h>
#include <sstream>
#include <string.h>
#include <string>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

#include "srsenb/hdr/phy/phy.h"
#include "srsran/common/threads.h"

#define Error(fmt, ...)                                                                                                \
  if (SRSRAN_DEBUG_ENABLED)                                                                                            \
  phy_log.error(fmt, ##__VA_ARGS__)
#define Warning(fmt, ...)                                                                                              \
  if (SRSRAN_DEBUG_ENABLED)                                                                                            \
  phy_log.warning(fmt, ##__VA_ARGS__)
#define Info(fmt, ...)                                                                                                 \
  if (SRSRAN_DEBUG_ENABLED)                                                                                            \
  phy_log.info(fmt, ##__VA_ARGS__)
#define Debug(fmt, ...)                                                                                                \
  if (SRSRAN_DEBUG_ENABLED)                                                                                            \
  phy_log.debug(fmt, ##__VA_ARGS__)

using namespace std;
using namespace asn1::rrc;

namespace srsenb {

static void srsran_phy_handler(phy_logger_level_t log_level, void* ctx, char* str)
{
  phy* r = (phy*)ctx;
  r->srsran_phy_logger(log_level, str);
}

void phy::srsran_phy_logger(phy_logger_level_t log_level, char* str)
{
  switch (log_level) {
    case LOG_LEVEL_INFO_S:
      phy_lib_log.info(" %s", str);
      break;
    case LOG_LEVEL_DEBUG_S:
      phy_lib_log.debug(" %s", str);
      break;
    case LOG_LEVEL_ERROR_S:
      phy_lib_log.error(" %s", str);
      break;
    default:
      break;
  }
}

phy::phy(srslog::sink& log_sink) :
  log_sink(log_sink),
  phy_log(srslog::fetch_basic_logger("PHY", log_sink)),
  phy_lib_log(srslog::fetch_basic_logger("PHY_LIB", log_sink)),
  lte_workers(MAX_WORKERS),
  nr_workers(MAX_WORKERS),
  workers_common(),
  nof_workers(0),
  tx_rx(phy_log)
{}

phy::~phy()
{
  stop();
}

void phy::parse_common_config(const phy_cfg_t& cfg)
{
  // PRACH configuration
  prach_cfg.config_idx       = cfg.prach_cnfg.prach_cfg_info.prach_cfg_idx;
  prach_cfg.hs_flag          = cfg.prach_cnfg.prach_cfg_info.high_speed_flag;
  prach_cfg.root_seq_idx     = cfg.prach_cnfg.root_seq_idx;
  prach_cfg.zero_corr_zone   = cfg.prach_cnfg.prach_cfg_info.zero_correlation_zone_cfg;
  prach_cfg.freq_offset      = cfg.prach_cnfg.prach_cfg_info.prach_freq_offset;
  prach_cfg.num_ra_preambles = cfg.phy_cell_cfg.at(0).num_ra_preambles;
  // DMRS
  workers_common.dmrs_pusch_cfg.cyclic_shift        = cfg.pusch_cnfg.ul_ref_sigs_pusch.cyclic_shift;
  workers_common.dmrs_pusch_cfg.delta_ss            = cfg.pusch_cnfg.ul_ref_sigs_pusch.group_assign_pusch;
  workers_common.dmrs_pusch_cfg.group_hopping_en    = cfg.pusch_cnfg.ul_ref_sigs_pusch.group_hop_enabled;
  workers_common.dmrs_pusch_cfg.sequence_hopping_en = cfg.pusch_cnfg.ul_ref_sigs_pusch.seq_hop_enabled;
}

int phy::init(const phy_args_t&            args,
              const phy_cfg_t&             cfg,
              srsran::radio_interface_phy* radio_,
              stack_interface_phy_lte*     stack_)
{
  if (cfg.phy_cell_cfg.size() > SRSRAN_MAX_CARRIERS) {
    phy_log.error(
        "Number of carriers=%zd provided above its maximum=%zd", cfg.phy_cell_cfg.size(), SRSRAN_MAX_CARRIERS);
    return SRSRAN_ERROR;
  }

  mlockall((uint32_t)MCL_CURRENT | (uint32_t)MCL_FUTURE);

  // Add PHY lib log.
  srslog::basic_levels log_lvl = srslog::str_to_basic_level(args.log.phy_lib_level);

  phy_lib_log.set_level(log_lvl);
  phy_lib_log.set_hex_dump_max_size(args.log.phy_hex_limit);
  if (log_lvl != srslog::basic_levels::none) {
    srsran_phy_log_register_handler(this, srsran_phy_handler);
  }

  // Create default log.
  phy_log.set_level(log_lvl);
  phy_log.set_hex_dump_max_size(args.log.phy_hex_limit);

  radio       = radio_;
  nof_workers = args.nof_phy_threads;

  workers_common.params = args;

  workers_common.init(cfg.phy_cell_cfg, cfg.phy_cell_cfg_nr, radio, stack_);

  parse_common_config(cfg);

  // Add workers to workers pool and start threads
  if (not cfg.phy_cell_cfg.empty()) {
    lte_workers.init(args, &workers_common, log_sink, WORKERS_THREAD_PRIO);
  }
  if (not cfg.phy_cell_cfg_nr.empty()) {
    nr_workers.init(args, &workers_common, log_sink, WORKERS_THREAD_PRIO);
  }

  // For each carrier, initialise PRACH worker
  for (uint32_t cc = 0; cc < cfg.phy_cell_cfg.size(); cc++) {
    prach_cfg.root_seq_idx = cfg.phy_cell_cfg[cc].root_seq_idx;
    prach.init(
        cc, cfg.phy_cell_cfg[cc].cell, prach_cfg, stack_, phy_log, PRACH_WORKER_THREAD_PRIO, args.nof_prach_threads);
  }
  prach.set_max_prach_offset_us(args.max_prach_offset_us);

  // Warning this must be initialized after all workers have been added to the pool
  tx_rx.init(stack_, radio, &lte_workers, &nr_workers, &workers_common, &prach, SF_RECV_THREAD_PRIO);

  initialized = true;

  return SRSRAN_SUCCESS;
}

void phy::stop()
{
  if (initialized) {
    tx_rx.stop();
    workers_common.stop();
    lte_workers.stop();
    nr_workers.stop();
    prach.stop();

    initialized = false;
  }
}

/***** MAC->PHY interface **********/

void phy::rem_rnti(uint16_t rnti)
{
  // Remove the RNTI when the TTI finishes, this has a delay up to the pipeline length (3 ms)
  for (uint32_t i = 0; i < nof_workers; i++) {
    lte::sf_worker* w = lte_workers.wait_worker_id(i);
    if (w) {
      w->rem_rnti(rnti);
      w->release();
    }
  }
  if (SRSRAN_RNTI_ISUSER(rnti)) {
    workers_common.ue_db.rem_rnti(rnti);
    workers_common.clear_grants(rnti);
  }
}

void phy::set_mch_period_stop(uint32_t stop)
{
  workers_common.set_mch_period_stop(stop);
}

void phy::set_activation_deactivation_scell(uint16_t rnti, const std::array<bool, SRSRAN_MAX_CARRIERS>& activation)
{
  // Iterate all elements except 0 that is reserved for primary cell
  for (uint32_t scell_idx = 1; scell_idx < SRSRAN_MAX_CARRIERS; scell_idx++) {
    workers_common.ue_db.activate_deactivate_scell(rnti, scell_idx, activation[scell_idx]);
  }
}

void phy::get_metrics(std::vector<phy_metrics_t>& metrics)
{
  std::vector<phy_metrics_t> metrics_tmp;
  for (uint32_t i = 0; i < nof_workers; i++) {
    lte_workers[i]->get_metrics(metrics_tmp);
    metrics.resize(std::max(metrics_tmp.size(), metrics.size()));
    for (uint32_t j = 0; j < metrics_tmp.size(); j++) {
      metrics[j].dl.n_samples += metrics_tmp[j].dl.n_samples;
      metrics[j].dl.mcs += metrics_tmp[j].dl.n_samples * metrics_tmp[j].dl.mcs;

      metrics[j].ul.n_samples += metrics_tmp[j].ul.n_samples;
      metrics[j].ul.n_samples_pucch += metrics_tmp[j].ul.n_samples_pucch;
      metrics[j].ul.mcs += metrics_tmp[j].ul.n_samples * metrics_tmp[j].ul.mcs;
      metrics[j].ul.n += metrics_tmp[j].ul.n_samples * metrics_tmp[j].ul.n;
      metrics[j].ul.rssi += metrics_tmp[j].ul.n_samples * metrics_tmp[j].ul.rssi;
      metrics[j].ul.pusch_sinr += metrics_tmp[j].ul.n_samples * metrics_tmp[j].ul.pusch_sinr;
      metrics[j].ul.pucch_sinr += metrics_tmp[j].ul.n_samples_pucch * metrics_tmp[j].ul.pucch_sinr;
      metrics[j].ul.turbo_iters += metrics_tmp[j].ul.n_samples * metrics_tmp[j].ul.turbo_iters;
    }
  }
  for (uint32_t j = 0; j < metrics.size(); j++) {
    metrics[j].dl.mcs /= metrics[j].dl.n_samples;
    metrics[j].ul.mcs /= metrics[j].ul.n_samples;
    metrics[j].ul.n /= metrics[j].ul.n_samples;
    metrics[j].ul.rssi /= metrics[j].ul.n_samples;
    metrics[j].ul.pusch_sinr /= metrics[j].ul.n_samples;
    metrics[j].ul.pucch_sinr /= metrics[j].ul.n_samples_pucch;
    metrics[j].ul.turbo_iters /= metrics[j].ul.n_samples;
  }
}

void phy::cmd_cell_gain(uint32_t cell_id, float gain_db)
{
  Info("set_cell_gain: cell_id=%d, gain_db=%.2f", cell_id, gain_db);
  workers_common.set_cell_gain(cell_id, gain_db);
}

void phy::cmd_cell_measure()
{
  workers_common.set_cell_measure_trigger();
}

/***** RRC->PHY interface **********/

void phy::set_config(uint16_t rnti, const phy_rrc_cfg_list_t& phy_cfg_list)
{
  // Update UE Database
  workers_common.ue_db.addmod_rnti(rnti, phy_cfg_list);

  // Iterate over the list and add the RNTIs
  for (const phy_rrc_cfg_t& config : phy_cfg_list) {
    // Add RNTI to eNb cell/carrier.
    // - Do not ignore PCell, it could have changed
    // - Do not remove RNTI from unused workers, it will be removed when the UE is released
    if (config.configured) {
      // Add RNTI to all SF workers
      for (uint32_t w = 0; w < nof_workers; w++) {
        lte_workers[w]->add_rnti(rnti, config.enb_cc_idx);
      }
    }
  }
}

void phy::complete_config(uint16_t rnti)
{
  // Forwards call to the UE Database
  if (workers_common.ue_db.complete_config(rnti) < SRSRAN_SUCCESS) {
    Error("Error completing configuration for RNTI %x. It does not exist.", rnti);
  }
}

void phy::configure_mbsfn(srsran::sib2_mbms_t* sib2, srsran::sib13_t* sib13, const srsran::mcch_msg_t& mcch)
{
  if (sib2->mbsfn_sf_cfg_list_present) {
    if (sib2->nof_mbsfn_sf_cfg == 0) {
      Warning("SIB2 does not have any MBSFN config although it was set as present");
    } else {
      if (sib2->nof_mbsfn_sf_cfg > 1) {
        Warning("SIB2 has %d MBSFN subframe configs - only 1 supported", sib2->nof_mbsfn_sf_cfg);
      }
      mbsfn_config.mbsfn_subfr_cnfg = sib2->mbsfn_sf_cfg_list[0];
    }
  } else {
    fprintf(stderr, "SIB2 has no MBSFN subframe config specified\n");
    return;
  }

  mbsfn_config.mbsfn_notification_cnfg = sib13->notif_cfg;
  if (sib13->nof_mbsfn_area_info > 0) {
    if (sib13->nof_mbsfn_area_info > 1) {
      Warning("SIB13 has %d MBSFN area info elements - only 1 supported", sib13->nof_mbsfn_area_info);
    }
    mbsfn_config.mbsfn_area_info = sib13->mbsfn_area_info_list[0];
  }

  mbsfn_config.mcch = mcch;

  workers_common.configure_mbsfn(&mbsfn_config);
}

// Start GUI
void phy::start_plot()
{
  lte_workers[0]->start_plot();
}

} // namespace srsenb
