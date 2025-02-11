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

#include "server/web_impl/handler/WebRequestHandler.h"

#include <algorithm>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include <fiu-local.h>

#include "config/Config.h"
#include "metrics/SystemInfo.h"
#include "server/delivery/request/BaseRequest.h"
#include "server/web_impl/Constants.h"
#include "server/web_impl/Types.h"
#include "server/web_impl/dto/PartitionDto.hpp"
#include "server/web_impl/utils/Util.h"
#include "thirdparty/nlohmann/json.hpp"
#include "utils/StringHelpFunctions.h"
#include "utils/ValidationUtil.h"

namespace milvus {
namespace server {
namespace web {

StatusCode
WebErrorMap(ErrorCode code) {
    static const std::map<ErrorCode, StatusCode> code_map = {
        {SERVER_UNEXPECTED_ERROR, StatusCode::UNEXPECTED_ERROR},
        {SERVER_UNSUPPORTED_ERROR, StatusCode::UNEXPECTED_ERROR},
        {SERVER_NULL_POINTER, StatusCode::UNEXPECTED_ERROR},
        {SERVER_INVALID_ARGUMENT, StatusCode::ILLEGAL_ARGUMENT},
        {SERVER_FILE_NOT_FOUND, StatusCode::FILE_NOT_FOUND},
        {SERVER_NOT_IMPLEMENT, StatusCode::UNEXPECTED_ERROR},
        {SERVER_CANNOT_CREATE_FOLDER, StatusCode::CANNOT_CREATE_FOLDER},
        {SERVER_CANNOT_CREATE_FILE, StatusCode::CANNOT_CREATE_FILE},
        {SERVER_CANNOT_DELETE_FOLDER, StatusCode::CANNOT_DELETE_FOLDER},
        {SERVER_CANNOT_DELETE_FILE, StatusCode::CANNOT_DELETE_FILE},
        {SERVER_COLLECTION_NOT_EXIST, StatusCode::COLLECTION_NOT_EXISTS},
        {SERVER_INVALID_COLLECTION_NAME, StatusCode::ILLEGAL_COLLECTION_NAME},
        {SERVER_INVALID_COLLECTION_DIMENSION, StatusCode::ILLEGAL_DIMENSION},
        {SERVER_INVALID_VECTOR_DIMENSION, StatusCode::ILLEGAL_DIMENSION},

        {SERVER_INVALID_INDEX_TYPE, StatusCode::ILLEGAL_INDEX_TYPE},
        {SERVER_INVALID_ROWRECORD, StatusCode::ILLEGAL_ROWRECORD},
        {SERVER_INVALID_ROWRECORD_ARRAY, StatusCode::ILLEGAL_ROWRECORD},
        {SERVER_INVALID_TOPK, StatusCode::ILLEGAL_TOPK},
        {SERVER_INVALID_NPROBE, StatusCode::ILLEGAL_ARGUMENT},
        {SERVER_INVALID_INDEX_NLIST, StatusCode::ILLEGAL_NLIST},
        {SERVER_INVALID_INDEX_METRIC_TYPE, StatusCode::ILLEGAL_METRIC_TYPE},
        {SERVER_INVALID_INDEX_FILE_SIZE, StatusCode::ILLEGAL_ARGUMENT},
        {SERVER_ILLEGAL_VECTOR_ID, StatusCode::ILLEGAL_VECTOR_ID},
        {SERVER_ILLEGAL_SEARCH_RESULT, StatusCode::ILLEGAL_SEARCH_RESULT},
        {SERVER_CACHE_FULL, StatusCode::CACHE_FAILED},
        {SERVER_BUILD_INDEX_ERROR, StatusCode::BUILD_INDEX_ERROR},
        {SERVER_OUT_OF_MEMORY, StatusCode::OUT_OF_MEMORY},

        {DB_NOT_FOUND, StatusCode::COLLECTION_NOT_EXISTS},
        {DB_META_TRANSACTION_FAILED, StatusCode::META_FAILED},
    };
    if (code < StatusCode::MAX) {
        return StatusCode(code);
    } else if (code_map.find(code) != code_map.end()) {
        return code_map.at(code);
    } else {
        return StatusCode::UNEXPECTED_ERROR;
    }
}

using FloatJson = nlohmann::basic_json<std::map, std::vector, std::string, bool, std::int64_t, std::uint64_t, float>;

/////////////////////////////////// Private methods ///////////////////////////////////////
void
WebRequestHandler::AddStatusToJson(nlohmann::json& json, int64_t code, const std::string& msg) {
    json["code"] = (int64_t)code;
    json["message"] = msg;
}

Status
WebRequestHandler::IsBinaryCollection(const std::string& collection_name, bool& bin) {
    CollectionSchema schema;
    auto status = request_handler_.DescribeCollection(context_ptr_, collection_name, schema);
    if (status.ok()) {
        auto metric = engine::MetricType(schema.metric_type_);
        bin = engine::MetricType::HAMMING == metric || engine::MetricType::JACCARD == metric ||
              engine::MetricType::TANIMOTO == metric || engine::MetricType::SUPERSTRUCTURE == metric ||
              engine::MetricType::SUBSTRUCTURE == metric;
    }

    return status;
}

Status
WebRequestHandler::CopyRecordsFromJson(const nlohmann::json& json, engine::VectorsData& vectors, bool bin) {
    if (!json.is_array()) {
        return Status(ILLEGAL_BODY, "field \"vectors\" must be an array");
    }

    vectors.vector_count_ = json.size();

    if (!bin) {
        for (auto& vec : json) {
            if (!vec.is_array()) {
                return Status(ILLEGAL_BODY, "A vector in field \"vectors\" must be a float array");
            }
            for (auto& data : vec) {
                vectors.float_data_.emplace_back(data.get<float>());
            }
        }
    } else {
        for (auto& vec : json) {
            if (!vec.is_array()) {
                return Status(ILLEGAL_BODY, "A vector in field \"vectors\" must be a float array");
            }
            for (auto& data : vec) {
                vectors.binary_data_.emplace_back(data.get<uint8_t>());
            }
        }
    }

    return Status::OK();
}

///////////////////////// WebRequestHandler methods ///////////////////////////////////////
Status
WebRequestHandler::GetCollectionMetaInfo(const std::string& collection_name, nlohmann::json& json_out) {
    CollectionSchema schema;
    auto status = request_handler_.DescribeCollection(context_ptr_, collection_name, schema);
    if (!status.ok()) {
        return status;
    }

    int64_t count;
    status = request_handler_.CountCollection(context_ptr_, collection_name, count);
    if (!status.ok()) {
        return status;
    }

    IndexParam index_param;
    status = request_handler_.DescribeIndex(context_ptr_, collection_name, index_param);
    if (!status.ok()) {
        return status;
    }

    json_out["collection_name"] = schema.collection_name_;
    json_out["dimension"] = schema.dimension_;
    json_out["index_file_size"] = schema.index_file_size_;
    json_out["index"] = IndexMap.at(engine::EngineType(index_param.index_type_));
    json_out["index_params"] = index_param.extra_params_;
    json_out["metric_type"] = MetricMap.at(engine::MetricType(schema.metric_type_));
    json_out["count"] = count;

    return Status::OK();
}

Status
WebRequestHandler::GetCollectionStat(const std::string& collection_name, nlohmann::json& json_out) {
    std::string collection_info;
    auto status = request_handler_.ShowCollectionInfo(context_ptr_, collection_name, collection_info);

    if (status.ok()) {
        try {
            json_out = nlohmann::json::parse(collection_info);
        } catch (std::exception& e) {
            return Status(SERVER_UNEXPECTED_ERROR,
                          "Error occurred when parsing collection stat information: " + std::string(e.what()));
        }
    }

    return status;
}

Status
WebRequestHandler::GetSegmentVectors(const std::string& collection_name, const std::string& segment_name,
                                     int64_t page_size, int64_t offset, nlohmann::json& json_out) {
    std::vector<int64_t> vector_ids;
    auto status = request_handler_.GetVectorIDs(context_ptr_, collection_name, segment_name, vector_ids);
    if (!status.ok()) {
        return status;
    }

    auto ids_begin = std::min(vector_ids.size(), (size_t)offset);
    auto ids_end = std::min(vector_ids.size(), (size_t)(offset + page_size));

    auto ids = std::vector<int64_t>(vector_ids.begin() + ids_begin, vector_ids.begin() + ids_end);
    nlohmann::json vectors_json;
    status = GetVectorsByIDs(collection_name, "", ids, vectors_json);

    nlohmann::json result_json;
    if (vectors_json.empty()) {
        json_out["vectors"] = std::vector<int64_t>();
    } else {
        json_out["vectors"] = vectors_json;
    }
    json_out["count"] = vector_ids.size();

    AddStatusToJson(json_out, status.code(), status.message());

    return Status::OK();
}

Status
WebRequestHandler::GetSegmentIds(const std::string& collection_name, const std::string& segment_name, int64_t page_size,
                                 int64_t offset, nlohmann::json& json_out) {
    std::vector<int64_t> vector_ids;
    auto status = request_handler_.GetVectorIDs(context_ptr_, collection_name, segment_name, vector_ids);
    if (status.ok()) {
        auto ids_begin = std::min(vector_ids.size(), (size_t)offset);
        auto ids_end = std::min(vector_ids.size(), (size_t)(offset + page_size));

        if (ids_begin >= ids_end) {
            json_out["ids"] = std::vector<int64_t>();
        } else {
            for (size_t i = ids_begin; i < ids_end; i++) {
                json_out["ids"].push_back(std::to_string(vector_ids.at(i)));
            }
        }
        json_out["count"] = vector_ids.size();
    }

    return status;
}

Status
WebRequestHandler::CommandLine(const std::string& cmd, std::string& reply) {
    return request_handler_.Cmd(context_ptr_, cmd, reply);
}

Status
WebRequestHandler::Cmd(const std::string& cmd, std::string& result_str) {
    std::string reply;
    auto status = CommandLine(cmd, reply);

    if (status.ok()) {
        nlohmann::json result;
        AddStatusToJson(result, status.code(), status.message());
        result["reply"] = reply;
        result_str = result.dump();
    }

    return status;
}

Status
WebRequestHandler::PreLoadCollection(const nlohmann::json& json, std::string& result_str) {
    if (!json.contains("collection_name")) {
        return Status(BODY_FIELD_LOSS, "Field \"load\" must contains collection_name");
    }

    auto collection_name = json["collection_name"];
    std::vector<std::string> partition_tags;
    if (json.contains("partition_tags")) {
        auto tags = json["partition_tags"];
        if (!tags.is_null() && !tags.is_array()) {
            return Status(BODY_PARSE_FAIL, "Field \"partition_tags\" must be an array");
        }

        for (auto& tag : tags) {
            partition_tags.emplace_back(tag.get<std::string>());
        }
    }

    auto status = request_handler_.PreloadCollection(context_ptr_, collection_name.get<std::string>(), partition_tags);
    if (status.ok()) {
        nlohmann::json result;
        AddStatusToJson(result, status.code(), status.message());
        result_str = result.dump();
    }

    return status;
}

Status
WebRequestHandler::ReleaseCollection(const nlohmann::json& json, std::string& result_str) {
    if (!json.contains("collection_name")) {
        return Status(BODY_FIELD_LOSS, "Field \"load\" must contains collection_name");
    }

    auto collection_name = json["collection_name"];
    std::vector<std::string> partition_tags;
    if (json.contains("partition_tags")) {
        auto tags = json["partition_tags"];
        if (!tags.is_null() && !tags.is_array()) {
            return Status(BODY_PARSE_FAIL, "Field \"partition_tags\" must be an array");
        }

        for (auto& tag : tags) {
            partition_tags.emplace_back(tag.get<std::string>());
        }
    }

    auto status = request_handler_.ReleaseCollection(context_ptr_, collection_name.get<std::string>(), partition_tags);
    if (status.ok()) {
        nlohmann::json result;
        AddStatusToJson(result, status.code(), status.message());
        result_str = result.dump();
    }

    return status;
}

Status
WebRequestHandler::Flush(const nlohmann::json& json, std::string& result_str) {
    if (!json.contains("collection_names")) {
        return Status(BODY_FIELD_LOSS, "Field \"flush\" must contains collection_names");
    }

    auto collection_names = json["collection_names"];
    if (!collection_names.is_array()) {
        return Status(BODY_FIELD_LOSS, "Field \"collection_names\" must be and array");
    }

    std::vector<std::string> names;
    for (auto& name : collection_names) {
        names.emplace_back(name.get<std::string>());
    }

    auto status = request_handler_.Flush(context_ptr_, names);
    if (status.ok()) {
        nlohmann::json result;
        AddStatusToJson(result, status.code(), status.message());
        result_str = result.dump();
    }

    return status;
}

Status
WebRequestHandler::Compact(const nlohmann::json& json, std::string& result_str) {
    if (!json.contains("collection_name")) {
        return Status(BODY_FIELD_LOSS, "Field \"compact\" must contains collection_names");
    }

    auto collection_name = json["collection_name"];
    if (!collection_name.is_string()) {
        return Status(BODY_FIELD_LOSS, "Field \"collection_names\" must be a string");
    }

    auto name = collection_name.get<std::string>();

    double compact_threshold = 0.1;  // compact trigger threshold: delete_counts/segment_counts
    auto status = request_handler_.Compact(context_ptr_, name, compact_threshold);

    if (status.ok()) {
        nlohmann::json result;
        AddStatusToJson(result, status.code(), status.message());
        result_str = result.dump();
    }

    return status;
}

Status
WebRequestHandler::GetConfig(std::string& result_str) {
    std::string cmd = "get_config *";
    std::string reply;
    auto status = CommandLine(cmd, reply);
    if (status.ok()) {
        nlohmann::json j = nlohmann::json::parse(reply);
#ifdef MILVUS_GPU_VERSION
        if (j.contains("gpu_resource_config")) {
            std::vector<std::string> gpus;
            if (j["gpu_resource_config"].contains("search_resources")) {
                auto gpu_search_res = j["gpu_resource_config"]["search_resources"].get<std::string>();
                StringHelpFunctions::SplitStringByDelimeter(gpu_search_res, ",", gpus);
                j["gpu_resource_config"]["search_resources"] = gpus;
            }
            if (j["gpu_resource_config"].contains("build_index_resources")) {
                auto gpu_build_res = j["gpu_resource_config"]["build_index_resources"].get<std::string>();
                gpus.clear();
                StringHelpFunctions::SplitStringByDelimeter(gpu_build_res, ",", gpus);
                j["gpu_resource_config"]["build_index_resources"] = gpus;
            }
        }
#endif
        // check if server require start
        Config& config = Config::GetInstance();
        bool required = false;
        config.GetServerRestartRequired(required);
        j["restart_required"] = required;
        result_str = j.dump();
    }

    return Status::OK();
}

Status
WebRequestHandler::SetConfig(const nlohmann::json& json, std::string& result_str) {
    if (!json.is_object()) {
        return Status(ILLEGAL_BODY, "Payload must be a map");
    }

    std::vector<std::string> cmds;
    for (auto& el : json.items()) {
        auto evalue = el.value();
        if (!evalue.is_object()) {
            return Status(ILLEGAL_BODY, "Invalid payload format, the root value must be json map");
        }

        for (auto& iel : el.value().items()) {
            auto ievalue = iel.value();
            if (!(ievalue.is_string() || ievalue.is_number() || ievalue.is_boolean())) {
                return Status(ILLEGAL_BODY, "Config value must be one of string, numeric or boolean");
            }
            std::ostringstream ss;
            if (ievalue.is_string()) {
                std::string vle = ievalue;
                ss << "set_config " << el.key() << "." << iel.key() << " " << vle;
            } else {
                ss << "set_config " << el.key() << "." << iel.key() << " " << ievalue;
            }
            cmds.emplace_back(ss.str());
        }
    }

    std::string msg;

    for (auto& c : cmds) {
        std::string reply;
        auto status = CommandLine(c, reply);
        if (!status.ok()) {
            return status;
        }
        msg += c + " successfully;";
    }

    nlohmann::json result;
    AddStatusToJson(result, StatusCode::SUCCESS, msg);

    bool required = false;
    Config& config = Config::GetInstance();
    config.GetServerRestartRequired(required);
    result["restart_required"] = required;

    result_str = result.dump();

    return Status::OK();
}

Status
WebRequestHandler::Search(const std::string& collection_name, const nlohmann::json& json, std::string& result_str) {
    if (!json.contains("topk")) {
        return Status(BODY_FIELD_LOSS, "Field \'topk\' is required");
    }
    int64_t topk = json["topk"];

    if (!json.contains("params")) {
        return Status(BODY_FIELD_LOSS, "Field \'params\' is required");
    }

    std::vector<std::string> partition_tags;
    if (json.contains("partition_tags")) {
        auto tags = json["partition_tags"];
        if (!tags.is_null() && !tags.is_array()) {
            return Status(BODY_PARSE_FAIL, "Field \"partition_tags\" must be an array");
        }

        for (auto& tag : tags) {
            partition_tags.emplace_back(tag.get<std::string>());
        }
    }

    TopKQueryResult result;
    Status status;
    if (json.contains("ids")) {
        auto vec_ids = json["ids"];
        if (!vec_ids.is_array()) {
            return Status(BODY_PARSE_FAIL, "Field \"ids\" must be ad array");
        }

        std::vector<int64_t> id_array;
        for (auto& id_str : vec_ids) {
            id_array.emplace_back(std::stol(id_str.get<std::string>()));
        }
        //        std::vector<int64_t> id_array(vec_ids.begin(), vec_ids.end());
        status = request_handler_.SearchByID(context_ptr_, collection_name, id_array, topk, json["params"],
                                             partition_tags, result);
    } else {
        std::vector<std::string> file_id_vec;
        if (json.contains("file_ids")) {
            auto ids = json["file_ids"];
            if (!ids.is_null() && !ids.is_array()) {
                return Status(BODY_PARSE_FAIL, "Field \"file_ids\" must be an array");
            }
            for (auto& id : ids) {
                file_id_vec.emplace_back(id.get<std::string>());
            }
        }

        bool bin_flag = false;
        status = IsBinaryCollection(collection_name, bin_flag);
        if (!status.ok()) {
            return status;
        }

        if (!json.contains("vectors")) {
            return Status(BODY_FIELD_LOSS, "Field \"vectors\" is required");
        }

        engine::VectorsData vectors_data;
        status = CopyRecordsFromJson(json["vectors"], vectors_data, bin_flag);
        if (!status.ok()) {
            return status;
        }

        status = request_handler_.Search(context_ptr_, collection_name, vectors_data, topk, json["params"],
                                         partition_tags, file_id_vec, result);
    }
    if (!status.ok()) {
        return status;
    }

    nlohmann::json result_json;
    result_json["num"] = result.row_num_;
    if (result.row_num_ == 0) {
        result_json["result"] = std::vector<int64_t>();
        result_str = result_json.dump();
        return Status::OK();
    }

    auto step = result.id_list_.size() / result.row_num_;
    nlohmann::json search_result_json;
    for (int64_t i = 0; i < result.row_num_; i++) {
        nlohmann::json raw_result_json;
        for (size_t j = 0; j < step; j++) {
            auto id = result.id_list_.at(i * step + j);
            if (id < 0) {
                continue;
            }
            nlohmann::json one_result_json;
            one_result_json["id"] = std::to_string(id);
            one_result_json["distance"] = std::to_string(result.distance_list_.at(i * step + j));
            raw_result_json.emplace_back(one_result_json);
        }
        search_result_json.emplace_back(raw_result_json);
    }
    result_json["result"] = search_result_json;
    result_str = result_json.dump();

    return Status::OK();
}

Status
WebRequestHandler::DeleteByIDs(const std::string& collection_name, const nlohmann::json& json,
                               std::string& result_str) {
    std::vector<int64_t> vector_ids;
    if (!json.contains("ids")) {
        return Status(BODY_FIELD_LOSS, "Field \"delete\" must contains \"ids\"");
    }
    auto ids = json["ids"];
    if (!ids.is_array()) {
        return Status(BODY_FIELD_LOSS, "\"ids\" must be an array");
    }

    for (auto& id : ids) {
        auto id_str = id.get<std::string>();
        if (!ValidationUtil::ValidateStringIsNumber(id_str).ok()) {
            return Status(ILLEGAL_BODY, "Members in \"ids\" must be integer string");
        }
        vector_ids.emplace_back(std::stol(id_str));
    }

    std::string partition_tag = "";
    if (json.contains("partition_tag")) {
        partition_tag = json["partition_tag"];
    }

    auto status = request_handler_.DeleteByID(context_ptr_, collection_name, partition_tag, vector_ids);

    nlohmann::json result_json;
    AddStatusToJson(result_json, status.code(), status.message());
    result_str = result_json.dump();

    return status;
}

Status
WebRequestHandler::GetVectorsByIDs(const std::string& collection_name, const std::string& partition_tag,
                                   const std::vector<int64_t>& ids, nlohmann::json& json_out) {
    std::vector<engine::VectorsData> vector_batch;
    auto status = request_handler_.GetVectorsByID(context_ptr_, collection_name, partition_tag, ids, vector_batch);
    if (!status.ok()) {
        return status;
    }

    bool bin;
    status = IsBinaryCollection(collection_name, bin);
    if (!status.ok()) {
        return status;
    }

    nlohmann::json vectors_json;
    for (size_t i = 0; i < vector_batch.size(); i++) {
        nlohmann::json vector_json;
        if (bin) {
            vector_json["vector"] = vector_batch.at(i).binary_data_;
        } else {
            vector_json["vector"] = vector_batch.at(i).float_data_;
        }
        vector_json["id"] = std::to_string(ids[i]);
        json_out.push_back(vector_json);
    }

    return Status::OK();
}

////////////////////////////////// Router methods ////////////////////////////////////////////
StatusDto::ObjectWrapper
WebRequestHandler::GetDevices(DevicesDto::ObjectWrapper& devices_dto) {
    auto system_info = SystemInfo::GetInstance();

    devices_dto->cpu = devices_dto->cpu->createShared();
    devices_dto->cpu->memory = system_info.GetPhysicalMemory() >> 30;

    devices_dto->gpus = devices_dto->gpus->createShared();

#ifdef MILVUS_GPU_VERSION
    size_t count = system_info.num_device();
    std::vector<int64_t> device_mems = system_info.GPUMemoryTotal();

    if (count != device_mems.size()) {
        RETURN_STATUS_DTO(UNEXPECTED_ERROR, "Can't obtain GPU info");
    }

    for (size_t i = 0; i < count; i++) {
        auto device_dto = DeviceInfoDto::createShared();
        device_dto->memory = device_mems.at(i) >> 30;
        devices_dto->gpus->put("GPU" + OString(std::to_string(i).c_str()), device_dto);
    }
#endif

    ASSIGN_RETURN_STATUS_DTO(Status::OK());
}

StatusDto::ObjectWrapper
WebRequestHandler::GetAdvancedConfig(AdvancedConfigDto::ObjectWrapper& advanced_config) {
    std::string reply;
    std::string cache_cmd_prefix = "get_config " + std::string(CONFIG_CACHE) + ".";

    std::string cache_cmd_string = cache_cmd_prefix + std::string(CONFIG_CACHE_CPU_CACHE_CAPACITY);
    auto status = CommandLine(cache_cmd_string, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status)
    }
    advanced_config->cpu_cache_capacity = std::stol(reply);

