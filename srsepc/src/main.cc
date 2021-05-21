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

#include "srsepc/hdr/hss/hss.h"
#include "srsepc/hdr/mme/mme.h"
#include "srsepc/hdr/spgw/spgw.h"
#include "srsran/build_info.h"
#include "srsran/common/bcd_helpers.h"
#include "srsran/common/common_helper.h"
#include "srsran/common/config_file.h"
#include "srsran/common/crash_handler.h"
#include "srsran/common/signal_handler.h"
#include "srsran/srslog/srslog.h"
#include "srsran/srsran.h"
#include <boost/program_options.hpp>
#include <iostream>

using namespace std;
using namespace srsepc;
namespace bpo = boost::program_options;

typedef struct {
  std::string nas_level;
  int         nas_hex_limit;
  std::string s1ap_level;
  int         s1ap_hex_limit;
  std::string mme_gtpc_level;
  int         mme_gtpc_hex_limit;
  std::string spgw_gtpc_level;
  int         spgw_gtpc_hex_limit;
  std::string gtpu_level;
  int         gtpu_hex_limit;
  std::string spgw_level;
  int         spgw_hex_limit;
  std::string hss_level;
  int         hss_hex_limit;
  std::string all_level;
  int         all_hex_limit;
  std::string filename;
} log_args_t;

typedef struct {
  mme_args_t  mme_args;
  hss_args_t  hss_args;
  spgw_args_t spgw_args;
  log_args_t  log_args;
} all_args_t;

/**********************************************************************
 *  Program arguments processing
 ***********************************************************************/
string config_file;

