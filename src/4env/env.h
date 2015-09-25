/*
 * Copyright (C) 2005-2015 Christoph Rupp (chris@crupp.de).
 * All Rights Reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * See the file COPYING for License information.
 */

/*
 * @exception_safe: nothrow
 * @thread_safe: yes
 */

#ifndef HAM_ENV_H
#define HAM_ENV_H

#include "0root/root.h"

#include <map>
#include <string>

#include "ham/hamsterdb_int.h"

// Always verify that a file of level N does not include headers > N!
#include "1base/error.h"
#include "1base/mutex.h"
#include "1base/scoped_ptr.h"
#include "2config/db_config.h"
#include "2config/env_config.h"
#include "4txn/txn.h"
#include "4env/env_test.h"

#ifndef HAM_ROOT_H
#  error "root.h was not included"
#endif

// A helper structure; ham_env_t is declared in ham/hamsterdb.h as an
// opaque C structure, but internally we use a C++ class. The ham_env_t
// struct satisfies the C compiler, and internally we just cast the pointers.
struct ham_env_t {
  int dummy;
};

namespace hamsterdb {

class Database;
class Transaction;

//
// The Environment is the "root" of all hamsterdb objects. It's a container
// for multiple databases and transactions.
//
// This class provides exception handling and locking mechanisms, then
// dispatches all calls to LocalEnvironment or RemoteEnvironment.
//
class Environment
{
  public:
    // Constructor
    Environment(EnvironmentConfiguration &config)
      : m_config(config) {
    }

    virtual ~Environment() {
    }

    // Returns the flags which were set when creating/opening the Environment
    uint32_t get_flags() const {
      return (m_config.flags);
    }

    // Returns the Environment's configuration
    const EnvironmentConfiguration &config() const {
      return (m_config);
    }

    // Returns this Environment's mutex
    Mutex &mutex() {
      return (m_mutex);
    }

    // Creates a new Environment (ham_env_create)
    ham_status_t create();

    // Opens a new Environment (ham_env_open)
    ham_status_t open();

    // Returns all database names (ham_env_get_database_names)
    ham_status_t get_database_names(uint16_t *names, uint32_t *count);

    // Returns environment parameters and flags (ham_env_get_parameters)
    ham_status_t get_parameters(ham_parameter_t *param);

    // Flushes the environment and its databases to disk (ham_env_flush)
    // Accepted flags: HAM_FLUSH_BLOCKING
    ham_status_t flush(uint32_t flags);

    // Creates a new database in the environment (ham_env_create_db)
    ham_status_t create_db(Database **db, DatabaseConfiguration &config,
                    const ham_parameter_t *param);

    // Opens an existing database in the environment (ham_env_open_db)
    ham_status_t open_db(Database **db, DatabaseConfiguration &config,
                    const ham_parameter_t *param);

    // Renames a database in the Environment (ham_env_rename_db)
    ham_status_t rename_db(uint16_t oldname, uint16_t newname, uint32_t flags);

    // Erases (deletes) a database from the Environment (ham_env_erase_db)
    ham_status_t erase_db(uint16_t name, uint32_t flags);

    // Closes an existing database in the environment (ham_db_close)
    ham_status_t close_db(Database *db, uint32_t flags);

    // Begins a new transaction (ham_txn_begin)
    ham_status_t txn_begin(Transaction **ptxn, const char *name,
                    uint32_t flags);

    // Returns the name of a Transaction
    std::string txn_get_name(Transaction *txn);

    // Commits a transaction (ham_txn_commit)
    ham_status_t txn_commit(Transaction *txn, uint32_t flags);

    // Commits a transaction (ham_txn_abort)
    ham_status_t txn_abort(Transaction *txn, uint32_t flags);

    // Closes the Environment (ham_env_close)
    ham_status_t close(uint32_t flags);

    // Fills in the current metrics
    ham_status_t fill_metrics(ham_env_metrics_t *metrics);

    // Returns a test object
    EnvironmentTest test();

  protected:
    // Creates a new Environment (ham_env_create)
    virtual ham_status_t do_create() = 0;

    // Opens a new Environment (ham_env_open)
    virtual ham_status_t do_open() = 0;

    // Returns all database names (ham_env_get_database_names)
    virtual ham_status_t do_get_database_names(uint16_t *names,
                    uint32_t *count) = 0;

    // Returns environment parameters and flags (ham_env_get_parameters)
    virtual ham_status_t do_get_parameters(ham_parameter_t *param) = 0;

    // Flushes the environment and its databases to disk (ham_env_flush)
    virtual ham_status_t do_flush(uint32_t flags) = 0;

    // Creates a new database in the environment (ham_env_create_db)
    virtual ham_status_t do_create_db(Database **db,
                    DatabaseConfiguration &config,
                    const ham_parameter_t *param) = 0;

    // Opens an existing database in the environment (ham_env_open_db)
    virtual ham_status_t do_open_db(Database **db,
                    DatabaseConfiguration &config,
                    const ham_parameter_t *param) = 0;

    // Renames a database in the Environment (ham_env_rename_db)
    virtual ham_status_t do_rename_db(uint16_t oldname, uint16_t newname,
                    uint32_t flags) = 0;

    // Erases (deletes) a database from the Environment (ham_env_erase_db)
    virtual ham_status_t do_erase_db(uint16_t name, uint32_t flags) = 0;

    // Begins a new transaction (ham_txn_begin)
    virtual Transaction *do_txn_begin(const char *name, uint32_t flags) = 0;

    // Commits a transaction (ham_txn_commit)
    virtual ham_status_t do_txn_commit(Transaction *txn, uint32_t flags) = 0;

    // Commits a transaction (ham_txn_abort)
    virtual ham_status_t do_txn_abort(Transaction *txn, uint32_t flags) = 0;

    // Closes the Environment (ham_env_close)
    virtual ham_status_t do_close(uint32_t flags) = 0;

    // Fills in the current metrics
    virtual void do_fill_metrics(ham_env_metrics_t *metrics) const = 0;

  protected:
    // A mutex to serialize access to this Environment
    Mutex m_mutex;

    // The Environment's configuration
    EnvironmentConfiguration m_config;

    // The Transaction manager; can be 0
    ScopedPtr<TransactionManager> m_txn_manager;

    // A map of all opened Databases
    typedef std::map<uint16_t, Database *> DatabaseMap;
    DatabaseMap m_database_map;
};

} // namespace hamsterdb

#endif /* HAM_ENV_H */