    cache_cmd_string = cache_cmd_prefix + std::string(CONFIG_CACHE_CACHE_INSERT_DATA);
    CommandLine(cache_cmd_string, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status)
    }
    advanced_config->cache_insert_data = ("1" == reply || "true" == reply);

    auto engine_cmd_prefix = "get_config " + std::string(CONFIG_ENGINE) + ".";
    auto engine_cmd_string = engine_cmd_prefix + std::string(CONFIG_ENGINE_USE_BLAS_THRESHOLD);
    CommandLine(engine_cmd_string, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status)
    }
    advanced_config->use_blas_threshold = std::stol(reply);

#ifdef MILVUS_GPU_VERSION
    engine_cmd_string = engine_cmd_prefix + std::string(CONFIG_GPU_RESOURCE_GPU_SEARCH_THRESHOLD);
    CommandLine(engine_cmd_string, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status)
    }
    advanced_config->gpu_search_threshold = std::stol(reply);
#endif

    ASSIGN_RETURN_STATUS_DTO(status)
}

StatusDto::ObjectWrapper
WebRequestHandler::SetAdvancedConfig(const AdvancedConfigDto::ObjectWrapper& advanced_config) {
    if (nullptr == advanced_config->cpu_cache_capacity.get()) {
        RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Field \'cpu_cache_capacity\' miss.");
    }

    if (nullptr == advanced_config->cache_insert_data.get()) {
        RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Field \'cache_insert_data\' miss.");
    }

    if (nullptr == advanced_config->use_blas_threshold.get()) {
        RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Field \'use_blas_threshold\' miss.");
    }

#ifdef MILVUS_GPU_VERSION
    if (nullptr == advanced_config->gpu_search_threshold.get()) {
        RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Field \'gpu_search_threshold\' miss.");
    }
#endif

    std::string reply;
    std::string cache_cmd_prefix = "set_config " + std::string(CONFIG_CACHE) + ".";

    std::string cache_cmd_string = cache_cmd_prefix + std::string(CONFIG_CACHE_CPU_CACHE_CAPACITY) + " " +
                                   std::to_string(advanced_config->cpu_cache_capacity->getValue());
    auto status = CommandLine(cache_cmd_string, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status)
    }

    cache_cmd_string = cache_cmd_prefix + std::string(CONFIG_CACHE_CACHE_INSERT_DATA) + " " +
                       std::to_string(advanced_config->cache_insert_data->getValue());
    status = CommandLine(cache_cmd_string, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status)
    }

    auto engine_cmd_prefix = "set_config " + std::string(CONFIG_ENGINE) + ".";

    auto engine_cmd_string = engine_cmd_prefix + std::string(CONFIG_ENGINE_USE_BLAS_THRESHOLD) + " " +
                             std::to_string(advanced_config->use_blas_threshold->getValue());
    status = CommandLine(engine_cmd_string, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status)
    }

