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

/******************************************************************************
 * File:        enb.h
 * Description: Top-level eNodeB class. Creates and links all
 *              layers and helpers.
 *****************************************************************************/

#ifndef SRSENB_ENB_H
#define SRSENB_ENB_H

#include <pthread.h>
#include <stdarg.h>
#include <string>

#include "phy/phy.h"

#include "srsran/radio/radio.h"

#include "srsenb/hdr/phy/enb_phy_base.h"
#include "srsenb/hdr/stack/enb_stack_base.h"
#include "srsenb/hdr/stack/rrc/rrc_config.h"

#include "srsran/common/bcd_helpers.h"
#include "srsran/common/buffer_pool.h"
#include "srsran/common/interfaces_common.h"
#include "srsran/common/mac_pcap.h"
#include "srsran/common/security.h"
#include "srsran/interfaces/enb_command_interface.h"
#include "srsran/interfaces/enb_metrics_interface.h"
#include "srsran/interfaces/sched_interface.h"
#include "srsran/interfaces/ue_interfaces.h"
#include "srsran/srslog/srslog.h"
#include "srsran/system/sys_metrics_processor.h"

namespace srsenb {

/*******************************************************************************
  eNodeB Parameters
*******************************************************************************/

struct enb_args_t {
  uint32_t enb_id;
  uint32_t dl_earfcn; // By default the EARFCN from rr.conf's cell list are used but this value can be used for single
                      // cell eNB
  uint32_t n_prb;
  uint32_t nof_ports;
  uint32_t transmission_mode;
  float    p_a;
};

struct enb_files_t {
  std::string sib_config;
  std::string rr_config;
  std::string drb_config;
};

struct log_args_t {
  std::string all_level;
  int         phy_hex_limit;

  int         all_hex_limit;
  int         file_max_size;
  std::string filename;
};

struct gui_args_t {
  bool enable;
};

struct general_args_t {
  uint32_t    rrc_inactivity_timer;
  float       metrics_period_secs;
  bool        metrics_csv_enable;
  std::string metrics_csv_filename;
  bool        report_json_enable;
  std::string report_json_filename;
  bool        alarms_log_enable;
  std::string alarms_filename;
  bool        print_buffer_state;
  bool        tracing_enable;
  std::size_t tracing_buffcapacity;
  std::string tracing_filename;
  std::string eia_pref_list;
  std::string eea_pref_list;
  uint32_t    max_mac_dl_kos;
  uint32_t    max_mac_ul_kos;
};

struct all_args_t {
  enb_args_t        enb;
  enb_files_t       enb_files;
  srsran::rf_args_t rf;
  log_args_t        log;
  gui_args_t        gui;
  general_args_t    general;
  phy_args_t        phy;
  stack_args_t      stack;
};

struct rrc_cfg_t;

/*******************************************************************************
  Main eNB class
*******************************************************************************/

class enb : public enb_metrics_interface, enb_command_interface
{
public:
  enb(srslog::sink& log_sink);

  virtual ~enb();

  int init(const all_args_t& args_);

  void stop();

  void start_plot();

  void print_pool();

  // eNodeB metrics interface
  bool get_metrics(enb_metrics_t* m) override;

  // eNodeB command interface
  void cmd_cell_measure() override;
  void cmd_cell_gain(uint32_t cell_id, float gain) override;

private:
  const static int ENB_POOL_SIZE = 1024 * 10;

  int parse_args(const all_args_t& args_, rrc_cfg_t& rrc_cfg);

  srslog::sink&         log_sink;
  srslog::basic_logger& enb_log;

  all_args_t args    = {};
  bool       started = false;

  phy_cfg_t phy_cfg = {};
  rrc_cfg_t rrc_cfg = {};

  // eNB components
  std::unique_ptr<enb_stack_base>     stack = nullptr;
  std::unique_ptr<srsran::radio_base> radio = nullptr;
  std::unique_ptr<enb_phy_base>       phy   = nullptr;

  // System metrics processor.
  srsran::sys_metrics_processor sys_proc;

  std::string get_build_mode();
  std::string get_build_info();
  std::string get_build_string();
};

} // namespace srsenb

#endif // SRSENB_ENB_H
