// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/jobs/manager.hpp"

#include <functional>
#include <iterator>

#include "clustering/administration/reactor_driver.hpp"
#include "concurrency/watchable.hpp"
#include "rdb_protocol/context.hpp"

RDB_IMPL_SERIALIZABLE_1_FOR_CLUSTER(jobs_manager_business_card_t,
                                    get_job_reports_mailbox_address);

const uuid_u jobs_manager_t::base_sindex_id =
    str_to_uuid("74d855a5-0c40-4930-a451-d1ce508ef2d2");

const uuid_u jobs_manager_t::base_disk_compaction_id =
    str_to_uuid("b8766ece-d15c-4f96-bee5-c0edacf10c9c");

jobs_manager_t::jobs_manager_t(mailbox_manager_t* _mailbox_manager) :
    mailbox_manager(_mailbox_manager),
    get_job_reports_mailbox(_mailbox_manager,
                            std::bind(&jobs_manager_t::on_get_job_reports,
                                      this, ph::_1, ph::_2))
    { }

jobs_manager_business_card_t jobs_manager_t::get_business_card() {
    business_card_t business_card;
    business_card.get_job_reports_mailbox_address =
        get_job_reports_mailbox.get_address();
    return business_card;
}

void jobs_manager_t::set_rdb_context(rdb_context_t *_rdb_context) {
    rdb_context = _rdb_context;
}

void jobs_manager_t::set_reactor_driver(reactor_driver_t *_reactor_driver) {
    reactor_driver = _reactor_driver;
}

void jobs_manager_t::on_get_job_reports(
        UNUSED signal_t *interruptor,
        const business_card_t::return_mailbox_t::address_t &reply_address) {
    std::vector<job_report_t> job_reports;

    // Note, as `time` is retrieved here a job may actually report to be started after
    // fetching the time, leading to a negative duration which we round to zero.
    microtime_t time = current_microtime();

    pmap(get_num_threads(), [&](int32_t threadnum) {
        // Here we need to store `job_report_t` locally to prevent multiple threads from
        // inserting into the outer `job_reports`.
        std::vector<job_report_t> job_reports_inner;
        {
            on_thread_t thread((threadnum_t(threadnum)));

            if (rdb_context != nullptr) {
                for (auto const &query : *(rdb_context->get_query_jobs())) {
                    job_reports_inner.emplace_back(
                        "query", query.first, time - std::min(query.second, time));
                }
            }
        }
        job_reports.insert(job_reports.end(),
                           std::make_move_iterator(job_reports_inner.begin()),
                           std::make_move_iterator(job_reports_inner.end()));
    });

    if (reactor_driver != nullptr) {
        for (auto const &sindex_job : reactor_driver->get_sindex_jobs()) {
            uuid_u id = uuid_u::from_hash(
                base_sindex_id,
                uuid_to_str(sindex_job.first.first) + sindex_job.first.second);

            job_reports.emplace_back(
                "sindex", id, time - std::min(sindex_job.second.start_time, time));
        }

        for (auto const &table : reactor_driver->get_tables_gc_active()) {
            uuid_u id = uuid_u::from_hash(base_disk_compaction_id, uuid_to_str(table));

            // `disk_compaction` jobs do not have a duration, it's set to -1 to prevent
            // it being displayed later
            job_reports.emplace_back("disk_compaction", id, -1);
        }
    }

    send(mailbox_manager, reply_address, job_reports);
}