#ifdef MILVUS_GPU_VERSION
    auto gpu_cmd_prefix = "set_config " + std::string(CONFIG_GPU_RESOURCE) + ".";
    auto gpu_cmd_string = gpu_cmd_prefix + std::string(CONFIG_GPU_RESOURCE_GPU_SEARCH_THRESHOLD) + " " +
                          std::to_string(advanced_config->gpu_search_threshold->getValue());
    status = CommandLine(gpu_cmd_string, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status)
    }
#endif

    ASSIGN_RETURN_STATUS_DTO(status)
}

#ifdef MILVUS_GPU_VERSION
StatusDto::ObjectWrapper
WebRequestHandler::GetGpuConfig(GPUConfigDto::ObjectWrapper& gpu_config_dto) {
    std::string reply;
    std::string gpu_cmd_prefix = "get_config " + std::string(CONFIG_GPU_RESOURCE) + ".";

    std::string gpu_cmd_request = gpu_cmd_prefix + std::string(CONFIG_GPU_RESOURCE_ENABLE);
    auto status = CommandLine(gpu_cmd_request, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status);
    }
    gpu_config_dto->enable = reply == "1" || reply == "true";

    if (!gpu_config_dto->enable->getValue()) {
        ASSIGN_RETURN_STATUS_DTO(Status::OK());
    }

    gpu_cmd_request = gpu_cmd_prefix + std::string(CONFIG_GPU_RESOURCE_CACHE_CAPACITY);
    status = CommandLine(gpu_cmd_request, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status);
    }
    gpu_config_dto->cache_capacity = std::stol(reply);

    gpu_cmd_request = gpu_cmd_prefix + std::string(CONFIG_GPU_RESOURCE_SEARCH_RESOURCES);
    status = CommandLine(gpu_cmd_request, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status);
    }

    std::vector<std::string> gpu_entry;
    StringHelpFunctions::SplitStringByDelimeter(reply, ",", gpu_entry);

    gpu_config_dto->search_resources = gpu_config_dto->search_resources->createShared();
    for (auto& device_id : gpu_entry) {
        gpu_config_dto->search_resources->pushBack(OString(device_id.c_str())->toUpperCase());
    }
    gpu_entry.clear();

    gpu_cmd_request = gpu_cmd_prefix + std::string(CONFIG_GPU_RESOURCE_BUILD_INDEX_RESOURCES);
    status = CommandLine(gpu_cmd_request, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status);
    }

    StringHelpFunctions::SplitStringByDelimeter(reply, ",", gpu_entry);
    gpu_config_dto->build_index_resources = gpu_config_dto->build_index_resources->createShared();
    for (auto& device_id : gpu_entry) {
        gpu_config_dto->build_index_resources->pushBack(OString(device_id.c_str())->toUpperCase());
    }

    ASSIGN_RETURN_STATUS_DTO(Status::OK());
}