void parse_args(all_args_t* args, int argc, char* argv[])
{
  string   mme_name;
  string   mme_code;
  string   mme_group;
  string   tac;
  string   mcc;
  string   mnc;
  string   mme_bind_addr;
  string   mme_apn;
  string   encryption_algo;
  string   integrity_algo;
  uint16_t paging_timer     = 0;
  uint32_t max_paging_queue = 0;
  string   spgw_bind_addr;
  string   sgi_if_addr;
  string   sgi_if_name;
  string   sgi_if_netmask;
  string   dns_addr;
  string   full_net_name;
  string   short_net_name;
  string   hss_db_file;
  string   hss_auth_algo;
  string   log_filename;

  // Command line only options
  bpo::options_description general("General options");
  // clang-format off
  general.add_options()
      ("help,h", "Produce help message")
      ("version,v", "Print version information and exit")
      ;

  // Command line or config file options
  bpo::options_description common("Configuration options");
  common.add_options()
    ("mme.mme_code",        bpo::value<string>(&mme_code)->default_value("0x01"),            "MME Code")
    ("mme.name",            bpo::value<string>(&mme_name)->default_value("srsmme01"),        "MME Name")
    ("mme.mme_group",       bpo::value<string>(&mme_group)->default_value("0x01"),           "Cell ID")
    ("mme.tac",             bpo::value<string>(&tac)->default_value("0x0"),                  "Tracking Area Code")
    ("mme.mcc",             bpo::value<string>(&mcc)->default_value("001"),                  "Mobile Country Code")
    ("mme.mnc",             bpo::value<string>(&mnc)->default_value("01"),                   "Mobile Network Code")
    ("mme.mme_bind_addr",   bpo::value<string>(&mme_bind_addr)->default_value("127.0.0.1"),  "IP address of MME for S1 connection")
    ("mme.dns_addr",        bpo::value<string>(&dns_addr)->default_value("8.8.8.8"),         "IP address of the DNS server for the UEs")
    ("mme.full_net_name",   bpo::value<string>(&full_net_name)->default_value("Software Radio Systems RAN"), "Full name of the network")
    ("mme.short_net_name",  bpo::value<string>(&short_net_name)->default_value("srsRAN"),    "Short name of the network")
    ("mme.apn",             bpo::value<string>(&mme_apn)->default_value(""),                 "Set Access Point Name (APN) for data services")
    ("mme.encryption_algo", bpo::value<string>(&encryption_algo)->default_value("EEA0"),     "Set preferred encryption algorithm for NAS layer ")
    ("mme.integrity_algo",  bpo::value<string>(&integrity_algo)->default_value("EIA1"),      "Set preferred integrity protection algorithm for NAS")
    ("mme.paging_timer",    bpo::value<uint16_t>(&paging_timer)->default_value(2),           "Set paging timer value in seconds (T3413)")
    ("hss.db_file",         bpo::value<string>(&hss_db_file)->default_value("ue_db.csv"),    ".csv file that stores UE's keys")
    ("spgw.gtpu_bind_addr", bpo::value<string>(&spgw_bind_addr)->default_value("127.0.0.1"), "IP address of SP-GW for the S1-U connection")
    ("spgw.sgi_if_addr",    bpo::value<string>(&sgi_if_addr)->default_value("176.16.0.1"),   "IP address of TUN interface for the SGi connection")
    ("spgw.sgi_if_name",    bpo::value<string>(&sgi_if_name)->default_value("srs_spgw_sgi"), "Name of TUN interface for the SGi connection")
    ("spgw.sgi_if_netmask", bpo::value<string>(&sgi_if_netmask)->default_value("255.255.255.0"),  "IP mask of TUN interface for the SGi connection")
    ("spgw.max_paging_queue", bpo::value<uint32_t>(&max_paging_queue)->default_value(100), "Max number of packets in paging queue")

    ("pcap.enable",   bpo::value<bool>(&args->mme_args.s1ap_args.pcap_enable)->default_value(false),         "Enable S1AP PCAP")
    ("pcap.filename", bpo::value<string>(&args->mme_args.s1ap_args.pcap_filename)->default_value("/tmp/epc.pcap"), "PCAP filename")

    ("log.nas_level",           bpo::value<string>(&args->log_args.nas_level),        "MME NAS  log level")
    ("log.nas_hex_limit",       bpo::value<int>(&args->log_args.nas_hex_limit),       "MME NAS log hex dump limit")
    ("log.s1ap_level",          bpo::value<string>(&args->log_args.s1ap_level),       "MME S1AP log level")
    ("log.s1ap_hex_limit",      bpo::value<int>(&args->log_args.s1ap_hex_limit),      "MME S1AP log hex dump limit")
    ("log.mme_gtpc_level",      bpo::value<string>(&args->log_args.mme_gtpc_level),   "MME GTPC log level")
    ("log.mme_gtpc_hex_limit",  bpo::value<int>(&args->log_args.mme_gtpc_hex_limit),  "MME GTPC log hex dump limit")
    ("log.spgw_gtpc_level",     bpo::value<string>(&args->log_args.spgw_gtpc_level),  "SPGW GTPC log level")
    ("log.spgw_gtpc_hex_limit", bpo::value<int>(&args->log_args.spgw_gtpc_hex_limit), "SPGW GTPC log hex dump limit")
    ("log.gtpu_level",          bpo::value<string>(&args->log_args.gtpu_level),       "GTP-U log level")
    ("log.gtpu_hex_limit",      bpo::value<int>(&args->log_args.gtpu_hex_limit),      "GTP-U log hex dump limit")
    ("log.spgw_level",          bpo::value<string>(&args->log_args.spgw_level),       "SPGW log level")
    ("log.spgw_hex_limit",      bpo::value<int>(&args->log_args.spgw_hex_limit),      "SPGW log hex dump limit")
    ("log.hss_level",           bpo::value<string>(&args->log_args.hss_level),        "HSS log level")
    ("log.hss_hex_limit",       bpo::value<int>(&args->log_args.hss_hex_limit),       "HSS log hex dump limit")

    ("log.all_level",     bpo::value<string>(&args->log_args.all_level)->default_value("info"),   "ALL log level")
    ("log.all_hex_limit", bpo::value<int>(&args->log_args.all_hex_limit)->default_value(32),  "ALL log hex dump limit")

    ("log.filename", bpo::value<string>(&args->log_args.filename)->default_value("/tmp/epc.log"),"Log filename")
    ;

  // Positional options - config file location
  bpo::options_description position("Positional options");
  position.add_options()
    ("config_file", bpo::value< string >(&config_file), "MME configuration file")
  ;
  // clang-format on

  bpo::positional_options_description p;
  p.add("config_file", -1);

  // these options are allowed on the command line
  bpo::options_description cmdline_options;
  cmdline_options.add(common).add(position).add(general);

  // parse the command line and store result in vm
  bpo::variables_map vm;
  try {
    bpo::store(bpo::command_line_parser(argc, argv).options(cmdline_options).positional(p).run(), vm);
    bpo::notify(vm);
  } catch (bpo::error& e) {
    cerr << e.what() << endl;
    exit(1);
  }

  // help option was given - print usage and exit
  if (vm.count("help")) {
    cout << "Usage: " << argv[0] << " [OPTIONS] config_file" << endl << endl;
    cout << common << endl << general << endl;
    exit(0);
  }

  // if no config file given, check users home path
  if (!vm.count("config_file")) {
    if (!config_exists(config_file, "epc.conf")) {
      cout << "Failed to read EPC configuration file " << config_file << " - exiting" << endl;
      exit(1);
    }
  }

  // Parsing Config File
  cout << "Reading configuration file " << config_file << "..." << endl;
  ifstream conf(config_file.c_str(), ios::in);
  if (conf.fail()) {
    cout << "Failed to read configuration file " << config_file << " - exiting" << endl;
    exit(1);
  }
  // parse config file and handle errors gracefully
  try {
    bpo::store(bpo::parse_config_file(conf, common), vm);
    bpo::notify(vm);
  } catch (const boost::program_options::error& e) {
    cerr << e.what() << endl;
    exit(1);
  }

  // Concert hex strings
  {
    std::stringstream sstr;
    sstr << std::hex << vm["mme.mme_group"].as<std::string>();
    sstr >> args->mme_args.s1ap_args.mme_group;
  }
  {
    std::stringstream sstr;
    sstr << std::hex << vm["mme.mme_code"].as<std::string>();
    uint16_t tmp; // Need intermediate uint16_t as uint8_t is treated as char
    sstr >> tmp;
    args->mme_args.s1ap_args.mme_code = tmp;
  }
  {
    std::stringstream sstr;
    sstr << std::hex << vm["mme.tac"].as<std::string>();
    sstr >> args->mme_args.s1ap_args.tac;
  }

  // Convert MCC/MNC strings
  if (!srsran::string_to_mcc(mcc, &args->mme_args.s1ap_args.mcc)) {
    cout << "Error parsing mme.mcc:" << mcc << " - must be a 3-digit string." << endl;
  }
  if (!srsran::string_to_mnc(mnc, &args->mme_args.s1ap_args.mnc)) {
    cout << "Error parsing mme.mnc:" << mnc << " - must be a 2 or 3-digit string." << endl;
  }

  // Convert MCC/MNC strings
  if (!srsran::string_to_mcc(mcc, &args->hss_args.mcc)) {
    cout << "Error parsing mme.mcc:" << mcc << " - must be a 3-digit string." << endl;
  }
  if (!srsran::string_to_mnc(mnc, &args->hss_args.mnc)) {
    cout << "Error parsing mme.mnc:" << mnc << " - must be a 2 or 3-digit string." << endl;
  }

  std::transform(encryption_algo.begin(), encryption_algo.end(), encryption_algo.begin(), ::tolower);
  if (encryption_algo == "eea0") {
    args->mme_args.s1ap_args.encryption_algo = srsran::CIPHERING_ALGORITHM_ID_EEA0;
  } else if (encryption_algo == "eea1") {
    args->mme_args.s1ap_args.encryption_algo = srsran::CIPHERING_ALGORITHM_ID_128_EEA1;
  } else if (encryption_algo == "eea2") {
    args->mme_args.s1ap_args.encryption_algo = srsran::CIPHERING_ALGORITHM_ID_128_EEA2;
  } else if (encryption_algo == "eea3") {
    args->mme_args.s1ap_args.encryption_algo = srsran::CIPHERING_ALGORITHM_ID_128_EEA3;
  } else {
    args->mme_args.s1ap_args.encryption_algo = srsran::CIPHERING_ALGORITHM_ID_EEA0;
    cout << "Error parsing mme.encryption_algo:" << encryption_algo << " - must be EEA0, EEA1, EEA2 or EEA3." << endl;
    cout << "Using default mme.encryption_algo: EEA0" << endl;
  }

  std::transform(integrity_algo.begin(), integrity_algo.end(), integrity_algo.begin(), ::tolower);
  if (integrity_algo == "eia0") {
    args->mme_args.s1ap_args.integrity_algo = srsran::INTEGRITY_ALGORITHM_ID_EIA0;
    cout << "Warning parsing mme.integrity_algo:" << encryption_algo
         << " - EIA0 will not supported by UEs use EIA1 or EIA2" << endl;
  } else if (integrity_algo == "eia1") {
    args->mme_args.s1ap_args.integrity_algo = srsran::INTEGRITY_ALGORITHM_ID_128_EIA1;
  } else if (integrity_algo == "eia2") {
    args->mme_args.s1ap_args.integrity_algo = srsran::INTEGRITY_ALGORITHM_ID_128_EIA2;
  } else if (integrity_algo == "eia3") {
    args->mme_args.s1ap_args.integrity_algo = srsran::INTEGRITY_ALGORITHM_ID_128_EIA3;
  } else {
    args->mme_args.s1ap_args.integrity_algo = srsran::INTEGRITY_ALGORITHM_ID_128_EIA1;
    cout << "Error parsing mme.integrity_algo:" << encryption_algo << " - must be EIA0, EIA1, EIA2 or EIA3." << endl;
    cout << "Using default mme.integrity_algo: EIA1" << endl;
  }

  args->mme_args.s1ap_args.mme_bind_addr = mme_bind_addr;
  args->mme_args.s1ap_args.mme_name      = mme_name;
  args->mme_args.s1ap_args.dns_addr      = dns_addr;
  args->mme_args.s1ap_args.full_net_name  = full_net_name;
  args->mme_args.s1ap_args.short_net_name = short_net_name;
  args->mme_args.s1ap_args.mme_apn       = mme_apn;
  args->mme_args.s1ap_args.paging_timer  = paging_timer;
  args->spgw_args.gtpu_bind_addr         = spgw_bind_addr;
  args->spgw_args.sgi_if_addr            = sgi_if_addr;
  args->spgw_args.sgi_if_name            = sgi_if_name;
  args->spgw_args.sgi_if_netmask         = sgi_if_netmask;
  args->spgw_args.max_paging_queue       = max_paging_queue;
  args->hss_args.db_file                 = hss_db_file;

  // Apply all_level to any unset layers
  if (vm.count("log.all_level")) {
    if (!vm.count("log.nas_level")) {
      args->log_args.nas_level = args->log_args.all_level;
    }
    if (!vm.count("log.s1ap_level")) {
      args->log_args.s1ap_level = args->log_args.all_level;
    }
    if (!vm.count("log.mme_gtpc_level")) {
      args->log_args.mme_gtpc_level = args->log_args.all_level;
    }
    if (!vm.count("log.spgw_gtpc_level")) {
      args->log_args.spgw_gtpc_level = args->log_args.all_level;
    }
    if (!vm.count("log.gtpu_level")) {
      args->log_args.gtpu_level = args->log_args.all_level;
    }
    if (!vm.count("log.spgw_level")) {
      args->log_args.spgw_level = args->log_args.all_level;
    }
    if (!vm.count("log.hss_level")) {
      args->log_args.hss_level = args->log_args.all_level;
    }
  }

  // Apply all_hex_limit to any unset layers
  if (vm.count("log.all_hex_limit")) {
    if (!vm.count("log.s1ap_hex_limit")) {
      args->log_args.s1ap_hex_limit = args->log_args.all_hex_limit;
    }
    if (!vm.count("log.mme_gtpc_hex_limit")) {
      args->log_args.mme_gtpc_hex_limit = args->log_args.all_hex_limit;
    }
    if (!vm.count("log.spgw_gtpc_hex_limit")) {
      args->log_args.spgw_gtpc_hex_limit = args->log_args.all_hex_limit;
    }
    if (!vm.count("log.gtpu_hex_limit")) {
      args->log_args.gtpu_hex_limit = args->log_args.all_hex_limit;
    }
    if (!vm.count("log.spgw_hex_limit")) {
      args->log_args.spgw_hex_limit = args->log_args.all_hex_limit;
    }
    if (!vm.count("log.hss_hex_limit")) {
      args->log_args.hss_hex_limit = args->log_args.all_hex_limit;
    }
    if (!vm.count("log.nas_hex_limit")) {
      args->log_args.nas_hex_limit = args->log_args.all_hex_limit;
    }
  }

  // Check user database
  if (!config_exists(args->hss_args.db_file, "user_db.csv")) {
    cout << "Failed to read HSS user database file " << args->hss_args.db_file << " - exiting" << endl;
    exit(1);
  }

  return;
}

