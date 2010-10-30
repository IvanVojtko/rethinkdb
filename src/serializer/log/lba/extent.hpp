
#ifndef __SERIALIZER_LOG_LBA_EXTENT__
#define __SERIALIZER_LOG_LBA_EXTENT__

#include "containers/intrusive_list.hpp"
#include "arch/arch.hpp"
#include "extent.hpp"
#include "../extents/extent_manager.hpp"
#include "disk_format.hpp"

/*
The extent_t type represents a LBA extent or LBA superblock extent on the disk.
It is a "meta-structure" that is a wrapper around the extent on disk.

There are two ways to create an extent_t:
1. Use the load() static method to load from a location on disk.
2. Use the create() static method to create a new extent.

If you use the create() method, you can then write to its contents using
the data() method and then use the sync() method to sync to disk.

There are two ways to destroy an extent_t:
1. Use the destroy() method to release both the extent_t and the location on disk
2. Use the shutdown() method to release just the extent_t

You should not use the 'new' or 'delete' operators with extent_t.
*/

/* TODO:
We need a way to tell the extent_t that we don't actually need its contents in memory anymore, so
it can free its internal buffer. This is an optimization to save memory and isn't urgent.
*/

extern perfmon_counter_t pm_serializer_lba_extents;

template <class data_t>
struct extent_t :
    public iocallback_t,
    public alloc_mixin_t<tls_small_obj_alloc_accessor<alloc_t>, extent_t<data_t> >
{
public:
    struct sync_callback_t : public intrusive_list_node_t<sync_callback_t> {
        virtual void on_sync_extent() = 0;
    };
    
    struct load_callback_t {
        virtual void on_load_extent() = 0;
    };

private:
    extent_manager_t *em;
    data_t *_data;
    direct_file_t *file;
    size_t amount_synced;
    
    enum {
        mode_loaded,
        mode_created
    } mode;
    
    // Used only if mode == mode_loaded
    bool done_loading;
    load_callback_t *load_callback;
    bool destroying;

public:
    off64_t offset;

private:
    // Use create() or load() instead
    extent_t(extent_manager_t *_em, direct_file_t *file, off64_t _offset)
        : em(_em), file(file), destroying(false), offset(_offset), last_sync(NULL)
    {
        assert(em);
        assert(offset % (em->extent_size) == 0);
        
        _data = (data_t*)malloc_aligned(em->extent_size, DEVICE_BLOCK_SIZE);
        assert(_data);

#ifdef VALGRIND
        memset(_data, 0xBD, em->extent_size);   // Make Valgrind happy
#endif
        
        pm_serializer_lba_extents++;
    }
    
    // Use delete() or shutdown() instead
    virtual ~extent_t() {
        assert(!last_sync);
        free(_data);
        pm_serializer_lba_extents--;
    }

public:
    /* Make a extent_t reflecting data already on disk */
    static bool load(extent_manager_t *em, direct_file_t *file, off64_t offset, size_t amount_used, extent_t **out, load_callback_t *cb)
    {
        em->reserve_extent(offset);
        
        extent_t *buf = new extent_t(em, file, offset);
        buf->mode = mode_loaded;
        buf->amount_synced = amount_used;
        
        buf->done_loading = false;
        buf->load_callback = cb;
        
        buf->file->read_async(
            buf->offset,
            buf->amount_synced,
            (void*)buf->_data,
            buf);
        
        *out = buf;
        return false;
    }
    
private:
    void on_io_complete(event_t *e) {
        assert(mode == mode_loaded);
        assert(!done_loading);
        done_loading = true;
        if (load_callback) load_callback->on_load_extent();
    }

public:
    /* Allocate a new extent */
    static void create(extent_manager_t *em, direct_file_t *file, extent_t **out)
    {
        off64_t offset = em->gen_extent();
        extent_t *buf = new extent_t(em, file, offset);
        buf->mode = mode_created;
        buf->amount_synced = 0;
        
        *out = buf;
    }
    
public:
    data_t *data() {
        assert(mode == mode_created || done_loading);
        return _data;
    }

private:
    struct sync_handler_t :
        public iocallback_t,
        public sync_callback_t,
        public alloc_mixin_t<tls_small_obj_alloc_accessor<alloc_t>, sync_handler_t>
    {
        extent_t *owner;
        bool waiting_for_part, waiting_for_all_prev;
        intrusive_list_t<sync_callback_t> callbacks;
        
        sync_handler_t(extent_t *owner) : owner(owner) {
            waiting_for_part = true;
            if (owner->last_sync) {
                waiting_for_all_prev = true;
                owner->last_sync->callbacks.push_front(this);
            } else {
                waiting_for_all_prev = false;
            }
            owner->last_sync = this;
        }
        
        void on_io_complete(event_t *e) {
            assert(waiting_for_part);
            waiting_for_part = false;
            if (!waiting_for_part && !waiting_for_all_prev) done();
        }
        
        void on_sync_extent() {
            assert(waiting_for_all_prev);
            waiting_for_all_prev = false;
            if (!waiting_for_part && !waiting_for_all_prev) done();
        }
        
        void done() {
            if (this == owner->last_sync) {
                owner->last_sync = NULL;
                if (owner->destroying) delete owner;
            }
            while (sync_callback_t *cb = callbacks.head()) {
                callbacks.remove(cb);
                cb->on_sync_extent();
            }
            delete this;
        }
    };
    sync_handler_t *last_sync;

public:
    bool sync(size_t amount, sync_callback_t *cb) {
        
        assert(amount >= amount_synced);
        assert(amount % DEVICE_BLOCK_SIZE == 0);
        
        if (amount > amount_synced) {
            
            sync_handler_t *handler = new sync_handler_t(this);
            if (cb) handler->callbacks.push_back(cb);
            
            file->write_async(
                offset + amount_synced,
                amount - amount_synced,
                (char*)_data + amount_synced,
                handler);
            
            amount_synced = amount;
            
            return false;
        
        } else {
        
            return true;
        }
    }

public:
    void destroy() {
        em->release_extent(offset);
        if (last_sync == NULL) delete this;
        else destroying = true;
    }
    
    void shutdown() {
        assert(last_sync == NULL);
        delete this;
    }
};

#endif /* __SERIALIZER_LOG_LBA_EXTENT__ */