StatusDto::ObjectWrapper
WebRequestHandler::SetGpuConfig(const GPUConfigDto::ObjectWrapper& gpu_config_dto) {
    // Step 1: Check config param
    if (nullptr == gpu_config_dto->enable.get()) {
        RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Field \'enable\' miss")
    }

    if (nullptr == gpu_config_dto->cache_capacity.get()) {
        RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Field \'cache_capacity\' miss")
    }

    if (nullptr == gpu_config_dto->search_resources.get()) {
        gpu_config_dto->search_resources = gpu_config_dto->search_resources->createShared();
        gpu_config_dto->search_resources->pushBack("GPU0");
    }

    if (nullptr == gpu_config_dto->build_index_resources.get()) {
        gpu_config_dto->build_index_resources = gpu_config_dto->build_index_resources->createShared();
        gpu_config_dto->build_index_resources->pushBack("GPU0");
    }

    // Step 2: Set config
    std::string reply;
    std::string gpu_cmd_prefix = "set_config " + std::string(CONFIG_GPU_RESOURCE) + ".";
    std::string gpu_cmd_request = gpu_cmd_prefix + std::string(CONFIG_GPU_RESOURCE_ENABLE) + " " +
                                  std::to_string(gpu_config_dto->enable->getValue());
    auto status = CommandLine(gpu_cmd_request, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status);
    }

    if (!gpu_config_dto->enable->getValue()) {
        RETURN_STATUS_DTO(SUCCESS, "Set Gpu resources to false");
    }

    gpu_cmd_request = gpu_cmd_prefix + std::string(CONFIG_GPU_RESOURCE_CACHE_CAPACITY) + " " +
                      std::to_string(gpu_config_dto->cache_capacity->getValue());
    status = CommandLine(gpu_cmd_request, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status);
    }

    std::vector<std::string> search_resources;
    gpu_config_dto->search_resources->forEach(
        [&search_resources](const OString& res) { search_resources.emplace_back(res->toLowerCase()->std_str()); });

    std::string search_resources_value;
    for (auto& res : search_resources) {
        search_resources_value += res + ",";
    }
    auto len = search_resources_value.size();
    if (len > 0) {
        search_resources_value.erase(len - 1);
    }

    gpu_cmd_request = gpu_cmd_prefix + std::string(CONFIG_GPU_RESOURCE_SEARCH_RESOURCES) + " " + search_resources_value;
    status = CommandLine(gpu_cmd_request, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status);
    }

    std::vector<std::string> build_resources;
    gpu_config_dto->build_index_resources->forEach(
        [&build_resources](const OString& res) { build_resources.emplace_back(res->toLowerCase()->std_str()); });

    std::string build_resources_value;
    for (auto& res : build_resources) {
        build_resources_value += res + ",";
    }
    len = build_resources_value.size();
    if (len > 0) {
        build_resources_value.erase(len - 1);
    }

    gpu_cmd_request =
        gpu_cmd_prefix + std::string(CONFIG_GPU_RESOURCE_BUILD_INDEX_RESOURCES) + " " + build_resources_value;
    status = CommandLine(gpu_cmd_request, reply);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status);
    }

    ASSIGN_RETURN_STATUS_DTO(Status::OK());
}
#endif