std::string get_build_mode()
{
  return std::string(srsran_get_build_mode());
}

std::string get_build_info()
{
  if (std::string(srsran_get_build_info()).find("  ") != std::string::npos) {
    return std::string(srsran_get_version());
  }
  return std::string(srsran_get_build_info());
}

std::string get_build_string()
{
  std::stringstream ss;
  ss << "Built in " << get_build_mode() << " mode using " << get_build_info() << "." << std::endl;
  return ss.str();
}

int main(int argc, char* argv[])
{
  srsran_register_signal_handler();

  // print build info
  cout << endl << get_build_string() << endl;

  cout << endl << "---  Software Radio Systems EPC  ---" << endl << endl;
  srsran_debug_handle_crash(argc, argv);

  all_args_t args;
  parse_args(&args, argc, argv);

  // Setup logging.
  log_sink = (args.log_args.filename == "stdout") ? srslog::create_stdout_sink()
                                                  : srslog::create_file_sink(args.log_args.filename);
  if (!log_sink) {
    return SRSRAN_ERROR;
  }
  srslog::log_channel* chan = srslog::create_log_channel("main_channel", *log_sink);
  if (!chan) {
    return SRSRAN_ERROR;
  }
  srslog::set_default_sink(*log_sink);

  // Start the log backend.
  srslog::init();

  if (args.log_args.filename != "stdout") {
    auto& epc_logger = srslog::fetch_basic_logger("EPC", false);
    epc_logger.info("\n\n%s\n---  Software Radio Systems EPC log ---\n\n", get_build_string().c_str());
  }

  srsran::log_args(argc, argv, "EPC");

  srslog::basic_logger& nas_logger = srslog::fetch_basic_logger("NAS", false);
  nas_logger.set_level(srslog::str_to_basic_level(args.log_args.nas_level));
  nas_logger.set_hex_dump_max_size(args.log_args.nas_hex_limit);

  auto& s1ap_logger = srslog::fetch_basic_logger("S1AP", false);
  s1ap_logger.set_level(srslog::str_to_basic_level(args.log_args.s1ap_level));
  s1ap_logger.set_hex_dump_max_size(args.log_args.s1ap_hex_limit);

  auto& mme_gtpc_logger = srslog::fetch_basic_logger("MME GTPC", false);
  mme_gtpc_logger.set_level(srslog::str_to_basic_level(args.log_args.mme_gtpc_level));
  mme_gtpc_logger.set_hex_dump_max_size(args.log_args.mme_gtpc_hex_limit);

  auto& hss_logger = srslog::fetch_basic_logger("HSS", false);
  hss_logger.set_level(srslog::str_to_basic_level(args.log_args.hss_level));
  hss_logger.set_hex_dump_max_size(args.log_args.hss_hex_limit);

  auto& spgw_gtpc_logger = srslog::fetch_basic_logger("SPGW GTPC", false);
  spgw_gtpc_logger.set_level(srslog::str_to_basic_level(args.log_args.spgw_gtpc_level));
  spgw_gtpc_logger.set_hex_dump_max_size(args.log_args.spgw_gtpc_hex_limit);

  auto& gtpu_logger = srslog::fetch_basic_logger("GTPU", false);
  gtpu_logger.set_level(srslog::str_to_basic_level(args.log_args.gtpu_level));
  gtpu_logger.set_hex_dump_max_size(args.log_args.gtpu_hex_limit);

  auto& spgw_logger = srslog::fetch_basic_logger("SPGW", false);
  spgw_logger.set_level(srslog::str_to_basic_level(args.log_args.spgw_level));
  spgw_logger.set_hex_dump_max_size(args.log_args.spgw_hex_limit);

  hss* hss = hss::get_instance();
  if (hss->init(&args.hss_args)) {
    cout << "Error initializing HSS" << endl;
    exit(1);
  }

  mme* mme = mme::get_instance();
  if (mme->init(&args.mme_args)) {
    cout << "Error initializing MME" << endl;
    exit(1);
  }

  spgw* spgw = spgw::get_instance();
  if (spgw->init(&args.spgw_args, hss->get_ip_to_imsi())) {
    cout << "Error initializing SP-GW" << endl;
    exit(1);
  }

  mme->start();
  spgw->start();
  while (running) {
    sleep(1);
  }

  mme->stop();
  mme->cleanup();
  spgw->stop();
  spgw->cleanup();
  hss->stop();
  hss->cleanup();

  cout << std::endl << "---  exiting  ---" << endl;
  return 0;
}
