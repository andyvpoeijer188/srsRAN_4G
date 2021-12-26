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

#include "srsue/hdr/phy/nr/sync_sa.h"
#include "srsran/radio/rf_buffer.h"

namespace srsue {
namespace nr {
sync_sa::sync_sa(srslog::basic_logger& logger_, worker_pool& workers_) :
  logger(logger_), workers(workers_), slot_synchronizer(logger_), searcher(logger_), srsran::thread("SYNC")
{}

sync_sa::~sync_sa() {}

bool sync_sa::init(const args_t& args, stack_interface_phy_nr* stack_, srsran::radio_interface_phy* radio_)
{
  stack   = stack_;
  radio   = radio_;
  slot_sz = (uint32_t)(args.srate_hz / 1000.0f);

  // Initialise cell search internal object
  if (not searcher.init(args.get_cell_search())) {
    logger.error("Error initialising cell searcher");
    return false;
  }

  // Initialise slot synchronizer object
  if (not slot_synchronizer.init(args.get_slot_sync(), stack, radio)) {
    logger.error("Error initialising slot synchronizer");
    return false;
  }

  // Cell bandwidth must be provided at init so set now sampling rate
  radio->set_rx_srate(args.srate_hz);
  radio->set_tx_srate(args.srate_hz);

  // Compute subframe size
  slot_sz = (uint32_t)(args.srate_hz / 1000.0f);

  // Allocate receive buffer
  rx_buffer = srsran_vec_cf_malloc(2 * slot_sz);
  if (rx_buffer == nullptr) {
    logger.error("Error allocating buffer");
    return false;
  }

  // Thread control
  running = true;
  start(args.thread_priority);

  // If reached here it was successful
  return true;
}

void sync_sa::stop()
{
  running = false;
  wait_thread_finish();
  radio->reset();
}

bool sync_sa::reset()
{
  // Wait worker pool to finish any processing
  tti_semaphore.wait_all();

  return true;
}

void sync_sa::cell_go_idle()
{
  std::unique_lock<std::mutex> ul(rrc_mutex);
  phy_state.go_idle();
}

bool sync_sa::wait_idle()
{
  // Wait for SYNC thread to transition to IDLE (max. 2000ms)
  if (!phy_state.wait_idle(100)) {
    return false;
  }

  // Reset UE sync. Attention: doing this reset when the FSM is NOT IDLE can cause PSS/SSS out-of-sync
  //...

  // Wait for workers to finish PHY processing
  tti_semaphore.wait_all();

  // As workers have finished, make sure the Tx burst is ended
  radio->tx_end();

  return phy_state.is_idle();
}

cell_search::ret_t sync_sa::cell_search_run(const cell_search::cfg_t& cfg)
{
  std::unique_lock<std::mutex> ul(rrc_mutex);

  cs_ret        = {};
  cs_ret.result = cell_search::ret_t::ERROR;

  // Wait the FSM to transition to IDLE
  if (!wait_idle()) {
    logger.error("Cell Search: SYNC thread didn't transition to IDLE after 100 ms\n");
    return cs_ret;
  }

  rrc_proc_state = PROC_SEARCH_RUNNING;

  // Configure searcher without locking state for avoiding stalling the Rx stream
  logger.info("Cell search: starting in center frequency %.2f and SSB frequency %.2f with subcarrier spacing of %s",
              cfg.center_freq_hz / 1e6,
              cfg.ssb_freq_hz / 1e6,
              srsran_subcarrier_spacing_to_str(cfg.ssb_scs));

  if (not searcher.start(cfg)) {
    logger.error("Sync: failed to start cell search");
    return cs_ret;
  }

  // Zero receive buffer
  srsran_vec_zero(rx_buffer, slot_sz);

  logger.info("Cell Search: Running Cell search state");
  cell_search_nof_trials = 0;
  phy_state.run_cell_search();

  rrc_proc_state = PROC_IDLE;

  return cs_ret;
}

bool sync_sa::cell_select_run(const phy_interface_rrc_nr::cell_select_args_t& req)
{
  std::unique_lock<std::mutex> ul(rrc_mutex);

  // Wait the FSM to transition to IDLE
  if (!wait_idle()) {
    logger.error("Cell Search: SYNC thread didn't transition to IDLE after 100 ms\n");
    return false;
  }

  rrc_proc_state = PROC_SELECT_RUNNING;

  // tune radio
  logger.info("Tuning Rx channel %d to %.2f MHz", 0, req.carrier.dl_center_frequency_hz / 1e6);
  radio->set_rx_freq(0, req.carrier.dl_center_frequency_hz);
  logger.info("Tuning Tx channel %d to %.2f MHz", 0, req.carrier.ul_center_frequency_hz / 1e6);
  radio->set_tx_freq(0, req.carrier.ul_center_frequency_hz);

  // SFN synchronization
  phy_state.run_sfn_sync();
  if (phy_state.is_camping()) {
    logger.info("Cell Select: SFN synchronized. CAMPING...");
  } else {
    logger.info("Cell Select: Could not synchronize SFN");
  }

  rrc_proc_state = PROC_IDLE;
  return true;
}

sync_state::state_t sync_sa::get_state()
{
  return phy_state.get_state();
}

void sync_sa::run_state_idle()
{
#define test 0
  if (radio->is_init() && test) {
    logger.debug("Discarding samples and sending tx_end");
    srsran::rf_buffer_t rf_buffer = {};
    rf_buffer.set_nof_samples(slot_sz);
    rf_buffer.set(0, rx_buffer);
    if (not slot_synchronizer.recv_callback(rf_buffer, last_rx_time.get_ptr(0))) {
      logger.error("SYNC: receiving from radio\n");
    }
    radio->tx_end();
  } else {
    logger.debug("Sleeping 1 s");
    sleep(1);
  }
}

void sync_sa::run_state_cell_search()
{
  // Receive samples
  srsran::rf_buffer_t rf_buffer = {};
  rf_buffer.set_nof_samples(slot_sz);
  rf_buffer.set(0, rx_buffer);
  if (not slot_synchronizer.recv_callback(rf_buffer, last_rx_time.get_ptr(0))) {
    logger.error("SYNC: receiving from radio\n");
  }

  // Run Searcher
  cs_ret = searcher.run_slot(rx_buffer, slot_sz);
  if (cs_ret.result < 0) {
    logger.error("Failed to run searcher. Transitioning to IDLE...");
  }

  cell_search_nof_trials++;

  // Leave CELL_SEARCH state if error or success and transition to IDLE
  if (cs_ret.result || cell_search_nof_trials >= cell_search_max_trials) {
    phy_state.state_exit();
  }
}

void sync_sa::run_state_cell_select()
{
  // TODO
  tti = 10240 - 4;
  phy_state.state_exit();
}

void sync_sa::run_state_cell_camping()
{
  nr::sf_worker* nr_worker = nullptr;
  nr_worker                = workers.wait_worker(tti);
  if (nr_worker == nullptr) {
    running = false;
    return;
  }

  // Receive samples
  srsran::rf_buffer_t rf_buffer = {};
  rf_buffer.set_nof_samples(slot_sz);
  rf_buffer.set(0, nr_worker->get_buffer(0, 0));
  if (not slot_synchronizer.recv_callback(rf_buffer, last_rx_time.get_ptr(0))) {
    logger.error("SYNC: receiving from radio\n");
  }

  srsran::phy_common_interface::worker_context_t context;
  context.sf_idx     = tti;
  context.worker_ptr = nr_worker;
  context.last       = true; // Set last if standalone
  last_rx_time.add(FDD_HARQ_DELAY_DL_MS * 1e-3);
  context.tx_time.copy(last_rx_time);

  nr_worker->set_context(context);

  // NR worker needs to be launched first, phy_common::worker_end expects first the NR worker and the LTE worker.
  tti_semaphore.push(nr_worker);
  workers.start_worker(nr_worker);

  tti = TTI_ADD(tti, 1);
}

void sync_sa::run_thread()
{
  while (running.load(std::memory_order_relaxed)) {
    logger.set_context(tti);

    logger.debug("SYNC:  state=%s, tti=%d", phy_state.to_string(), tti);

    switch (phy_state.run_state()) {
      case sync_state::IDLE:
        run_state_idle();
        break;
      case sync_state::CELL_SEARCH:
        run_state_cell_search();
        break;
      case sync_state::SFN_SYNC:
        run_state_cell_select();
        break;
      case sync_state::CAMPING:
        run_state_cell_camping();
        break;
    }
      // Advance stack TTI
#ifdef useradio
    slot_synchronizer.run_stack_tti();
#else
    stack->run_tti(tti, 1);
#endif
  }
}
void sync_sa::worker_end(const srsran::phy_common_interface::worker_context_t& w_ctx,
                         const bool&                                           tx_enable,
                         srsran::rf_buffer_t&                                  tx_buffer)
{
  // Wait for the green light to transmit in the current TTI
  tti_semaphore.wait(w_ctx.worker_ptr);

  // Add current time alignment
  srsran::rf_timestamp_t tx_time = w_ctx.tx_time; // get transmit time from the last worker
  // todo: tx_time.sub((double)ta.get_sec());

  // Check if any worker had a transmission
  if (tx_enable) {
    // Actual baseband transmission
    radio->tx(tx_buffer, tx_time);
  } else {
    if (radio->is_continuous_tx()) {
      if (is_pending_tx_end) {
        radio->tx_end();
        is_pending_tx_end = false;
      } else {
        if (!radio->get_is_start_of_burst()) {
          // TODO
          /*
          zeros_multi.set_nof_samples(buffer.get_nof_samples());
          radio->tx(zeros_multi, tx_time);
           */
        }
      }
    } else {
      radio->tx_end();
    }
  }

  // Allow next TTI to transmit
  tti_semaphore.release();
}

} // namespace nr
} // namespace srsue