/*************
 *
 * Collection {
 */
StatusDto::ObjectWrapper
WebRequestHandler::CreateCollection(const CollectionRequestDto::ObjectWrapper& collection_schema) {
    if (nullptr == collection_schema->collection_name.get()) {
        RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Field \'collection_name\' is missing")
    }

    if (nullptr == collection_schema->dimension.get()) {
        RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Field \'dimension\' is missing")
    }

    if (nullptr == collection_schema->index_file_size.get()) {
        RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Field \'index_file_size\' is missing")
    }

    if (nullptr == collection_schema->metric_type.get()) {
        RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Field \'metric_type\' is missing")
    }

    if (MetricNameMap.find(collection_schema->metric_type->std_str()) == MetricNameMap.end()) {
        RETURN_STATUS_DTO(ILLEGAL_METRIC_TYPE, "metric_type is illegal")
    }

    auto status = request_handler_.CreateCollection(
        context_ptr_, collection_schema->collection_name->std_str(), collection_schema->dimension,
        collection_schema->index_file_size,
        static_cast<int64_t>(MetricNameMap.at(collection_schema->metric_type->std_str())));

    ASSIGN_RETURN_STATUS_DTO(status)
}

StatusDto::ObjectWrapper
WebRequestHandler::ShowCollections(const OQueryParams& query_params, OString& result) {
    int64_t offset = 0;
    auto status = ParseQueryInteger(query_params, "offset", offset);
    if (!status.ok()) {
        RETURN_STATUS_DTO(status.code(), status.message().c_str());
    }

    int64_t page_size = 10;
    status = ParseQueryInteger(query_params, "page_size", page_size);
    if (!status.ok()) {
        RETURN_STATUS_DTO(status.code(), status.message().c_str());
    }

    if (offset < 0 || page_size < 0) {
        RETURN_STATUS_DTO(ILLEGAL_QUERY_PARAM, "Query param 'offset' or 'page_size' should equal or bigger than 0");
    }

    bool all_required = false;
    ParseQueryBool(query_params, "all_required", all_required);
    if (!status.ok()) {
        RETURN_STATUS_DTO(status.code(), status.message().c_str());
    }

    std::vector<std::string> collections;
    status = request_handler_.ShowCollections(context_ptr_, collections);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status)
    }

    if (all_required) {
        offset = 0;
        page_size = collections.size();
    } else {
        offset = std::min((size_t)offset, collections.size());
        page_size = std::min(collections.size() - offset, (size_t)page_size);
    }

    nlohmann::json collections_json;
    for (int64_t i = offset; i < page_size + offset; i++) {
        nlohmann::json collection_json;
        status = GetCollectionMetaInfo(collections.at(i), collection_json);
        if (!status.ok()) {
            ASSIGN_RETURN_STATUS_DTO(status)
        }
        collections_json.push_back(collection_json);
    }

    nlohmann::json result_json;
    result_json["count"] = collections.size();
    if (collections_json.empty()) {
        result_json["collections"] = std::vector<int64_t>();
    } else {
        result_json["collections"] = collections_json;
    }

    result = result_json.dump().c_str();

    ASSIGN_RETURN_STATUS_DTO(status)
}

