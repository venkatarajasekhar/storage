#include "store_client_center.h"
#include "storage.h"
#include "store_types.h"

namespace storage
{

uint64_t kOpSeq = 0;

RecordRecycle::RecordRecycle(Logger *logger, StoreClientCenter *client_center)
: logger_(logger), client_center_(client_center), mutex_("RecordRecycle::mutex"), stop_(false)
{

}

void RecordRecycle::Init()
{
    Create();
    return;
}

int32_t RecordRecycle::AddToRecycleQueue(StoreClient *store_client, RecordFile *record_file)
{
    assert(store_client != NULL);
    assert(record_file != NULL);

    Mutex::Locker lock(mutex_);
    LOG_DEBUG(logger_, "add to recycle queue, store_client is %p, record_file is %p", store_client, record_file);

    UTime end_time = record_file->end_time_;

    RecycleItem recycle_item;
    recycle_item.store_client = store_client;
    recycle_item.record_file = record_file;

    multimap<UTime, RecycleItem>::iterator insert_iter = recycle_map_.insert(make_pair(end_time, recycle_item));

    pair<map<RecordFile*, multimap<UTime, RecycleItem>::iterator>::iterator, bool> ret;
    ret = recycle_item_search_map_.insert(make_pair(record_file, insert_iter));
    assert(ret.second == true);

    return 0;
}

int32_t RecordRecycle::RemoveFromRecycleQueue(RecordFile *record_file)
{
    Mutex::Locker lock(mutex_);

    map<RecordFile*, multimap<UTime, RecycleItem>::iterator>::iterator search_iter = recycle_item_search_map_.find(record_file);
    assert(search_iter != recycle_item_search_map_.end());

    multimap<UTime, RecycleItem>::iterator iter = search_iter->second;
    assert(iter != recycle_map_.end());

    recycle_map_.erase(iter);
    recycle_item_search_map_.erase(search_iter);
    
    return 0;
}

int32_t RecordRecycle::UpdateRecordFile(StoreClient *store_client, RecordFile *record_file)
{
    assert(store_client != NULL);
    assert(record_file != NULL);

    int32_t ret;

    ret = RemoveFromRecycleQueue(record_file);
    assert (ret == 0);

    ret = AddToRecycleQueue(store_client, record_file);
    assert(ret == 0);

    return 0;
}

int32_t RecordRecycle::StartRecycle()
{
    Mutex::Locker lock(mutex_);
    cond_.Signal();

    return 0;
}

void *RecordRecycle::Entry()
{
    int32_t ret = 0;

    mutex_.Lock();
    while (true)
    {
        uint32_t recycle_count = 0;
        multimap<UTime, RecycleItem>::iterator iter = recycle_map_.begin();
        while (recycle_count < kFilesPerRecycle && iter != recycle_map_.end() && !stop_)
        {
            RecycleItem recycle_item = iter->second;
            RecordFile *record_file = recycle_item.record_file;
            StoreClient *store_client = recycle_item.store_client;

            assert(record_file != NULL);
            assert(store_client != NULL);

            LOG_INFO(logger_, "recycle record file %srecord_%05d, store_client info [%s]", record_file->base_name_.c_str(), 
                                record_file->number_, store_client->GetStreamInfo().c_str());

            ret = store_client->RecycleRecordFile(record_file);
            if (ret == -ERR_RECORD_FILE_BUSY)
            {
                LOG_INFO(logger_, "recycle continue, record file %srecord_%05d is using", 
                                    record_file->base_name_.c_str(), record_file->number_);
                iter++;
                continue;
            }

            assert(ret == 0);

            multimap<UTime, RecycleItem>::iterator del_iter = iter;
            iter++;
            recycle_map_.erase(del_iter);
            recycle_item_search_map_.erase(record_file);

            /* add to free file table */
            free_file_table->Put(record_file);
            recycle_count++;

            client_center_->TryRemoveStoreClient(store_client);
        }

        if (iter == recycle_map_.end())
        {
            LOG_ERROR(logger_, "recycle entry reached to end of recycle_map_");
        }

        if (stop_)
        {
            break;
        }
        
        cond_.Wait(mutex_);
    }

    mutex_.Unlock();

    return 0;
}

void RecordRecycle::Shutdown()
{
    LOG_INFO(logger_, "record recycle shutdown");
    mutex_.Lock();
    stop_ = true;
    cond_.Signal();
    mutex_.Unlock();
    Join();

    return;
}

void RecordRecycle::Dump()
{
    LOG_INFO(logger_, "*************** record recycle map have %d record files ***************", recycle_map_.size());

    return;
}

StoreClientCenter::StoreClientCenter(Logger *logger)
: logger_(logger), rwlock_("StoreClientCenter::RWLock"), timer_lock("StoreClientCenter::timer_lock"), timer(logger_, timer_lock)
{
    clients_.resize(MAX_STREAM_COUNTS, 0);
}

int32_t StoreClientCenter::FindStoreClientUnlocked(string stream_info, StoreClient **client)
{
    map<string, StoreClient*>::iterator iter = client_search_map_.find(stream_info);
    if (iter == client_search_map_.end())
    {
        LOG_WARN(logger_, "find stream info error, stream info [%s]", stream_info.c_str());
        return -ERR_ITEM_NOT_FOUND;
    }

    *client = iter->second;
    LOG_DEBUG(logger_, "find store client ok, stream info is %s", stream_info.c_str());

    return 0;
}

int32_t StoreClientCenter::GetStoreClientUnlocked(int32_t id, StoreClient **client)
{
    assert(client != NULL);

    if (id >= MAX_STREAM_COUNTS)
    {
        LOG_WARN(logger_, "id %d exceed max stream id", id);
        return -ERR_ITEM_NOT_FOUND;
    }

    *client = clients_[id];
    if (*client == NULL)
    {
        LOG_WARN(logger_, "store client is NULL, id %d not exist", id);
        return -ERR_ITEM_NOT_FOUND;
    }

    LOG_DEBUG(logger_, "get store client ok, id is %d", id);

    return 0;
}


int32_t StoreClientCenter::Init()
{
    {
        Mutex::Locker lock(timer_lock);
        timer.Init();
    }

    return 0;
}

int32_t StoreClientCenter::Open(int flag, int32_t id, string &stream_info)
{
    assert(flag == 0 || flag == 1);

    int32_t ret;
    StoreClient *client = NULL;

    if (flag == 0)
    {
        ret = FindStoreClient(stream_info, &client);
        if (ret != 0)
        {
            LOG_INFO(logger_, "find store client error, stream info %s, ret %d", stream_info.c_str(), ret);
            return -ERR_ITEM_NOT_FOUND;
        }

        ret = client->OpenRead(id);
        if (ret != 0)
        {
            LOG_INFO(logger_, "open read error, id %d, flag %d, stream info %s, ret %d", id, flag, stream_info.c_str(), ret);
            return ret;
        }

        assert(clients_[id] == NULL);
        {
            RWLock::WRLocker lock(rwlock_);
            clients_[id] = client;
            client->IncUse();
        }
    }
    else
    {
        bool new_client = false;
        ret = FindStoreClient(stream_info, &client);
        if (ret != 0)
        {
            client = new StoreClient(logger_, stream_info);
            assert(client != NULL);
            LOG_INFO(logger_, "no store client, so new store client, id %d, flag %d, stream info %s", id, flag, stream_info.c_str());
            new_client = true;
        }

        ret = client->OpenWrite(id);
        if (ret != 0)
        {
            LOG_INFO(logger_, "open write error, id %d, flag %d, stream info %s, ret %d", id, flag, stream_info.c_str());
            return ret;
        }

        assert(clients_[id] == NULL);
        {
            RWLock::WRLocker lock(rwlock_);
            clients_[id] = client;
            if (new_client)
            {
                client_search_map_.insert(make_pair(stream_info, client));
            }
            client->IncUse();
        }
    }

    LOG_DEBUG(logger_, "open ok, id %d, flag %d, stream info %s", id, flag, stream_info.c_str());

    return 0;
}

int32_t StoreClientCenter::AddStoreClient(string &stream_info, StoreClient **client)
{
    LOG_INFO(logger_, "add store client, stream info %s", stream_info.c_str());

    RWLock::WRLocker lock(rwlock_);
    *client = new StoreClient(logger_, stream_info);
    assert(*client != NULL);
    client_search_map_.insert(make_pair(stream_info, *client));

    return 0;
}

int32_t StoreClientCenter::FindStoreClient(string stream_info, StoreClient **client)
{
    assert(client != NULL);

    RWLock::RDLocker lock(rwlock_);
    map<string, StoreClient*>::iterator iter = client_search_map_.find(stream_info);
    if (iter == client_search_map_.end())
    {
        LOG_INFO(logger_, "find stream info error, stream info [%s]", stream_info.c_str());
        return -ERR_ITEM_NOT_FOUND;
    }

    *client = iter->second;
    LOG_TRACE(logger_, "find store client ok, stream info is %s", stream_info.c_str());

    return 0;
}

int32_t StoreClientCenter::GetStoreClient(int32_t id, StoreClient **client)
{
    assert(client != NULL);

    if (id >= MAX_STREAM_COUNTS)
    {
        LOG_WARN(logger_, "id %d exceed max stream id", id);
        return -ERR_ITEM_NOT_FOUND;
    }

    RWLock::RDLocker lock(rwlock_);
    *client = clients_[id];
    if (*client == NULL)
    {
        LOG_WARN(logger_, "store client is NULL, id %d not exist", id);
        return -ERR_ITEM_NOT_FOUND;
    }

    LOG_TRACE(logger_, "get store client ok, id is %d", id);

    return 0;
}

int32_t StoreClientCenter::TryRemoveStoreClientUnlock(StoreClient *client)
{
    assert(client != NULL);
    bool ret = 0;
    
    ret = client->CheckRecycle();
    if (!ret)
    {
        LOG_DEBUG(logger_, "client using, so can't recycle");
        return -ERR_CLIENT_USED;
    }

    string stream_info = client->GetStreamInfo();
    map<string, StoreClient*>::iterator map_iter = client_search_map_.find(stream_info);
    assert(map_iter != client_search_map_.end());
    client_search_map_.erase(map_iter);

    delete client;
    client = NULL;

    LOG_INFO(logger_, "remove store client ok, client is %p", client);
    return 0;
}

int32_t StoreClientCenter::TryRemoveStoreClient(StoreClient *client)
{
    RWLock::WRLocker lock(rwlock_);
    return TryRemoveStoreClientUnlock(client);
}

int32_t StoreClientCenter::Close(int32_t id, int flag)
{
    assert(flag == 0 || flag == 1);

    int32_t ret;
    StoreClient *client = NULL;

    ret = GetStoreClient(id, &client);
    if (ret != 0)
    {
        LOG_INFO(logger_, "get store client error, id %d, ret %d", id, ret);
        return ret;
    }

    if (flag == 0)
    {
        ret = client->CloseRead(id);
        if (ret != 0)
        {
            LOG_WARN(logger_, "close read error, id %d, ret %d", id, ret);
        }
    }
    else
    {
        ret = client->CloseWrite(id);
        if (ret != 0)
        {
            LOG_WARN(logger_, "close write error, id %d, ret %d", id, ret);
        }
    }

    /* update clients */
    {
        RWLock::WRLocker lock(rwlock_);
        clients_[id] = NULL;
        client->DecUse();
        TryRemoveStoreClientUnlock(client);
    }

    LOG_INFO(logger_, "close store client return, id %d, ret %d", id, ret);

    return ret;
}

int32_t StoreClientCenter::WriteFrame(int32_t id, FRAME_INFO_T *frame)
{
    assert(frame != NULL);
    LOG_TRACE(logger_, "write frame, id %d, frame %p, buffer size %d", id, frame, frame->size);

    int32_t ret;
    StoreClient *client = NULL;

    ret = GetStoreClient(id, &client);
    if (ret != 0)
    {
        LOG_WARN(logger_, "get store client error, id %d, ret %d", id, ret);
        return ret;
    }

    return client->EnqueueFrame(frame);
}

int32_t StoreClientCenter::SeekRead(int32_t id, UTime &stamp)
{
    int32_t ret;
    StoreClient *client = NULL;

    ret = GetStoreClient(id, &client);
    if (ret != 0)
    {
        LOG_WARN(logger_, "get store client error, id %d, ret %d", id, ret);
        return ret;
    }

    ret = client->SeekRead(id, stamp);
    if (ret != 0)
    {
        LOG_WARN(logger_, "seek frame error, id %d, ret %d", id, ret);
        return ret;
    }

    LOG_DEBUG(logger_, "seek read ok, id %d, stamp %d.%d", id, stamp.tv_sec, stamp.tv_nsec);
    return 0;
}

int32_t StoreClientCenter::ReadFrame(int32_t id, FRAME_INFO_T *frame)
{
    assert(frame != NULL);

    int32_t ret;
    StoreClient *client = NULL;

    ret = GetStoreClient(id, &client);
    if (ret != 0)
    {
        LOG_WARN(logger_, "get store client error, id %d, ret %d", id, ret);
        return ret;
    }

    ret = client->ReadFrame(id, frame);
    if (ret != 0)
    {
        LOG_WARN(logger_, "read frame error, id %d, ret %d", id, ret);
        return ret;
    }

    LOG_TRACE(logger_, "read frame ok, id %d", id);
    return 0;
}

int32_t StoreClientCenter::ListRecordFragments(int32_t id, UTime &start, UTime &end, deque<FRAGMENT_INFO_T> &frag_info_queue)
{
    LOG_DEBUG(logger_, "list record fragments, id is %d, start time is %d.%d, end time is %d.%d", 
        id, start.tv_sec, start.tv_nsec, end.tv_sec, end.tv_nsec);

    int32_t ret;
    StoreClient *store_client = NULL;

    ret = GetStoreClient(id, &store_client);
    if (ret != 0)
    {
        LOG_WARN(logger_, "get store client error, id %d, start time %d.%d, end time %d.%d, ret %d", 
                    id, start.tv_sec, start.tv_nsec, end.tv_sec, end.tv_nsec, ret);
        return ret;
    }

    ret = store_client->ListRecordFragments(start, end, frag_info_queue);
    if (ret != 0)
    {
        LOG_WARN(logger_, "list record fragments error, id %d, start time %d.%d, end time %d.%d, ret %d",
                    id, start.tv_sec, start.tv_nsec, end.tv_sec, end.tv_nsec, ret);
        return ret;
    }

    LOG_DEBUG(logger_, "list record fragments ok, id %d, start time %d.%d, end time %d.%d, ret %d",
                id, start.tv_sec, start.tv_nsec, end.tv_sec, end.tv_nsec, ret);
    return 0;
}

void StoreClientCenter::Shutdown()
{
    {
        RWLock::WRLocker lock(rwlock_);
        map<string, StoreClient*>::iterator iter = client_search_map_.begin();
        for(; iter != client_search_map_.end(); iter++)
        {
            StoreClient *store_client = iter->second;
            assert(store_client != NULL);
            delete store_client;
            iter->second = NULL;
        }
    }

    {
        Mutex::Locker lock(timer_lock);
        timer.Shutdown();
    }

    return;
}

void StoreClientCenter::Dump()
{
    int32_t record_file_sum = 0;

    RWLock::RDLocker lock(rwlock_);

    LOG_INFO(logger_, "############### store client center dump start ###############");
    map<string, StoreClient*>::iterator iter = client_search_map_.begin();
    for (; iter != client_search_map_.end(); iter++)
    {
        StoreClient *store_client = iter->second;
        store_client->Dump();
        record_file_sum += store_client->GetRecordFileNumbers();
    }
    LOG_INFO(logger_, "############### store client center dump end, record file sum %d ###############", record_file_sum);

    return;
}

}

