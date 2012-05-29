#include "clustering/immediate_consistency/branch/backfiller.hpp"

#include "clustering/immediate_consistency/branch/history.hpp"
#include "concurrency/fifo_enforcer.hpp"
#include "rpc/semilattice/view.hpp"
#include "stl_utils.hpp"

inline state_timestamp_t get_earliest_timestamp_of_version_range(const version_range_t &vr) {
    return vr.earliest.timestamp;
}

template <class protocol_t>
backfiller_t<protocol_t>::backfiller_t(mailbox_manager_t *mm,
				       boost::shared_ptr<semilattice_read_view_t<branch_history_t<protocol_t> > > bh,
				       store_view_t<protocol_t> *s)
    : mailbox_manager(mm), branch_history(bh),
      store(s),
      backfill_mailbox(mailbox_manager,
		       boost::bind(&backfiller_t::on_backfill, this, _1, _2, _3, _4, _5, auto_drainer_t::lock_t(&drainer))),
      cancel_backfill_mailbox(mailbox_manager,
			      boost::bind(&backfiller_t::on_cancel_backfill, this, _1, auto_drainer_t::lock_t(&drainer))),
      request_progress_mailbox(mailbox_manager,
			       boost::bind(&backfiller_t::request_backfill_progress, this, _1, _2, auto_drainer_t::lock_t(&drainer))) { }

template <class protocol_t>
backfiller_business_card_t<protocol_t> backfiller_t<protocol_t>::get_business_card() {
    return backfiller_business_card_t<protocol_t>(backfill_mailbox.get_address(),
						  cancel_backfill_mailbox.get_address(),
						  request_progress_mailbox.get_address()
						  );
}

template <class protocol_t>
bool backfiller_t<protocol_t>::confirm_and_send_metainfo(typename store_view_t<protocol_t>::metainfo_t metainfo, UNUSED region_map_t<protocol_t, version_range_t> start_point,
							 mailbox_addr_t<void(region_map_t<protocol_t, version_range_t>)> end_point_cont) {
    rassert(metainfo.get_domain() == start_point.get_domain());
    region_map_t<protocol_t, version_range_t> end_point =
	region_map_transform<protocol_t, binary_blob_t, version_range_t>(metainfo,
									 &binary_blob_t::get<version_range_t>
									 );

#ifndef NDEBUG
    /* Confirm that `start_point` is a point in our past */
    typedef region_map_t<protocol_t, version_range_t> version_map_t;

    for (typename version_map_t::const_iterator it = start_point.begin();
	 it != start_point.end();
	 ++it) {
	for (typename version_map_t::const_iterator jt = end_point.begin();
	     jt != end_point.end();
	     ++jt) {
	    typename protocol_t::region_t ixn = region_intersection(it->first, jt->first);
	    if (!region_is_empty(ixn)) {
		version_t start = it->second.latest;
		version_t end = jt->second.earliest;
		rassert(start.timestamp <= end.timestamp);
		rassert(version_is_ancestor(branch_history->get(), start, end, ixn));
	    }
	}
    }
#endif
    /* Transmit `end_point` to the backfillee */
    send(mailbox_manager, end_point_cont, end_point);

    return true;
}

template <class protocol_t>
void send_chunk(mailbox_manager_t *mbox_manager, 
                mailbox_addr_t<void(typename protocol_t::backfill_chunk_t, fifo_enforcer_write_token_t)> chunk_addr,
                const typename protocol_t::backfill_chunk_t &chunk,
                fifo_enforcer_source_t *fifo_src) {
    send(mbox_manager, chunk_addr, chunk, fifo_src->enter_write());
}

template <class protocol_t>
void backfiller_t<protocol_t>::on_backfill(
					   backfill_session_id_t session_id,
					   region_map_t<protocol_t, version_range_t> start_point,
					   mailbox_addr_t<void(region_map_t<protocol_t, version_range_t>)> end_point_cont,
					   mailbox_addr_t<void(typename protocol_t::backfill_chunk_t, fifo_enforcer_write_token_t)> chunk_cont,
					   mailbox_addr_t<void(fifo_enforcer_write_token_t)> done_cont,
					   auto_drainer_t::lock_t keepalive) {

    assert_thread();
    rassert(region_is_superset(store->get_region(), start_point.get_domain()));

    /* Set up a local interruptor cond and put it in the map so that this
       session can be interrupted if the backfillee decides to abort */
    cond_t local_interruptor;
    map_insertion_sentry_t<backfill_session_id_t, cond_t *> be_interruptible(&local_interruptors, session_id, &local_interruptor);

    /* Set up a local progress monitor so people can query us for progress. */
    typename protocol_t::backfill_progress_t local_progress;
    map_insertion_sentry_t<backfill_session_id_t, typename protocol_t::backfill_progress_t *> display_progress(&local_backfill_progress, session_id, &local_progress);

    /* Set up a cond that gets pulsed if we're interrupted by either the
       backfillee stopping or the backfiller destructor being called, but don't
       wait on that cond yet. */
    wait_any_t interrupted(&local_interruptor, keepalive.get_drain_signal());

    try {
        fifo_enforcer_source_t fifo_src;

        boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> send_backfill_token;
        store->new_read_token(send_backfill_token);

        /* Actually perform the backfill */
        store->send_backfill(
                     region_map_transform<protocol_t, version_range_t, state_timestamp_t>(
                                                      start_point,
                                                      &get_earliest_timestamp_of_version_range
                                                      ),
                     boost::bind(&backfiller_t<protocol_t>::confirm_and_send_metainfo, this, _1, start_point, end_point_cont),
                     boost::bind(send_chunk<protocol_t>, mailbox_manager, chunk_cont, _1, &fifo_src),
                     local_backfill_progress[session_id],
                     send_backfill_token,
                     &interrupted
                     );

        /* Send a confirmation */
        send(mailbox_manager, done_cont, fifo_src.enter_write());

    } catch (interrupted_exc_t) {
	/* Ignore. If we were interrupted by the backfillee, then it already
	   knows the backfill is cancelled. If we were interrupted by the
	   backfiller shutting down, it will know when it sees we deconstructed
	   our `resource_advertisement_t`. */
    }
}


template <class protocol_t>
void backfiller_t<protocol_t>::on_cancel_backfill(backfill_session_id_t session_id, UNUSED auto_drainer_t::lock_t) {

    assert_thread();

    typename std::map<backfill_session_id_t, cond_t *>::iterator it =
	local_interruptors.find(session_id);
    if (it != local_interruptors.end()) {
	(*it).second->pulse();
    } else {
	/* The backfill ended on its own right as we were trying to cancel
	   it. Since the backfill was over, we removed the local interruptor
	   from the map, but the cancel message was already in flight. Since
	   there is no backfill to cancel, we just ignore the cancel message.
	*/
    }
}


template <class protocol_t>
void backfiller_t<protocol_t>::request_backfill_progress(backfill_session_id_t session_id,
							 mailbox_addr_t<void(std::pair<int, int>)> response_mbox,
							 auto_drainer_t::lock_t) {
    if (std_contains(local_backfill_progress, session_id) && local_backfill_progress[session_id]) {
	send(mailbox_manager, response_mbox, local_backfill_progress[session_id]->guess_completion());
    } else {
	send(mailbox_manager, response_mbox, std::make_pair(-1, -1));
    }

    //TODO indicate an error has occurred
}


#include "memcached/protocol.hpp"
#include "mock/dummy_protocol.hpp"

template class backfiller_t<mock::dummy_protocol_t>;
template class backfiller_t<memcached_protocol_t>;