StatusDto::ObjectWrapper
WebRequestHandler::GetCollection(const OString& collection_name, const OQueryParams& query_params, OString& result) {
    if (nullptr == collection_name.get()) {
        RETURN_STATUS_DTO(PATH_PARAM_LOSS, "Path param \'collection_name\' is required!");
    }

    std::string stat;
    auto status = ParseQueryStr(query_params, "info", stat);
    if (!status.ok()) {
        RETURN_STATUS_DTO(status.code(), status.message().c_str());
    }

    if (!stat.empty() && stat == "stat") {
        nlohmann::json json;
        status = GetCollectionStat(collection_name->std_str(), json);
        result = status.ok() ? json.dump().c_str() : "NULL";
    } else {
        nlohmann::json json;
        status = GetCollectionMetaInfo(collection_name->std_str(), json);
        result = status.ok() ? json.dump().c_str() : "NULL";
    }

    ASSIGN_RETURN_STATUS_DTO(status);
}

StatusDto::ObjectWrapper
WebRequestHandler::DropCollection(const OString& collection_name) {
    auto status = request_handler_.DropCollection(context_ptr_, collection_name->std_str());

    ASSIGN_RETURN_STATUS_DTO(status)
}

/***********
 *
 * Index {
 */

StatusDto::ObjectWrapper
WebRequestHandler::CreateIndex(const OString& collection_name, const OString& body) {
    try {
        auto request_json = nlohmann::json::parse(body->std_str());
        if (!request_json.contains("index_type")) {
            RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Field \'index_type\' is required");
        }

        std::string index_type = request_json["index_type"];
        if (IndexNameMap.find(index_type) == IndexNameMap.end()) {
            RETURN_STATUS_DTO(ILLEGAL_INDEX_TYPE, "The index type is invalid.")
        }
        auto index = static_cast<int64_t>(IndexNameMap.at(index_type));
        if (!request_json.contains("params")) {
            RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Field \'params\' is required")
        }
        auto status =
            request_handler_.CreateIndex(context_ptr_, collection_name->std_str(), index, request_json["params"]);
        ASSIGN_RETURN_STATUS_DTO(status);
    } catch (nlohmann::detail::parse_error& e) {
        RETURN_STATUS_DTO(BODY_PARSE_FAIL, e.what())
    } catch (nlohmann::detail::type_error& e) {
        RETURN_STATUS_DTO(BODY_PARSE_FAIL, e.what())
    }

    ASSIGN_RETURN_STATUS_DTO(Status::OK())
}

StatusDto::ObjectWrapper
WebRequestHandler::GetIndex(const OString& collection_name, OString& result) {
    IndexParam param;
    auto status = request_handler_.DescribeIndex(context_ptr_, collection_name->std_str(), param);

    if (status.ok()) {
        nlohmann::json json_out;
        auto index_type = IndexMap.at(engine::EngineType(param.index_type_));
        json_out["index_type"] = index_type;
        json_out["params"] = nlohmann::json::parse(param.extra_params_);
        result = json_out.dump().c_str();
    }

    ASSIGN_RETURN_STATUS_DTO(status)
}

StatusDto::ObjectWrapper
WebRequestHandler::DropIndex(const OString& collection_name) {
    auto status = request_handler_.DropIndex(context_ptr_, collection_name->std_str());

    ASSIGN_RETURN_STATUS_DTO(status)
}

StatusDto::ObjectWrapper
WebRequestHandler::CreatePartition(const OString& collection_name, const PartitionRequestDto::ObjectWrapper& param) {
    if (nullptr == param->partition_tag.get()) {
        RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Field \'partition_tag\' is required")
    }

    auto status =
        request_handler_.CreatePartition(context_ptr_, collection_name->std_str(), param->partition_tag->std_str());

    ASSIGN_RETURN_STATUS_DTO(status)
}

StatusDto::ObjectWrapper
WebRequestHandler::ShowPartitions(const OString& collection_name, const OQueryParams& query_params,
                                  PartitionListDto::ObjectWrapper& partition_list_dto) {
    int64_t offset = 0;
    auto status = ParseQueryInteger(query_params, "offset", offset);
    if (!status.ok()) {
        RETURN_STATUS_DTO(status.code(), status.message().c_str());
    }

    int64_t page_size = 10;
    status = ParseQueryInteger(query_params, "page_size", page_size);
    if (!status.ok()) {
        RETURN_STATUS_DTO(status.code(), status.message().c_str());
    }

    if (offset < 0 || page_size < 0) {
        ASSIGN_RETURN_STATUS_DTO(
            Status(SERVER_UNEXPECTED_ERROR, "Query param 'offset' or 'page_size' should equal or bigger than 0"));
    }

    bool all_required = false;
    auto required = query_params.get("all_required");
    if (nullptr != required.get()) {
        auto required_str = required->std_str();
        if (!ValidationUtil::ValidateStringIsBool(required_str).ok()) {
            RETURN_STATUS_DTO(ILLEGAL_QUERY_PARAM, "Query param \'all_required\' must be a bool")
        }
        all_required = required_str == "True" || required_str == "true";
    }

    std::vector<PartitionParam> partitions;
    status = request_handler_.ShowPartitions(context_ptr_, collection_name->std_str(), partitions);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status)
    }

    if (all_required) {
        offset = 0;
        page_size = partitions.size();
    } else {
        offset = std::min((size_t)offset, partitions.size());
        page_size = std::min(partitions.size() - offset, (size_t)page_size);
    }

    partition_list_dto->count = partitions.size();
    partition_list_dto->partitions = partition_list_dto->partitions->createShared();

    if (offset < (int64_t)(partitions.size())) {
        for (int64_t i = offset; i < page_size + offset; i++) {
            auto partition_dto = PartitionFieldsDto::createShared();
            partition_dto->partition_tag = partitions.at(i).tag_.c_str();
            partition_list_dto->partitions->pushBack(partition_dto);
        }
    }

    ASSIGN_RETURN_STATUS_DTO(status)
}

