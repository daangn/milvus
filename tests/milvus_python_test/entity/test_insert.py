import time
import pdb
import threading
import logging
import threading
from multiprocessing import Pool, Process
import pytest
from milvus import IndexType, MetricType
from utils import *


dim = 128
index_file_size = 10
collection_id = "test_add"
ADD_TIMEOUT = 60
tag = "1970-01-01"
add_interval_time = 5
nb = 6000


class TestAddBase:
    """
    ******************************************************************
      The following cases are used to test `insert` function
    ******************************************************************
    """
    @pytest.fixture(
        scope="function",
        params=gen_simple_index()
    )
    def get_simple_index(self, request, connect):
        if str(connect._cmd("mode")[1]) == "CPU":
            if request.param["index_type"] == IndexType.IVF_SQ8H:
                pytest.skip("sq8h not support in cpu mode")
        if request.param["index_type"] == IndexType.IVF_PQ:
            pytest.skip("Skip PQ Temporary")
        return request.param

    def test_add_vector_create_collection(self, connect, collection):
        '''
        target: test add vector, then create collection again
        method: add vector and create collection
        expected: status not ok
        '''
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        param = {'collection_name': collection,
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        assert not status.OK()

    def test_add_vector_has_collection(self, connect, collection):
        '''
        target: test add vector, then check collection existence
        method: add vector and call Hascollection
        expected: collection exists, status ok
        '''
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        assert assert_has_collection(connect, collection)

    def test_add_vector_with_empty_vector(self, connect, collection):
        '''
        target: test add vectors with empty vectors list
        method: set empty vectors list as add method params
        expected: raises a Exception
        '''
        vector = []
        with pytest.raises(Exception) as e:
            status, ids = connect.insert(collection, vector)

    def test_add_vector_with_None(self, connect, collection):
        '''
        target: test add vectors with None
        method: set None as add method params
        expected: raises a Exception
        '''
        vector = None
        with pytest.raises(Exception) as e:
            status, ids = connect.insert(collection, vector)

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_drop_collection_add_vector(self, connect, collection):
        '''
        target: test add vector after collection deleted
        method: delete collection and add vector
        expected: status not ok
        '''
        status = connect.drop_collection(collection)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        assert not status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_drop_collection_add_vector_another(self, connect, collection):
        '''
        target: test add vector to collection_1 after collection_2 deleted
        method: delete collection_2 and add vector to collection_1
        expected: status ok
        '''
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        status = connect.drop_collection(collection)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(param['collection_name'], vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_drop_collection(self, connect, collection):
        '''
        target: test delete collection after add vector
        method: add vector and delete collection
        expected: status ok
        '''
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        status = connect.drop_collection(collection)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_delete_another_collection(self, connect, collection):
        '''
        target: test delete collection_1 collection after add vector to collection_2
        method: add vector and delete collection
        expected: status ok
        '''
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_sleep_drop_collection(self, connect, collection):
        '''
        target: test delete collection after add vector for a while
        method: add vector, sleep, and delete collection
        expected: status ok
        '''
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        assert status.OK()
        connect.flush([collection])
        status = connect.drop_collection(collection)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_sleep_delete_another_collection(self, connect, collection):
        '''
        target: test delete collection_1 collection after add vector to collection_2 for a while
        method: add vector , sleep, and delete collection
        expected: status ok
        '''
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        connect.flush([collection])
        status = connect.drop_collection(param['collection_name'])
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_create_index_add_vector(self, connect, collection, get_simple_index):
        '''
        target: test add vector after build index
        method: build index and add vector
        expected: status ok
        '''
        index_param = get_simple_index["index_param"]
        index_type = get_simple_index["index_type"]
        status = connect.create_index(collection, index_type, index_param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_create_index_add_vector_another(self, connect, collection, get_simple_index):
        '''
        target: test add vector to collection_2 after build index for collection_1
        method: build index and add vector
        expected: status ok
        '''
        index_param = get_simple_index["index_param"]
        index_type = get_simple_index["index_type"]
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        status = connect.create_index(collection, index_type, index_param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        connect.drop_collection(param['collection_name'])
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_create_index(self, connect, collection, get_simple_index):
        '''
        target: test build index add after vector
        method: add vector and build index
        expected: status ok
        '''
        index_param = get_simple_index["index_param"]
        index_type = get_simple_index["index_type"]
        logging.getLogger().info(index_param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        status = connect.create_index(collection, index_type, index_param)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_create_index_another(self, connect, collection, get_simple_index):
        '''
        target: test add vector to collection_2 after build index for collection_1
        method: build index and add vector
        expected: status ok
        '''
        index_param = get_simple_index["index_param"]
        index_type = get_simple_index["index_type"]
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        status = connect.create_index(
            param['collection_name'], index_type, index_param)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_sleep_create_index(self, connect, collection, get_simple_index):
        '''
        target: test build index add after vector for a while
        method: add vector and build index
        expected: status ok
        '''
        index_param = get_simple_index["index_param"]
        index_type = get_simple_index["index_type"]
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        connect.flush([collection])
        status = connect.create_index(collection, index_type, index_param)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_sleep_create_index_another(self, connect, collection, get_simple_index):
        '''
        target: test add vector to collection_2 after build index for collection_1 for a while
        method: build index and add vector
        expected: status ok
        '''
        index_param = get_simple_index["index_param"]
        index_type = get_simple_index["index_type"]
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        connect.flush([collection])
        status = connect.create_index(
            param['collection_name'], index_type, index_param)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_search_vector_add_vector(self, connect, collection):
        '''
        target: test add vector after search collection
        method: search collection and add vector
        expected: status ok
        '''
        vector = gen_single_vector(dim)
        status, result = connect.search(collection, 1, vector)
        status, ids = connect.insert(collection, vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_search_vector_add_vector_another(self, connect, collection):
        '''
        target: test add vector to collection_1 after search collection_2
        method: search collection and add vector
        expected: status ok
        '''
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        vector = gen_single_vector(dim)
        status, result = connect.search(collection, 1, vector)
        status, ids = connect.insert(param['collection_name'], vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_search_vector(self, connect, collection):
        '''
        target: test search vector after add vector
        method: add vector and search collection
        expected: status ok
        '''
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        assert status.OK()
        connect.flush([collection])
        status, result = connect.search(collection, 1, vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_search_vector_another(self, connect, collection):
        '''
        target: test add vector to collection_1 after search collection_2
        method: search collection and add vector
        expected: status ok
        '''
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        status, result = connect.search(param['collection_name'], 1, vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_sleep_search_vector(self, connect, collection):
        '''
        target: test search vector after add vector after a while
        method: add vector, sleep, and search collection
        expected: status ok
        '''
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        connect.flush([collection])
        status, result = connect.search(collection, 1, vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_sleep_search_vector_another(self, connect, collection):
        '''
        target: test add vector to collection_1 after search collection_2 a while
        method: search collection , sleep, and add vector
        expected: status ok
        '''
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(collection, vector)
        connect.flush([collection])
        status, result = connect.search(param['collection_name'], 1, vector)
        assert status.OK()

    """
    ******************************************************************
      The following cases are used to test `insert` function
    ******************************************************************
    """

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert_ids(self, connect, collection):
        '''
        target: test add vectors in collection, use customize ids
        method: create collection and add vectors in it, check the ids returned and the collection length after vectors added
        expected: the length of ids and the collection row count
        '''
        nq = 5
        top_k = 1
        vectors = gen_vectors(nq, dim)
        ids = [i for i in range(nq)]
        status, ids = connect.insert(collection, vectors, ids)
        connect.flush([collection])
        assert status.OK()
        assert len(ids) == nq
        status, result = connect.search(
            collection, top_k, query_records=vectors)
        logging.getLogger().info(result)
        assert len(result) == nq
        for i in range(nq):
            assert result[i][0].id == i

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert_twice_ids_no_ids(self, connect, collection):
        '''
        target: check the result of insert, with params ids and no ids
        method: test add vectors twice, use customize ids first, and then use no ids
        expected: status not OK 
        '''
        nq = 5
        top_k = 1
        vectors = gen_vectors(nq, dim)
        ids = [i for i in range(nq)]
        status, ids = connect.insert(collection, vectors, ids)
        assert status.OK()
        status, ids = connect.insert(collection, vectors)
        logging.getLogger().info(status)
        logging.getLogger().info(ids)
        assert not status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert_twice_not_ids_ids(self, connect, collection):
        '''
        target: check the result of insert, with params ids and no ids
        method: test add vectors twice, use not ids first, and then use customize ids
        expected: status not OK 
        '''
        nq = 5
        top_k = 1
        vectors = gen_vectors(nq, dim)
        ids = [i for i in range(nq)]
        status, ids = connect.insert(collection, vectors)
        assert status.OK()
        status, ids = connect.insert(collection, vectors, ids)
        logging.getLogger().info(status)
        logging.getLogger().info(ids)
        assert not status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert_ids_length_not_match(self, connect, collection):
        '''
        target: test add vectors in collection, use customize ids, len(ids) != len(vectors)
        method: create collection and add vectors in it
        expected: raise an exception
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        ids = [i for i in range(1, nq)]
        with pytest.raises(Exception) as e:
            status, ids = connect.insert(collection, vectors, ids)

    @pytest.fixture(
        scope="function",
        params=gen_invalid_vector_ids()
    )
    def get_vector_id(self, request):
        yield request.param

    @pytest.mark.level(2)
    def test_insert_ids_invalid(self, connect, collection, get_vector_id):
        '''
        target: test add vectors in collection, use customize ids, which are not int64
        method: create collection and add vectors in it
        expected: raise an exception
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        vector_id = get_vector_id
        ids = [vector_id for _ in range(nq)]
        with pytest.raises(Exception):
            connect.insert(collection, vectors, ids)

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert(self, connect, collection):
        '''
        target: test add vectors in collection created before
        method: create collection and add vectors in it, check the ids returned and the collection length after vectors added
        expected: the length of ids and the collection row count
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        status, ids = connect.insert(collection, vectors)
        assert status.OK()
        assert len(ids) == nq

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert_tag(self, connect, collection):
        '''
        target: test add vectors in collection created before
        method: create collection and add vectors in it, with the partition_tag param
        expected: the collection row count equals to nq
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        status = connect.create_partition(collection, tag)
        status, ids = connect.insert(collection, vectors, partition_tag=tag)
        assert status.OK()
        assert len(ids) == nq

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert_default_tag(self, connect, collection):
        '''
        target: test add vectors in default partition created before
        method: create collection and add vectors in it, with the default paritition name
        expected: the collection row count equals to nq
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        status, ids = connect.insert(collection, vectors, partition_tag="_default")
        assert status.OK()
        assert len(ids) == nq

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert_tag_A(self, connect, collection):
        '''
        target: test add vectors in collection created before
        method: create partition and add vectors in it
        expected: the collection row count equals to nq
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        status = connect.create_partition(collection, tag)
        status, ids = connect.insert(collection, vectors, partition_tag=tag)
        assert status.OK()
        assert len(ids) == nq

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert_tag_not_existed(self, connect, collection):
        '''
        target: test add vectors in collection created before
        method: create collection and add vectors in it, with the not existed partition_tag param
        expected: status not ok
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        status, ids = connect.insert(collection, vectors, partition_tag=tag)
        assert not status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert_tag_not_existed_A(self, connect, collection):
        '''
        target: test add vectors in collection created before
        method: create partition, add vectors with the not existed partition_tag param
        expected: status not ok
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        new_tag = "new_tag"
        status = connect.create_partition(collection, tag)
        status, ids = connect.insert(
            collection, vectors, partition_tag=new_tag)
        assert not status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert_tag_existed(self, connect, collection):
        '''
        target: test add vectors in collection created before
        method: create collection and add vectors in it repeatly, with the partition_tag param
        expected: the collection row count equals to nq
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        status = connect.create_partition(collection, tag)
        status, ids = connect.insert(collection, vectors, partition_tag=tag)
        for i in range(5):
            status, ids = connect.insert(
                collection, vectors, partition_tag=tag)
            assert status.OK()
            assert len(ids) == nq

    @pytest.mark.level(2)
    def test_insert_without_connect(self, dis_connect, collection):
        '''
        target: test add vectors without connection
        method: create collection and add vectors in it, check if added successfully
        expected: raise exception
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        with pytest.raises(Exception) as e:
            status, ids = dis_connect.insert(collection, vectors)

    def test_add_collection_not_existed(self, connect):
        '''
        target: test add vectors in collection, which not existed before
        method: add vectors collection not existed, check the status
        expected: status not ok
        '''
        nq = 5
        vector = gen_single_vector(dim)
        status, ids = connect.insert(
            gen_unique_str("not_exist_collection"), vector)
        assert not status.OK()
        assert not ids

    def test_add_vector_dim_not_matched(self, connect, collection):
        '''
        target: test add vector, the vector dimension is not equal to the collection dimension
        method: the vector dimension is half of the collection dimension, check the status
        expected: status not ok
        '''
        vector = gen_single_vector(int(dim)//2)
        status, ids = connect.insert(collection, vector)
        assert not status.OK()

    def test_insert_dim_not_matched(self, connect, collection):
        '''
        target: test add vectors, the vector dimension is not equal to the collection dimension
        method: the vectors dimension is half of the collection dimension, check the status
        expected: status not ok
        '''
        nq = 5
        vectors = gen_vectors(nq, int(dim)//2)
        status, ids = connect.insert(collection, vectors)
        assert not status.OK()

    def test_add_vector_query_after_sleep(self, connect, collection):
        '''
        target: test add vectors, and search it after sleep
        method: set vector[0][1] as query vectors
        expected: status ok and result length is 1
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        status, ids = connect.insert(collection, vectors)
        connect.flush([collection])
        status, result = connect.search(collection, 1, [vectors[0]])
        assert status.OK()
        assert len(result) == 1

    @pytest.mark.level(2)
    @pytest.mark.timeout(30)
    def test_collection_add_rows_count_multi_threading(self, args, collection):
        '''
        target: test collection rows_count is correct or not with multi threading
        method: create collection and add vectors in it(idmap),
            assert the value returned by count_entities method is equal to length of vectors
        expected: the count is equal to the length of vectors
        '''
        if args["handler"] == "HTTP":
            pytest.skip("Skip test in http mode")
        thread_num = 8
        threads = []
        milvus = get_milvus(
            host=args["ip"], port=args["port"], handler=args["handler"], try_connect=False)
        vectors = gen_vectors(nb, dim)

        def add(thread_i):
            logging.getLogger().info("In thread-%d" % thread_i)
            # milvus = get_milvus(host=args["ip"], port=args["port"], handler=args["handler"])
            # assert milvus
            status, result = milvus.insert(collection, records=vectors)
            assert status.OK()
            status = milvus.flush([collection])
            assert status.OK()
        for i in range(thread_num):
            x = threading.Thread(target=add, args=(i, ))
            threads.append(x)
            x.start()
        for th in threads:
            th.join()
        status, res = milvus.count_entities(collection)
        assert res == thread_num * nb

    def test_add_vector_multi_collections(self, connect):
        '''
        target: test add vectors is correct or not with multiple collections of L2
        method: create 50 collections and add vectors into them in turn
        expected: status ok
        '''
        nq = 100
        vectors = gen_vectors(nq, dim)
        collection_list = []
        for i in range(20):
            collection_name = gen_unique_str(
                'test_add_vector_multi_collections')
            collection_list.append(collection_name)
            param = {'collection_name': collection_name,
                     'dimension': dim,
                     'index_file_size': index_file_size,
                     'metric_type': MetricType.L2}
            connect.create_collection(param)
        for j in range(5):
            for i in range(20):
                status, ids = connect.insert(
                    collection_name=collection_list[i], records=vectors)
                assert status.OK()


class TestAddAsync:
    @pytest.fixture(scope="function", autouse=True)
    def skip_http_check(self, args):
        if args["handler"] == "HTTP":
            pytest.skip("skip in http mode")

    @pytest.fixture(
        scope="function",
        params=[
            1,
            1000
        ],
    )
    def insert_count(self, request):
        yield request.param

    def check_status(self, status, result):
        logging.getLogger().info("In callback check status")
        assert status.OK()

    def check_status_not_ok(self, status, result):
        logging.getLogger().info("In callback check status")
        assert not status.OK()

    def test_insert_async(self, connect, collection, insert_count):
        '''
        target: test add vectors with different length of vectors
        method: set different vectors as add method params
        expected: length of ids is equal to the length of vectors
        '''
        nb = insert_count
        insert_vec_list = gen_vectors(nb, dim)
        future = connect.insert(collection, insert_vec_list, _async=True)
        status, ids = future.result()
        connect.flush([collection])
        assert len(ids) == nb
        assert status.OK()

    @pytest.mark.level(2)
    def test_insert_async_false(self, connect, collection, insert_count):
        '''
        target: test add vectors with different length of vectors
        method: set different vectors as add method params
        expected: length of ids is equal to the length of vectors
        '''
        nb = insert_count
        insert_vec_list = gen_vectors(nb, dim)
        status, ids = connect.insert(collection, insert_vec_list, _async=False)
        connect.flush([collection])
        assert len(ids) == nb
        assert status.OK()

    def test_insert_async_callback(self, connect, collection, insert_count):
        '''
        target: test add vectors with different length of vectors
        method: set different vectors as add method params
        expected: length of ids is equal to the length of vectors
        '''
        nb = insert_count
        insert_vec_list = gen_vectors(nb, dim)
        future = connect.insert(
            collection, insert_vec_list, _async=True, _callback=self.check_status)
        future.done()

    @pytest.mark.level(2)
    def test_insert_async_long(self, connect, collection):
        '''
        target: test add vectors with different length of vectors
        method: set different vectors as add method params
        expected: length of ids is equal to the length of vectors
        '''
        nb = 50000
        insert_vec_list = gen_vectors(nb, dim)
        future = connect.insert(
            collection, insert_vec_list, _async=True, _callback=self.check_status)
        status, result = future.result()
        assert status.OK()
        assert len(result) == nb
        connect.flush([collection])
        status, count = connect.count_entities(collection)
        assert status.OK()
        logging.getLogger().info(status)
        logging.getLogger().info(count)
        assert count == nb

    def test_insert_async_callback_timeout(self, connect, collection):
        '''
        target: test add vectors with different length of vectors
        method: set different vectors as add method params
        expected: length of ids is equal to the length of vectors
        '''
        nb = 100000
        insert_vec_list = gen_vectors(nb, dim)
        future = connect.insert(collection, insert_vec_list,
                                _async=True, _callback=self.check_status, timeout=1)
        future.done()

    def test_insert_async_invalid_params(self, connect, collection):
        '''
        target: test add vectors with different length of vectors
        method: set different vectors as add method params
        expected: length of ids is equal to the length of vectors
        '''
        insert_vec_list = gen_vectors(nb, dim)
        collection_new = gen_unique_str()
        future = connect.insert(collection_new, insert_vec_list, _async=True)
        status, result = future.result()
        assert not status.OK()

    # TODO: add assertion
    def test_insert_async_invalid_params_raise_exception(self, connect, collection):
        '''
        target: test add vectors with different length of vectors
        method: set different vectors as add method params
        expected: length of ids is equal to the length of vectors
        '''
        insert_vec_list = []
        collection_new = gen_unique_str()
        with pytest.raises(Exception) as e:
            future = connect.insert(
                collection_new, insert_vec_list, _async=True)



class TestAddIP:
    """
    ******************************************************************
      The following cases are used to test `insert / index / search / delete` mixed function
    ******************************************************************
    """
    @pytest.fixture(
        scope="function",
        params=gen_simple_index()
    )
    def get_simple_index(self, request, connect):
        if str(connect._cmd("mode")[1]) == "CPU":
            if request.param["index_type"] == IndexType.IVF_SQ8H:
                pytest.skip("sq8h not support in cpu mode")
        if request.param["index_type"] == IndexType.IVF_PQ:
            pytest.skip("Skip PQ Temporary")
        return request.param

    def test_add_vector_create_collection(self, connect, ip_collection):
        '''
        target: test add vector, then create collection again
        method: add vector and create collection
        expected: status not ok
        '''
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        param = {'collection_name': ip_collection,
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        assert not status.OK()

    def test_add_vector_has_collection(self, connect, ip_collection):
        '''
        target: test add vector, then check collection existence
        method: add vector and call Hascollection
        expected: collection exists, status ok
        '''
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        assert assert_has_collection(connect, ip_collection)

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_drop_collection_add_vector(self, connect, ip_collection):
        '''
        target: test add vector after collection deleted
        method: delete collection and add vector
        expected: status not ok
        '''
        status = connect.drop_collection(ip_collection)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        assert not status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_drop_collection_add_vector_another(self, connect, ip_collection):
        '''
        target: test add vector to collection_1 after collection_2 deleted
        method: delete collection_2 and add vector to collection_1
        expected: status ok
        '''
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        status = connect.drop_collection(ip_collection)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(param['collection_name'], vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_drop_collection(self, connect, ip_collection):
        '''
        target: test delete collection after add vector
        method: add vector and delete collection
        expected: status ok
        '''
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        status = connect.drop_collection(ip_collection)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_delete_another_collection(self, connect, ip_collection):
        '''
        target: test delete collection_1 collection after add vector to collection_2
        method: add vector and delete collection
        expected: status ok
        '''
        param = {'collection_name': 'test_add_vector_delete_another_collection',
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        status = connect.drop_collection(param['collection_name'])
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_sleep_drop_collection(self, connect, ip_collection):
        '''
        target: test delete collection after add vector for a while
        method: add vector, sleep, and delete collection
        expected: status ok
        '''
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        connect.flush([ip_collection])
        status = connect.drop_collection(ip_collection)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_sleep_delete_another_collection(self, connect, ip_collection):
        '''
        target: test delete collection_1 collection after add vector to collection_2 for a while
        method: add vector , sleep, and delete collection
        expected: status ok
        '''
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        connect.flush([ip_collection])
        status = connect.drop_collection(param['collection_name'])
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_create_index_add_vector(self, connect, ip_collection, get_simple_index):
        '''
        target: test add vector after build index
        method: build index and add vector
        expected: status ok
        '''
        index_param = get_simple_index["index_param"]
        index_type = get_simple_index["index_type"]
        status = connect.create_index(ip_collection, index_type, index_param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_create_index_add_vector_another(self, connect, ip_collection, get_simple_index):
        '''
        target: test add vector to collection_2 after build index for collection_1
        method: build index and add vector
        expected: status ok
        '''
        index_param = get_simple_index["index_param"]
        index_type = get_simple_index["index_type"]
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        status = connect.create_index(ip_collection, index_type, index_param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_create_index(self, connect, ip_collection, get_simple_index):
        '''
        target: test build index add after vector
        method: add vector and build index
        expected: status ok
        '''
        index_param = get_simple_index["index_param"]
        index_type = get_simple_index["index_type"]
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        status, mode = connect._cmd("mode")
        assert status.OK()
        status = connect.create_index(ip_collection, index_type, index_param)
        if str(mode) == "GPU" and (index_type == IndexType.IVF_PQ):
            assert not status.OK()
        else:
            assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_create_index_another(self, connect, ip_collection, get_simple_index):
        '''
        target: test add vector to collection_2 after build index for collection_1
        method: build index and add vector
        expected: status ok
        '''
        index_param = get_simple_index["index_param"]
        index_type = get_simple_index["index_type"]
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        status = connect.create_index(
            param['collection_name'], index_type, index_param)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_sleep_create_index(self, connect, ip_collection, get_simple_index):
        '''
        target: test build index add after vector for a while
        method: add vector and build index
        expected: status ok
        '''
        index_param = get_simple_index["index_param"]
        index_type = get_simple_index["index_type"]
        if index_type == IndexType.IVF_PQ:
            pytest.skip("Skip some PQ cases")
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        assert status.OK()
        time.sleep(add_interval_time)
        status = connect.create_index(ip_collection, index_type, index_param)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_sleep_create_index_another(self, connect, ip_collection, get_simple_index):
        '''
        target: test add vector to collection_2 after build index for collection_1 for a while
        method: build index and add vector
        expected: status ok
        '''
        index_param = get_simple_index["index_param"]
        index_type = get_simple_index["index_type"]
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        connect.flush([ip_collection])
        status = connect.create_index(
            param['collection_name'], index_type, index_param)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_search_vector_add_vector(self, connect, ip_collection):
        '''
        target: test add vector after search collection
        method: search collection and add vector
        expected: status ok
        '''
        vector = gen_single_vector(dim)
        status, result = connect.search(ip_collection, 1, vector)
        status, ids = connect.insert(ip_collection, vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_search_vector_add_vector_another(self, connect, ip_collection):
        '''
        target: test add vector to collection_1 after search collection_2
        method: search collection and add vector
        expected: status ok
        '''
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        vector = gen_single_vector(dim)
        status, result = connect.search(ip_collection, 1, vector)
        status, ids = connect.insert(param['collection_name'], vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_search_vector(self, connect, ip_collection):
        '''
        target: test search vector after add vector
        method: add vector and search collection
        expected: status ok
        '''
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        assert status.OK()
        connect.flush([ip_collection])
        status, result = connect.search(ip_collection, 1, vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_search_vector_another(self, connect, ip_collection):
        '''
        target: test add vector to collection_1 after search collection_2
        method: search collection and add vector
        expected: status ok
        '''
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        connect.flush([ip_collection])
        status, result = connect.search(param['collection_name'], 1, vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_sleep_search_vector(self, connect, ip_collection):
        '''
        target: test search vector after add vector after a while
        method: add vector, sleep, and search collection
        expected: status ok
        '''
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        time.sleep(add_interval_time)
        status, result = connect.search(ip_collection, 1, vector)
        assert status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_add_vector_sleep_search_vector_another(self, connect, ip_collection):
        '''
        target: test add vector to collection_1 after search collection_2 a while
        method: search collection , sleep, and add vector
        expected: status ok
        '''
        param = {'collection_name': gen_unique_str(),
                 'dimension': dim,
                 'index_file_size': index_file_size,
                 'metric_type': MetricType.L2}
        status = connect.create_collection(param)
        vector = gen_single_vector(dim)
        status, ids = connect.insert(ip_collection, vector)
        assert status.OK()
        time.sleep(add_interval_time)
        status, result = connect.search(param['collection_name'], 1, vector)
        assert status.OK()

    """
    ******************************************************************
      The following cases are used to test `insert` function
    ******************************************************************
    """

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert_ids(self, connect, ip_collection):
        '''
        target: test add vectors in collection, use customize ids
        method: create collection and add vectors in it, check the ids returned and the collection length after vectors added
        expected: the length of ids and the collection row count
        '''
        nq = 5
        top_k = 1
        vectors = gen_vectors(nq, dim)
        ids = [i for i in range(nq)]
        status, ids = connect.insert(ip_collection, vectors, ids)
        assert status.OK()
        connect.flush([ip_collection])
        assert len(ids) == nq
        # check search result
        status, result = connect.search(ip_collection, top_k, vectors)
        logging.getLogger().info(result)
        assert len(result) == nq
        for i in range(nq):
            assert result[i][0].id == i

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert_twice_ids_no_ids(self, connect, ip_collection):
        '''
        target: check the result of insert, with params ids and no ids
        method: test add vectors twice, use customize ids first, and then use no ids
        expected: status not OK 
        '''
        nq = 5
        top_k = 1
        vectors = gen_vectors(nq, dim)
        ids = [i for i in range(nq)]
        status, ids = connect.insert(ip_collection, vectors, ids)
        assert status.OK()
        status, ids = connect.insert(ip_collection, vectors)
        logging.getLogger().info(status)
        logging.getLogger().info(ids)
        assert not status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert_twice_not_ids_ids(self, connect, ip_collection):
        '''
        target: check the result of insert, with params ids and no ids
        method: test add vectors twice, use not ids first, and then use customize ids
        expected: status not OK 
        '''
        nq = 5
        top_k = 1
        vectors = gen_vectors(nq, dim)
        ids = [i for i in range(nq)]
        status, ids = connect.insert(ip_collection, vectors)
        assert status.OK()
        status, ids = connect.insert(ip_collection, vectors, ids)
        logging.getLogger().info(status)
        logging.getLogger().info(ids)
        assert not status.OK()

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert_ids_length_not_match(self, connect, ip_collection):
        '''
        target: test add vectors in collection, use customize ids, len(ids) != len(vectors)
        method: create collection and add vectors in it
        expected: raise an exception
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        ids = [i for i in range(1, nq)]
        with pytest.raises(Exception) as e:
            status, ids = connect.insert(ip_collection, vectors, ids)

    @pytest.fixture(
        scope="function",
        params=gen_invalid_vector_ids()
    )
    def get_vector_id(self, request):
        yield request.param

    @pytest.mark.level(2)
    def test_insert_ids_invalid(self, connect, ip_collection, get_vector_id):
        '''
        target: test add vectors in collection, use customize ids, which are not int64
        method: create collection and add vectors in it
        expected: raise an exception
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        vector_id = get_vector_id
        ids = [vector_id for i in range(nq)]
        with pytest.raises(Exception) as e:
            status, ids = connect.insert(ip_collection, vectors, ids)

    @pytest.mark.timeout(ADD_TIMEOUT)
    def test_insert(self, connect, ip_collection):
        '''
        target: test add vectors in collection created before
        method: create collection and add vectors in it, check the ids returned and the collection length after vectors added
        expected: the length of ids and the collection row count
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        status, ids = connect.insert(ip_collection, vectors)
        assert status.OK()
        assert len(ids) == nq

    # @pytest.mark.level(2)
    # def test_insert_without_connect(self, dis_connect, ip_collection):
    #     '''
    #     target: test add vectors without connection
    #     method: create collection and add vectors in it, check if added successfully
    #     expected: raise exception
    #     '''
    #     nq = 5
    #     vectors = gen_vectors(nq, dim)
    #     with pytest.raises(Exception) as e:
    #         status, ids = dis_connect.insert(ip_collection, vectors)

    def test_add_vector_dim_not_matched(self, connect, ip_collection):
        '''
        target: test add vector, the vector dimension is not equal to the collection dimension
        method: the vector dimension is half of the collection dimension, check the status
        expected: status not ok
        '''
        vector = gen_single_vector(int(dim)//2)
        status, ids = connect.insert(ip_collection, vector)
        assert not status.OK()

    def test_insert_dim_not_matched(self, connect, ip_collection):
        '''
        target: test add vectors, the vector dimension is not equal to the collection dimension
        method: the vectors dimension is half of the collection dimension, check the status
        expected: status not ok
        '''
        nq = 5
        vectors = gen_vectors(nq, int(dim)//2)
        status, ids = connect.insert(ip_collection, vectors)
        assert not status.OK()

    def test_add_vector_query_after_sleep(self, connect, ip_collection):
        '''
        target: test add vectors, and search it after sleep
        method: set vector[0][1] as query vectors
        expected: status ok and result length is 1
        '''
        nq = 5
        vectors = gen_vectors(nq, dim)
        status, ids = connect.insert(ip_collection, vectors)
        time.sleep(add_interval_time)
        status, result = connect.search(ip_collection, 1, [vectors[0]])
        assert status.OK()
        assert len(result) == 1

    def test_add_vector_multi_collections(self, connect):
        '''
        target: test add vectors is correct or not with multiple collections of IP
        method: create 50 collections and add vectors into them in turn
        expected: status ok
        '''
        nq = 100
        vectors = gen_vectors(nq, dim)
        collection_list = []
        for i in range(20):
            collection_name = gen_unique_str(
                'test_add_vector_multi_collections')
            collection_list.append(collection_name)
            param = {'collection_name': collection_name,
                     'dimension': dim,
                     'index_file_size': index_file_size,
                     'metric_type': MetricType.IP}
            connect.create_collection(param)
        for j in range(10):
            for i in range(20):
                status, ids = connect.insert(
                    collection_name=collection_list[i], records=vectors)
                assert status.OK()


class TestAddAdvance:
    @pytest.fixture(
        scope="function",
        params=[
            1,
            1000,
            6000
        ],
    )
    def insert_count(self, request):
        yield request.param

    def test_insert_much(self, connect, collection, insert_count):
        '''
        target: test add vectors with different length of vectors
        method: set different vectors as add method params
        expected: length of ids is equal to the length of vectors
        '''
        nb = insert_count
        insert_vec_list = gen_vectors(nb, dim)
        status, ids = connect.insert(collection, insert_vec_list)
        assert len(ids) == nb
        assert status.OK()

    def test_insert_much_ip(self, connect, ip_collection, insert_count):
        '''
        target: test add vectors with different length of vectors
        method: set different vectors as add method params
        expected: length of ids is equal to the length of vectors
        '''
        nb = insert_count
        insert_vec_list = gen_vectors(nb, dim)
        status, ids = connect.insert(ip_collection, insert_vec_list)
        assert len(ids) == nb
        assert status.OK()

    def test_insert_much_jaccard(self, connect, jac_collection, insert_count):
        '''
        target: test add vectors with different length of vectors
        method: set different vectors as add method params
        expected: length of ids is equal to the length of vectors
        '''
        nb = insert_count
        tmp, insert_vec_list = gen_binary_vectors(nb, dim)
        status, ids = connect.insert(jac_collection, insert_vec_list)
        assert len(ids) == nb
        assert status.OK()

    def test_insert_much_hamming(self, connect, ham_collection, insert_count):
        '''
        target: test add vectors with different length of vectors
        method: set different vectors as add method params
        expected: length of ids is equal to the length of vectors
        '''
        nb = insert_count
        tmp, insert_vec_list = gen_binary_vectors(nb, dim)
        status, ids = connect.insert(ham_collection, insert_vec_list)
        assert len(ids) == nb
        assert status.OK()

    def test_insert_much_tanimoto(self, connect, tanimoto_collection, insert_count):
        '''
        target: test add vectors with different length of vectors
        method: set different vectors as add method params
        expected: length of ids is equal to the length of vectors
        '''
        nb = insert_count
        tmp, insert_vec_list = gen_binary_vectors(nb, dim)
        status, ids = connect.insert(tanimoto_collection, insert_vec_list)
        assert len(ids) == nb
        assert status.OK()


class TestNameInvalid(object):
    """
    Test adding vectors with invalid collection names
    """
    @pytest.fixture(
        scope="function",
        params=gen_invalid_collection_names()
    )
    def get_collection_name(self, request):
        yield request.param

    @pytest.fixture(
        scope="function",
        params=gen_invalid_collection_names()
    )
    def get_tag_name(self, request):
        yield request.param

    @pytest.mark.level(2)
    def test_insert_with_invalid_collection_name(self, connect, get_collection_name):
        collection_name = get_collection_name
        vectors = gen_vectors(1, dim)
        status, result = connect.insert(collection_name, vectors)
        assert not status.OK()

    @pytest.mark.level(2)
    def test_insert_with_invalid_tag_name(self, connect, get_collection_name, get_tag_name):
        collection_name = get_collection_name
        tag_name = get_tag_name
        vectors = gen_vectors(1, dim)
        status, result = connect.insert(
            collection_name, vectors, partition_tag=tag_name)
        assert not status.OK()


class TestAddCollectionVectorsInvalid(object):
    single_vector = gen_single_vector(dim)
    vectors = gen_vectors(2, dim)

    """
    Test adding vectors with invalid vectors
    """
    @pytest.fixture(
        scope="function",
        params=gen_invalid_vectors()
    )
    def gen_vector(self, request):
        yield request.param

    @pytest.mark.level(2)
    def test_add_vector_with_invalid_vectors(self, connect, collection, gen_vector):
        tmp_single_vector = copy.deepcopy(self.single_vector)
        tmp_single_vector[0][1] = gen_vector
        with pytest.raises(Exception) as e:
            status, result = connect.insert(collection, tmp_single_vector)

    @pytest.mark.level(2)
    def test_insert_with_invalid_vectors(self, connect, collection, gen_vector):
        tmp_vectors = copy.deepcopy(self.vectors)
        tmp_vectors[1][1] = gen_vector
        with pytest.raises(Exception) as e:
            status, result = connect.insert(collection, tmp_vectors)

    @pytest.mark.level(2)
    def test_insert_with_invalid_vectors_jaccard(self, connect, jac_collection, gen_vector):
        tmp_vectors = copy.deepcopy(self.vectors)
        tmp_vectors[1][1] = gen_vector
        with pytest.raises(Exception) as e:
            status, result = connect.insert(jac_collection, tmp_vectors)

    @pytest.mark.level(2)
    def test_insert_with_invalid_vectors_hamming(self, connect, ham_collection, gen_vector):
        tmp_vectors = copy.deepcopy(self.vectors)
        tmp_vectors[1][1] = gen_vector
        with pytest.raises(Exception) as e:
            status, result = connect.insert(ham_collection, tmp_vectors)
