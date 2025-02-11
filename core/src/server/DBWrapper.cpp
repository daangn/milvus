// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include "server/DBWrapper.h"

#include <omp.h>
#include <cmath>
#include <string>
#include <vector>

#include <faiss/utils/distances.h>

#include "config/Config.h"
#include "db/DBFactory.h"
#include "index/knowhere/knowhere/index/vector_index/helpers/FaissIO.h"
#include "utils/CommonUtil.h"
#include "utils/Log.h"
#include "utils/StringHelpFunctions.h"

namespace milvus {
namespace server {

Status
DBWrapper::StartService() {
    Config& config = Config::GetInstance();
    Status s;

    // db config
    engine::DBOptions opt;
    s = config.GetGeneralConfigMetaURI(opt.meta_.backend_uri_);
    if (!s.ok()) {
        std::cerr << s.ToString() << std::endl;
        return s;
    }
    config.GetGeneralConfigMetaSslCa(opt.meta_.ssl_ca_);
    config.GetGeneralConfigMetaSslKey(opt.meta_.ssl_key_);
    config.GetGeneralConfigMetaSslCert(opt.meta_.ssl_cert_);

    std::string path;
    s = config.GetStorageConfigPath(path);
    if (!s.ok()) {
        std::cerr << s.ToString() << std::endl;
        return s;
    }
    opt.meta_.path_ = path + "/db";

    s = config.GetStorageConfigAutoFlushInterval(opt.auto_flush_interval_);
    if (!s.ok()) {
        std::cerr << s.ToString() << std::endl;
        return s;
    }

    s = config.GetStorageConfigFileCleanupTimeup(opt.file_cleanup_timeout_);
    if (!s.ok()) {
        std::cerr << s.ToString() << std::endl;
        return s;
    }

    // metric config
    s = config.GetMetricConfigEnableMonitor(opt.metric_enable_);
    if (!s.ok()) {
        std::cerr << s.ToString() << std::endl;
        return s;
    }

    // cache config
    s = config.GetCacheConfigCacheInsertData(opt.insert_cache_immediately_);
    if (!s.ok()) {
        std::cerr << s.ToString() << std::endl;
        return s;
    }

    int64_t insert_buffer_size = 1 * engine::GB;
    s = config.GetCacheConfigInsertBufferSize(insert_buffer_size);
    if (!s.ok()) {
        std::cerr << s.ToString() << std::endl;
        return s;
    }
    opt.insert_buffer_size_ = insert_buffer_size;

    bool cluster_enable = false;
    std::string cluster_role;
    STATUS_CHECK(config.GetClusterConfigEnable(cluster_enable));
    STATUS_CHECK(config.GetClusterConfigRole(cluster_role));
    if (not cluster_enable) {
        opt.mode_ = engine::DBOptions::MODE::SINGLE;
    } else if (cluster_role == "ro") {
        opt.mode_ = engine::DBOptions::MODE::CLUSTER_READONLY;
    } else if (cluster_role == "rw") {
        opt.mode_ = engine::DBOptions::MODE::CLUSTER_WRITABLE;
    } else {
        std::cerr << "Error: cluster.role is not one of rw and ro." << std::endl;
        kill(0, SIGUSR1);
    }

    // get wal configurations
    s = config.GetWalConfigEnable(opt.wal_enable_);
    if (!s.ok()) {
        std::cerr << "ERROR! Failed to get wal_enable configuration." << std::endl;
        std::cerr << s.ToString() << std::endl;
        kill(0, SIGUSR1);
    }

    if (opt.wal_enable_) {
        s = config.GetWalConfigRecoveryErrorIgnore(opt.recovery_error_ignore_);
        if (!s.ok()) {
            std::cerr << "ERROR! Failed to get recovery_error_ignore configuration." << std::endl;
            std::cerr << s.ToString() << std::endl;
            kill(0, SIGUSR1);
        }

        int64_t wal_buffer_size = 0;
        s = config.GetWalConfigBufferSize(wal_buffer_size);
        if (!s.ok()) {
            std::cerr << "ERROR! Failed to get buffer_size configuration." << std::endl;
            std::cerr << s.ToString() << std::endl;
            kill(0, SIGUSR1);
        }
        wal_buffer_size /= (1024 * 1024);
        opt.buffer_size_ = wal_buffer_size;

        s = config.GetWalConfigWalPath(opt.mxlog_path_);
        if (!s.ok()) {
            std::cerr << "ERROR! Failed to get mxlog_path configuration." << std::endl;
            std::cerr << s.ToString() << std::endl;
            kill(0, SIGUSR1);
        }
    }

    // engine config
    int64_t omp_thread;
    s = config.GetEngineConfigOmpThreadNum(omp_thread);
    if (!s.ok()) {
        std::cerr << s.ToString() << std::endl;
        return s;
    }

    if (omp_thread > 0) {
        omp_set_num_threads(omp_thread);
    } else {
        int64_t sys_thread_cnt = 8;
        if (CommonUtil::GetSystemAvailableThreads(sys_thread_cnt)) {
            omp_thread = static_cast<int32_t>(ceil(sys_thread_cnt * 0.5));
            omp_set_num_threads(omp_thread);
        }
    }
    LOG_SERVER_DEBUG_ << "Specify openmp thread number: " << omp_thread;

    // init faiss global variable
    int64_t use_blas_threshold;
    s = config.GetEngineConfigUseBlasThreshold(use_blas_threshold);
    if (!s.ok()) {
        std::cerr << s.ToString() << std::endl;
        return s;
    }
    faiss::distance_compute_blas_threshold = use_blas_threshold;

    // set archive config
    engine::ArchiveConf::CriteriaT criterial;
    int64_t disk, days;
    s = config.GetDBConfigArchiveDiskThreshold(disk);
    if (!s.ok()) {
        std::cerr << s.ToString() << std::endl;
        return s;
    }

    if (disk > 0) {
        criterial[engine::ARCHIVE_CONF_DISK] = disk;
    }

    s = config.GetDBConfigArchiveDaysThreshold(days);
    if (!s.ok()) {
        std::cerr << s.ToString() << std::endl;
        return s;
    }

    if (days > 0) {
        criterial[engine::ARCHIVE_CONF_DAYS] = days;
    }
    opt.meta_.archive_conf_.SetCriterias(criterial);

    // create db root folder
    s = CommonUtil::CreateDirectory(opt.meta_.path_);
    if (!s.ok()) {
        std::cerr << "Error: Failed to create database path: " << path << std::endl;
        kill(0, SIGUSR1);
    }

    // create db instance
    try {
        db_ = engine::DBFactory::Build(opt);
    } catch (std::exception& ex) {
        std::cerr << "Error: Failed to open database: " << ex.what() << std::endl;
        kill(0, SIGUSR1);
    }

    db_->Start();

    // preload collection
    std::string preload_collections;
    s = config.GetCacheConfigPreloadCollection(preload_collections);
    if (!s.ok()) {
        std::cerr << s.ToString() << std::endl;
        return s;
    }

    s = PreloadCollections(preload_collections);
    if (!s.ok()) {
        std::cerr << "ERROR! Failed to preload tables: " << preload_collections << std::endl;
        std::cerr << s.ToString() << std::endl;
        kill(0, SIGUSR1);
    }

    bool trace_enable = false;
    s = config.GetLogsTraceEnable(trace_enable);
    if (s.ok() && trace_enable) {
        knowhere::enable_faiss_logging();
    }

    return Status::OK();
}

Status
DBWrapper::StopService() {
    if (db_) {
        db_->Stop();
    }

    return Status::OK();
}

Status
DBWrapper::PreloadCollections(const std::string& preload_collections) {
    if (preload_collections.empty()) {
        // do nothing
    } else if (preload_collections == "*") {
        // load all tables
        std::vector<engine::meta::CollectionSchema> table_schema_array;
        db_->AllCollections(table_schema_array);

        std::vector<std::string> partition_tags;
        for (auto& schema : table_schema_array) {
            auto status = db_->PreloadCollection(nullptr, schema.collection_id_, partition_tags);
            if (!status.ok()) {
                return status;
            }
        }
    } else {
        std::vector<std::string> collection_names;
        StringHelpFunctions::SplitStringByDelimeter(preload_collections, ",", collection_names);

        std::vector<std::string> partition_tags;
        for (auto& name : collection_names) {
            auto status = db_->PreloadCollection(nullptr, name, partition_tags);
            if (!status.ok()) {
                return status;
            }
        }
    }

    return Status::OK();
}

}  // namespace server
}  // namespace milvus