StatusDto::ObjectWrapper
WebRequestHandler::DropPartition(const OString& collection_name, const OString& body) {
    std::string tag;
    try {
        auto json = nlohmann::json::parse(body->std_str());
        tag = json["partition_tag"].get<std::string>();
    } catch (nlohmann::detail::parse_error& e) {
        RETURN_STATUS_DTO(BODY_PARSE_FAIL, e.what())
    } catch (nlohmann::detail::type_error& e) {
        RETURN_STATUS_DTO(BODY_PARSE_FAIL, e.what())
    }
    auto status = request_handler_.DropPartition(context_ptr_, collection_name->std_str(), tag);

    ASSIGN_RETURN_STATUS_DTO(status)
}

/***********
 *
 * Segment {
 */
StatusDto::ObjectWrapper
WebRequestHandler::ShowSegments(const OString& collection_name, const OQueryParams& query_params, OString& response) {
    int64_t offset = 0;
    auto status = ParseQueryInteger(query_params, "offset", offset);
    if (!status.ok()) {
        RETURN_STATUS_DTO(status.code(), status.message().c_str());
    }

    int64_t page_size = 10;
    status = ParseQueryInteger(query_params, "page_size", page_size);
    if (!status.ok()) {
        RETURN_STATUS_DTO(status.code(), status.message().c_str());
    }

    if (offset < 0 || page_size < 0) {
        RETURN_STATUS_DTO(ILLEGAL_QUERY_PARAM, "Query param 'offset' or 'page_size' should equal or bigger than 0");
    }

    bool all_required = false;
    auto required = query_params.get("all_required");
    if (nullptr != required.get()) {
        auto required_str = required->std_str();
        if (!ValidationUtil::ValidateStringIsBool(required_str).ok()) {
            RETURN_STATUS_DTO(ILLEGAL_QUERY_PARAM, "Query param \'all_required\' must be a bool")
        }
        all_required = required_str == "True" || required_str == "true";
    }

    std::string tag;
    if (nullptr != query_params.get("partition_tag").get()) {
        tag = query_params.get("partition_tag")->std_str();
    }

    std::string info;
    status = request_handler_.ShowCollectionInfo(context_ptr_, collection_name->std_str(), info);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status)
    }

    nlohmann::json info_json = nlohmann::json::parse(info);
    nlohmann::json segments_json = nlohmann::json::array();
    for (auto& par : info_json["partitions"]) {
        if (!(all_required || tag.empty() || tag == par["tag"])) {
            continue;
        }

        auto segments = par["segments"];
        if (!segments.is_null()) {
            for (auto& seg : segments) {
                seg["partition_tag"] = par["tag"];
                segments_json.push_back(seg);
            }
        }
    }
    nlohmann::json result_json;
    if (!all_required) {
        int64_t size = segments_json.size();
        int iter_begin = std::min(size, offset);
        int iter_end = std::min(size, offset + page_size);

        nlohmann::json segments_slice_json = nlohmann::json::array();
        segments_slice_json.insert(segments_slice_json.begin(), segments_json.begin() + iter_begin,
                                   segments_json.begin() + iter_end);
        result_json["segments"] = segments_slice_json;  // segments_json;
    } else {
        result_json["segments"] = segments_json;
    }
    result_json["count"] = segments_json.size();
    AddStatusToJson(result_json, status.code(), status.message());
    response = result_json.dump().c_str();

    ASSIGN_RETURN_STATUS_DTO(status)
}

StatusDto::ObjectWrapper
WebRequestHandler::GetSegmentInfo(const OString& collection_name, const OString& segment_name, const OString& info,
                                  const OQueryParams& query_params, OString& result) {
    int64_t offset = 0;
    auto status = ParseQueryInteger(query_params, "offset", offset);
    if (!status.ok()) {
        RETURN_STATUS_DTO(status.code(), status.message().c_str());
    }

    int64_t page_size = 10;
    status = ParseQueryInteger(query_params, "page_size", page_size);
    if (!status.ok()) {
        RETURN_STATUS_DTO(status.code(), status.message().c_str());
    }

    if (offset < 0 || page_size < 0) {
        ASSIGN_RETURN_STATUS_DTO(
            Status(SERVER_UNEXPECTED_ERROR, "Query param 'offset' or 'page_size' should equal or bigger than 0"));
    }

    std::string re = info->std_str();
    status = Status::OK();
    nlohmann::json json;
    // Get vectors
    if (re == "vectors") {
        status = GetSegmentVectors(collection_name->std_str(), segment_name->std_str(), page_size, offset, json);
        // Get vector ids
    } else if (re == "ids") {
        status = GetSegmentIds(collection_name->std_str(), segment_name->std_str(), page_size, offset, json);
    }

    result = status.ok() ? json.dump().c_str() : "NULL";

    ASSIGN_RETURN_STATUS_DTO(status)
}

/**********
 *
 * Vector {
 */
StatusDto::ObjectWrapper
WebRequestHandler::Insert(const OString& collection_name, const OString& body, VectorIdsDto::ObjectWrapper& ids_dto) {
    if (nullptr == body.get() || body->getSize() == 0) {
        RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Request payload is required.")
    }

    // step 1: copy vectors
    bool bin_flag;
    auto status = IsBinaryCollection(collection_name->std_str(), bin_flag);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status)
    }

    auto body_json = nlohmann::json::parse(body->std_str());
    if (!body_json.contains("vectors")) {
        RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Field \'vectors\' is required");
    }
    engine::VectorsData vectors;
    CopyRecordsFromJson(body_json["vectors"], vectors, bin_flag);
    if (!status.ok()) {
        ASSIGN_RETURN_STATUS_DTO(status)
    }

    // step 2: copy id array
    if (body_json.contains("ids")) {
        auto& ids_json = body_json["ids"];
        if (!ids_json.is_array()) {
            RETURN_STATUS_DTO(ILLEGAL_BODY, "Field \"ids\" must be an array");
        }
        auto& id_array = vectors.id_array_;
        id_array.clear();
        try {
            for (auto& id_str : ids_json) {
                int64_t id = std::stol(id_str.get<std::string>());
                id_array.emplace_back(id);
            }
        } catch (std::exception& e) {
            std::string err_msg = std::string("Cannot convert vectors id. details: ") + e.what();
            RETURN_STATUS_DTO(SERVER_UNEXPECTED_ERROR, err_msg.c_str());
        }
    }

    // step 3: copy partition tag
    std::string tag;
    if (body_json.contains("partition_tag")) {
        tag = body_json["partition_tag"];
    }

    // step 4: construct result
    status = request_handler_.Insert(context_ptr_, collection_name->std_str(), vectors, tag);
    if (status.ok()) {
        ids_dto->ids = ids_dto->ids->createShared();
        for (auto& id : vectors.id_array_) {
            ids_dto->ids->pushBack(std::to_string(id).c_str());
        }
    }

    ASSIGN_RETURN_STATUS_DTO(status)
}

