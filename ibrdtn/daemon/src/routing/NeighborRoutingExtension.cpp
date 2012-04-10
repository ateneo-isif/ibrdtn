/*
 * NeighborRoutingExtension.cpp
 *
 *  Created on: 16.02.2010
 *      Author: morgenro
 */

#include "config.h"
#include "routing/NeighborRoutingExtension.h"
#include "routing/QueueBundleEvent.h"
#include "core/TimeEvent.h"
#include "net/TransferCompletedEvent.h"
#include "net/TransferAbortedEvent.h"
#include "net/ConnectionEvent.h"
#include "core/NodeEvent.h"
#include "core/Node.h"
#include "net/ConnectionManager.h"
#include "ibrcommon/thread/MutexLock.h"
#include "storage/SimpleBundleStorage.h"
#include "core/BundleEvent.h"
#include <ibrcommon/Logger.h>

#ifdef HAVE_SQLITE
#include "storage/SQLiteBundleStorage.h"
#endif

#include <functional>
#include <list>
#include <algorithm>
#include <typeinfo>
#include <memory>

namespace dtn
{
	namespace routing
	{
		NeighborRoutingExtension::NeighborRoutingExtension()
		{
		}

		NeighborRoutingExtension::~NeighborRoutingExtension()
		{
			stop();
			join();
		}

		void NeighborRoutingExtension::__cancellation()
		{
			_taskqueue.abort();
		}

		void NeighborRoutingExtension::run()
		{
#ifdef HAVE_SQLITE
			class BundleFilter : public dtn::storage::BundleStorage::BundleFilterCallback, public dtn::storage::SQLiteDatabase::SQLBundleQuery
#else
			class BundleFilter : public dtn::storage::BundleStorage::BundleFilterCallback
#endif
			{
			public:
				BundleFilter(const NeighborDatabase::NeighborEntry &entry)
				 : _entry(entry)
				{};

				virtual ~BundleFilter() {};

				virtual size_t limit() const { return 10; };

				virtual bool shouldAdd(const dtn::data::MetaBundle &meta) const
				{
					// check Scope Control Block - do not forward bundles with hop limit == 0
					if (meta.hopcount == 0)
					{
						return false;
					}

					if (meta.get(dtn::data::PrimaryBlock::DESTINATION_IS_SINGLETON))
					{
						// do not forward local bundles
						if (meta.destination.getNode() == dtn::core::BundleCore::local)
						{
							return false;
						}

						// do not forward bundles for other nodes
						if (_entry.eid.getNode() != meta.destination.getNode())
						{
							return false;
						}
					}

					// do not forward bundles already known by the destination
					if (_entry.has(meta))
					{
						return false;
					}

					return true;
				};

#ifdef HAVE_SQLITE
				const std::string getWhere() const
				{
					return "destination LIKE ?";
				};

				size_t bind(sqlite3_stmt *st, size_t offset) const
				{
					const std::string d = _entry.eid.getNode().getString() + "%";
					sqlite3_bind_text(st, offset, d.c_str(), d.size(), SQLITE_TRANSIENT);
					return offset + 1;
				}
#endif

			private:
				const NeighborDatabase::NeighborEntry &_entry;
			};

			dtn::storage::BundleStorage &storage = (**this).getStorage();

			while (true)
			{
				NeighborDatabase &db = (**this).getNeighborDB();

				try {
					Task *t = _taskqueue.getnpop(true);
					std::auto_ptr<Task> killer(t);

					IBRCOMMON_LOGGER_DEBUG(5) << "processing neighbor routing task " << t->toString() << IBRCOMMON_LOGGER_ENDL;

					/**
					 * SearchNextBundleTask triggers a search for a bundle to transfer
					 * to another host. This Task is generated by TransferCompleted, TransferAborted
					 * and node events.
					 */
					try {
						SearchNextBundleTask &task = dynamic_cast<SearchNextBundleTask&>(*t);

						// this destination is not handles by any static route
						ibrcommon::MutexLock l(db);
						NeighborDatabase::NeighborEntry &entry = db.get(task.eid);

						// create a new bundle filter
						BundleFilter filter(entry);

						// query an unknown bundle from the storage, the list contains max. 10 items.
						const std::list<dtn::data::MetaBundle> list = storage.get(filter);

						IBRCOMMON_LOGGER_DEBUG(5) << "got " << list.size() << " items to transfer to " << task.eid.getString() << IBRCOMMON_LOGGER_ENDL;

						// send the bundles as long as we have resources
						for (std::list<dtn::data::MetaBundle>::const_iterator iter = list.begin(); iter != list.end(); iter++)
						{
							try {
								// transfer the bundle to the neighbor
								transferTo(entry, *iter);
							} catch (const NeighborDatabase::AlreadyInTransitException&) { };
						}
					} catch (const NeighborDatabase::NoMoreTransfersAvailable&) {
					} catch (const NeighborDatabase::NeighborNotAvailableException&) {
					} catch (const std::bad_cast&) { };

					/**
					 * process a received bundle
					 */
					try {
						dynamic_cast<ProcessBundleTask&>(*t);

						// new bundles trigger a recheck for all neighbors
						const std::set<dtn::core::Node> nl = dtn::core::BundleCore::getInstance().getNeighbors();

						for (std::set<dtn::core::Node>::const_iterator iter = nl.begin(); iter != nl.end(); iter++)
						{
							const dtn::core::Node &n = (*iter);

							// transfer the next bundle to this destination
							_taskqueue.push( new SearchNextBundleTask( n.getEID() ) );
						}
					} catch (const std::bad_cast&) { };

				} catch (const std::exception &ex) {
					IBRCOMMON_LOGGER_DEBUG(20) << "neighbor routing failed: " << ex.what() << IBRCOMMON_LOGGER_ENDL;
					return;
				}

				yield();
			}
		}