StatusDto::ObjectWrapper
WebRequestHandler::GetVector(const OString& collection_name, const OQueryParams& query_params, OString& response) {
    auto status = Status::OK();
    try {
        auto query_ids = query_params.get("ids");
        if (query_ids == nullptr || query_ids.get() == nullptr) {
            RETURN_STATUS_DTO(QUERY_PARAM_LOSS, "Query param ids is required.");
        }

        std::string str_tag;
        auto partition_tag = query_params.get("partition_tag");
        if (partition_tag != nullptr) {
            str_tag = partition_tag->c_str();
        }

        std::vector<std::string> ids;
        StringHelpFunctions::SplitStringByDelimeter(query_ids->c_str(), ",", ids);

        std::vector<int64_t> vector_ids;
        for (auto& id : ids) {
            vector_ids.push_back(std::stol(id));
        }
        engine::VectorsData vectors;
        nlohmann::json vectors_json;
        status = GetVectorsByIDs(collection_name->std_str(), str_tag, vector_ids, vectors_json);
        if (!status.ok()) {
            response = "NULL";
            ASSIGN_RETURN_STATUS_DTO(status)
        }

        FloatJson json;
        json["code"] = (int64_t)status.code();
        json["message"] = status.message();
        if (vectors_json.empty()) {
            json["vectors"] = std::vector<int64_t>();
        } else {
            json["vectors"] = vectors_json;
        }
        response = json.dump().c_str();
    } catch (std::exception& e) {
        RETURN_STATUS_DTO(SERVER_UNEXPECTED_ERROR, e.what());
    }

    ASSIGN_RETURN_STATUS_DTO(status);
}

StatusDto::ObjectWrapper
WebRequestHandler::VectorsOp(const OString& collection_name, const OString& payload, OString& response) {
    auto status = Status::OK();
    std::string result_str;

    try {
        nlohmann::json payload_json = nlohmann::json::parse(payload->std_str());

        if (payload_json.contains("delete")) {
            status = DeleteByIDs(collection_name->std_str(), payload_json["delete"], result_str);
        } else if (payload_json.contains("search")) {
            status = Search(collection_name->std_str(), payload_json["search"], result_str);
        } else {
            status = Status(ILLEGAL_BODY, "Unknown body");
        }
    } catch (nlohmann::detail::parse_error& e) {
        std::string emsg = "json error: code=" + std::to_string(e.id) + ", reason=" + e.what();
        RETURN_STATUS_DTO(BODY_PARSE_FAIL, emsg.c_str());
    } catch (nlohmann::detail::type_error& e) {
        std::string emsg = "json error: code=" + std::to_string(e.id) + ", reason=" + e.what();
        RETURN_STATUS_DTO(BODY_PARSE_FAIL, emsg.c_str());
    } catch (std::exception& e) {
        RETURN_STATUS_DTO(SERVER_UNEXPECTED_ERROR, e.what());
    }

    response = status.ok() ? result_str.c_str() : "NULL";

    ASSIGN_RETURN_STATUS_DTO(status)
}

/**********
 *
 * System {
 */
StatusDto::ObjectWrapper
WebRequestHandler::SystemInfo(const OString& cmd, const OQueryParams& query_params, OString& response_str) {
    std::string info = cmd->std_str();

    auto status = Status::OK();
    std::string result_str;

    try {
        if (info == "config") {
            status = GetConfig(result_str);
        } else {
            if ("info" == info) {
                info = "get_system_info";
            }
            status = Cmd(info, result_str);
        }
    } catch (nlohmann::detail::parse_error& e) {
        std::string emsg = "json error: code=" + std::to_string(e.id) + ", reason=" + e.what();
        RETURN_STATUS_DTO(BODY_PARSE_FAIL, emsg.c_str());
    } catch (nlohmann::detail::type_error& e) {
        std::string emsg = "json error: code=" + std::to_string(e.id) + ", reason=" + e.what();
        RETURN_STATUS_DTO(BODY_PARSE_FAIL, emsg.c_str());
    }

    response_str = status.ok() ? result_str.c_str() : "NULL";

    ASSIGN_RETURN_STATUS_DTO(status);
}

StatusDto::ObjectWrapper
WebRequestHandler::SystemOp(const OString& op, const OString& body_str, OString& response_str) {
    if (nullptr == body_str.get() || body_str->getSize() == 0) {
        RETURN_STATUS_DTO(BODY_FIELD_LOSS, "Payload is empty.");
    }

    Status status = Status::OK();
    std::string result_str;
    try {
        fiu_do_on("WebRequestHandler.SystemOp.raise_parse_error",
                  throw nlohmann::detail::parse_error::create(0, 0, ""));
        fiu_do_on("WebRequestHandler.SystemOp.raise_type_error", throw nlohmann::detail::type_error::create(0, ""));
        nlohmann::json j = nlohmann::json::parse(body_str->c_str());
        if (op->equals("task")) {
            if (j.contains("load")) {
                status = PreLoadCollection(j["load"], result_str);
            } else if (j.contains("flush")) {
                status = Flush(j["flush"], result_str);
            } else if (j.contains("compact")) {
                status = Compact(j["compact"], result_str);
            } else if (j.contains("release")) {
                status = ReleaseCollection(j["release"], result_str);
            }
        } else if (op->equals("config")) {
            status = SetConfig(j, result_str);
        } else {
            status = Status(UNKNOWN_PATH, "Unknown path: /system/" + op->std_str());
        }
    } catch (nlohmann::detail::parse_error& e) {
        std::string emsg = "json error: code=" + std::to_string(e.id) + ", reason=" + e.what();
        RETURN_STATUS_DTO(BODY_PARSE_FAIL, emsg.c_str());
    } catch (nlohmann::detail::type_error& e) {
        std::string emsg = "json error: code=" + std::to_string(e.id) + ", reason=" + e.what();
        RETURN_STATUS_DTO(BODY_PARSE_FAIL, emsg.c_str());
    }

    response_str = status.ok() ? result_str.c_str() : "NULL";

    ASSIGN_RETURN_STATUS_DTO(status);
}

}  // namespace web
}  // namespace server
}  // namespace milvus