		void NeighborRoutingExtension::notify(const dtn::core::Event *evt)
		{
			try {
				const QueueBundleEvent &queued = dynamic_cast<const QueueBundleEvent&>(*evt);
				_taskqueue.push( new ProcessBundleTask(queued.bundle, queued.origin) );
				return;
			} catch (const std::bad_cast&) { };

			try {
				const dtn::net::TransferCompletedEvent &completed = dynamic_cast<const dtn::net::TransferCompletedEvent&>(*evt);
				const dtn::data::MetaBundle &meta = completed.getBundle();
				const dtn::data::EID &peer = completed.getPeer();

				if ((meta.destination.getNode() == peer.getNode())
						&& (meta.procflags & dtn::data::Bundle::DESTINATION_IS_SINGLETON))
				{
					try {
						dtn::storage::BundleStorage &storage = (**this).getStorage();

						// bundle has been delivered to its destination
						// delete it from our storage
						storage.remove(meta);

						IBRCOMMON_LOGGER(notice) << "singleton bundle delivered and removed: " << meta.toString() << IBRCOMMON_LOGGER_ENDL;

						// gen a report
						dtn::core::BundleEvent::raise(meta, dtn::core::BUNDLE_DELETED, dtn::data::StatusReportBlock::DEPLETED_STORAGE);
					} catch (const dtn::storage::BundleStorage::NoBundleFoundException&) { };

					// transfer the next bundle to this destination
					_taskqueue.push( new SearchNextBundleTask( peer ) );
				}
				return;
			} catch (const std::bad_cast&) { };

			try {
				const dtn::net::TransferAbortedEvent &aborted = dynamic_cast<const dtn::net::TransferAbortedEvent&>(*evt);
				const dtn::data::EID &peer = aborted.getPeer();
				const dtn::data::BundleID &id = aborted.getBundleID();

				switch (aborted.reason)
				{
					case dtn::net::TransferAbortedEvent::REASON_UNDEFINED:
						break;

					case dtn::net::TransferAbortedEvent::REASON_RETRY_LIMIT_REACHED:
						break;

					case dtn::net::TransferAbortedEvent::REASON_BUNDLE_DELETED:
						break;

					case dtn::net::TransferAbortedEvent::REASON_CONNECTION_DOWN:
						return;

					case dtn::net::TransferAbortedEvent::REASON_REFUSED:
					{
						try {
							const dtn::data::MetaBundle meta = (**this).getStorage().get(id);

							// if the bundle has been sent by this module delete it
							if ((meta.destination.getNode() == peer.getNode())
									&& (meta.procflags & dtn::data::Bundle::DESTINATION_IS_SINGLETON))
							{
								// bundle is not deliverable
								(**this).getStorage().remove(id);
							}
						} catch (const dtn::storage::BundleStorage::NoBundleFoundException&) { };
					}
					break;
				}

				// transfer the next bundle to this destination
				_taskqueue.push( new SearchNextBundleTask( peer ) );

				return;
			} catch (const std::bad_cast&) { };

			// If a new neighbor comes available, send him a request for the summary vector
			// If a neighbor went away we can free the stored summary vector
			try {
				const dtn::core::NodeEvent &nodeevent = dynamic_cast<const dtn::core::NodeEvent&>(*evt);
				const dtn::core::Node &n = nodeevent.getNode();

				if (nodeevent.getAction() == NODE_AVAILABLE)
				{
					_taskqueue.push( new SearchNextBundleTask( n.getEID() ) );
				}

				return;
			} catch (const std::bad_cast&) { };

			try {
				const dtn::net::ConnectionEvent &ce = dynamic_cast<const dtn::net::ConnectionEvent&>(*evt);

				if (ce.state == dtn::net::ConnectionEvent::CONNECTION_UP)
				{
					// send all (multi-hop) bundles in the storage to the neighbor
					_taskqueue.push( new SearchNextBundleTask(ce.peer) );
				}
				return;
			} catch (const std::bad_cast&) { };
		}

		/****************************************/

		NeighborRoutingExtension::SearchNextBundleTask::SearchNextBundleTask(const dtn::data::EID &e)
		 : eid(e)
		{ }

		NeighborRoutingExtension::SearchNextBundleTask::~SearchNextBundleTask()
		{ }

		std::string NeighborRoutingExtension::SearchNextBundleTask::toString()
		{
			return "SearchNextBundleTask: " + eid.getString();
		}

		/****************************************/

		NeighborRoutingExtension::ProcessBundleTask::ProcessBundleTask(const dtn::data::MetaBundle &meta, const dtn::data::EID &o)
		 : bundle(meta), origin(o)
		{ }

		NeighborRoutingExtension::ProcessBundleTask::~ProcessBundleTask()
		{ }

		std::string NeighborRoutingExtension::ProcessBundleTask::toString()
		{
			return "ProcessBundleTask: " + bundle.toString();
		}
	}
}
