#include "destor.h"
#include "jcr.h"
#include "storage/containerstore.h"
#include "storage/db.h"
#include "recipe/recipestore.h"
#include "rewrite_phase.h"
#include "backup.h"
#include "index/index.h"
#include "index/fingerprint_cache.h"

static pthread_t filter_t;
static int64_t chunk_num;
extern GHashTable *upgrade_processing;
extern GHashTable *upgrade_container;
extern GHashTable *upgrade_storage_buffer;
containerid upgrade_storage_buffer_id = -1;

struct{
	/* accessed in dedup phase */
	struct container *container_buffer;
	/* In order to facilitate sampling in container,
	 * we keep a list for chunks in container buffer. */
	GSequence *chunks;
} storage_buffer;

extern struct {
	/* g_mutex_init() is unnecessary if in static storage. */
	GMutex mutex;
	GCond cond; // index buffer is not full
	int wait_threshold;
} index_lock;

extern upgrade_lock_t upgrade_index_lock;

static void flush_container() {
    if(destor.index_category[1] != INDEX_CATEGORY_PHYSICAL_LOCALITY) return;

    GHashTable *features = sampling(storage_buffer.chunks,
            g_sequence_get_length(storage_buffer.chunks));
    index_update(features, get_container_id(storage_buffer.container_buffer));

    // do_update index
    if (job == DESTOR_UPDATE && destor.upgrade_level == UPGRADE_1D_RELATION) {
        pthread_mutex_lock(&upgrade_index_lock.mutex);
        upgrade_index_update(storage_buffer.chunks, get_container_id(storage_buffer.container_buffer));
        pthread_mutex_unlock(&upgrade_index_lock.mutex);
    }

    g_hash_table_destroy(features);
    g_sequence_free(storage_buffer.chunks);
    storage_buffer.chunks = g_sequence_new(free_chunk);

    write_container_async(storage_buffer.container_buffer);
    storage_buffer.container_buffer = create_container();
}

static void append_chunk_to_buffer(struct chunk* c) {
    /*
    * If the chunk is unique, or be fragmented and not denied,
    * we write it to a container.
    * Fragmented indicates: sparse, or out of order and not in cache,
    */
    if (storage_buffer.container_buffer == NULL){
        storage_buffer.container_buffer = create_container();
        if(destor.index_category[1] == INDEX_CATEGORY_PHYSICAL_LOCALITY)
            storage_buffer.chunks = g_sequence_new(free_chunk);
    }

    if (container_overflow(storage_buffer.container_buffer, c->size)) {
        flush_container();
    }

    if(add_chunk_to_container(storage_buffer.container_buffer, c)){
        if (!CHECK_CHUNK(c, CHUNK_DUPLICATE)) {
            jcr.unique_chunk_num++;
            jcr.unique_data_size += c->size;
        }

        if(destor.index_category[1] == INDEX_CATEGORY_PHYSICAL_LOCALITY){
            struct chunk* ck = new_chunk(0);
            assert(ck);
            memcpy(&ck->fp, &c->fp, sizeof(fingerprint));
            memcpy(&ck->old_fp, &c->old_fp, sizeof(fingerprint));
            g_sequence_append(storage_buffer.chunks, ck);
        }

        VERBOSE("Filter phase: Write %dth chunk to container %lld",
                chunk_num, c->id);
    }else{
        VERBOSE("Filter phase: container %lld already has this chunk", c->id);
        c->id = get_container_id(storage_buffer.container_buffer);
        SET_CHUNK(c, CHUNK_DUPLICATE);
    }
    chunk_num++; // BUGS: 可能会多次++
}

/*
 * When a container buffer is full, we push it into container_queue.
 */
static void* filter_thread(void *arg) {
    int enable_rewrite = 1;
    struct fileRecipeMeta* r = NULL;
    struct backupVersion* bv = NULL;
    if (job == DESTOR_UPDATE) {
        bv = jcr.new_bv;
    } else {
        bv = jcr.bv;
    }

    DECLARE_TIME_RECORDER("filter_thread");

    while (1) {
        struct chunk* c = sync_queue_pop(rewrite_queue);

        if (c == NULL)
            /* backup job finish */
            break;

        /* reconstruct a segment */
        struct segment* s = new_segment();

        /* segment head */
        assert(CHECK_CHUNK(c, CHUNK_SEGMENT_START));
        free_chunk(c);

        c = sync_queue_pop(rewrite_queue);
        while (!(CHECK_CHUNK(c, CHUNK_SEGMENT_END))) {
            g_sequence_append(s->chunks, c);
            if (!CHECK_CHUNK(c, CHUNK_FILE_START)
                    && !CHECK_CHUNK(c, CHUNK_FILE_END))
                s->chunk_num++;

            c = sync_queue_pop(rewrite_queue);
        }
        free_chunk(c);

        BEGIN_TIME_RECORD

        /* For self-references in a segment.
         * If we find an early copy of the chunk in this segment has been rewritten,
         * the rewrite request for it will be denied to avoid repeat rewriting. */
        GHashTable *recently_rewritten_chunks = g_hash_table_new_full(g_int64_hash,
        		g_fingerprint_equal, NULL, free_chunk);
        GHashTable *recently_unique_chunks = g_hash_table_new_full(g_int64_hash,
        			g_fingerprint_equal, NULL, free_chunk);

        pthread_mutex_lock(&index_lock.mutex);

        TIMER_DECLARE(1);
        TIMER_BEGIN(1);
        /* This function will check the fragmented chunks
         * that would be rewritten later.
         * If we find an early copy of the chunk in earlier segments,
         * has been rewritten,
         * the rewrite request for it will be denied. */
        index_check_buffer(s);

    	GSequenceIter *iter = g_sequence_get_begin_iter(s->chunks);
    	GSequenceIter *end = g_sequence_get_end_iter(s->chunks);
        for (; iter != end; iter = g_sequence_iter_next(iter)) {
            c = g_sequence_get(iter);

    		if (CHECK_CHUNK(c, CHUNK_FILE_START) || CHECK_CHUNK(c, CHUNK_FILE_END))
    			continue;

            VERBOSE("Filter phase: %dth chunk in %s container %lld", chunk_num,
                    CHECK_CHUNK(c, CHUNK_OUT_OF_ORDER) ? "out-of-order" : "", c->id);

            /* Cache-Aware Filter */
            if (destor.rewrite_enable_cache_aware && restore_aware_contains(c->id)) {
                assert(c->id != TEMPORARY_ID);
                VERBOSE("Filter phase: %dth chunk is cached", chunk_num);
                SET_CHUNK(c, CHUNK_IN_CACHE);
            }

            /* A cfl-switch for rewriting out-of-order chunks. */
            if (destor.rewrite_enable_cfl_switch) {
                double cfl = restore_aware_get_cfl();
                if (enable_rewrite && cfl > destor.rewrite_cfl_require) {
                    VERBOSE("Filter phase: Turn OFF the (out-of-order) rewrite switch of %.3f",
                            cfl);
                    enable_rewrite = 0;
                } else if (!enable_rewrite && cfl < destor.rewrite_cfl_require) {
                    VERBOSE("Filter phase: Turn ON the (out-of-order) rewrite switch of %.3f",
                            cfl);
                    enable_rewrite = 1;
                }
            }

            if(CHECK_CHUNK(c, CHUNK_DUPLICATE) && c->id == TEMPORARY_ID){
            	struct chunk* ruc = g_hash_table_lookup(recently_unique_chunks, &c->fp);
            	assert(ruc);
            	c->id = ruc->id;
            }
            struct chunk* rwc = g_hash_table_lookup(recently_rewritten_chunks, &c->fp);
            if(rwc){
            	c->id = rwc->id;
            	SET_CHUNK(c, CHUNK_REWRITE_DENIED);
            }

            /* A fragmented chunk will be denied if it has been rewritten recently */
            if (!CHECK_CHUNK(c, CHUNK_DUPLICATE) 
					|| (!CHECK_CHUNK(c, CHUNK_REWRITE_DENIED)
            		&& (CHECK_CHUNK(c, CHUNK_SPARSE)
                    || (enable_rewrite && CHECK_CHUNK(c, CHUNK_OUT_OF_ORDER)
                        && !CHECK_CHUNK(c, CHUNK_IN_CACHE))))) {
                /*
                 * If the chunk is unique, or be fragmented and not denied,
                 * we write it to a container.
                 * Fragmented indicates: sparse, or out of order and not in cache,
                 */
                if (storage_buffer.container_buffer == NULL){
                	storage_buffer.container_buffer = create_container();
                	if(destor.index_category[1] == INDEX_CATEGORY_PHYSICAL_LOCALITY)
                		storage_buffer.chunks = g_sequence_new(free_chunk);
                }

                if (container_overflow(storage_buffer.container_buffer, c->size)) {

                    if(destor.index_category[1] == INDEX_CATEGORY_PHYSICAL_LOCALITY){
                        /*
                         * TO-DO
                         * Update_index for physical locality
                         */
                        GHashTable *features = sampling(storage_buffer.chunks,
                        		g_sequence_get_length(storage_buffer.chunks));
                        index_update(features, get_container_id(storage_buffer.container_buffer));

                        // do_update index
                        if (job == DESTOR_UPDATE && destor.upgrade_level != UPGRADE_NAIVE) {
                            pthread_mutex_lock(&upgrade_index_lock.mutex);
                            upgrade_index_update(storage_buffer.chunks, get_container_id(storage_buffer.container_buffer));
                            pthread_mutex_unlock(&upgrade_index_lock.mutex);
                        }

                        g_hash_table_destroy(features);
                        g_sequence_free(storage_buffer.chunks);
                        storage_buffer.chunks = g_sequence_new(free_chunk);
                    }
                    TIMER_END(1, jcr.filter_time);
                    write_container_async(storage_buffer.container_buffer);
                    TIMER_BEGIN(1);
                    storage_buffer.container_buffer = create_container();
                }

                if(add_chunk_to_container(storage_buffer.container_buffer, c)){

                	struct chunk* wc = new_chunk(0);
                	memcpy(&wc->fp, &c->fp, sizeof(fingerprint));
                	wc->id = c->id;
                	if (!CHECK_CHUNK(c, CHUNK_DUPLICATE)) {
                		jcr.unique_chunk_num++;
                		jcr.unique_data_size += c->size;
                		g_hash_table_insert(recently_unique_chunks, &wc->fp, wc);
                    	VERBOSE("Filter phase: %dth chunk is recently unique, size %d", chunk_num,
                    			g_hash_table_size(recently_unique_chunks));
                	} else {
                		jcr.rewritten_chunk_num++;
                		jcr.rewritten_chunk_size += c->size;
                		g_hash_table_insert(recently_rewritten_chunks, &wc->fp, wc);
                	}

                	if(destor.index_category[1] == INDEX_CATEGORY_PHYSICAL_LOCALITY){
                		struct chunk* ck = new_chunk(0);
                		memcpy(&ck->fp, &c->fp, sizeof(fingerprint));
                        memcpy(&ck->old_fp, &c->old_fp, sizeof(fingerprint));
                		g_sequence_append(storage_buffer.chunks, ck);
                	}

                	VERBOSE("Filter phase: Write %dth chunk to container %lld",
                			chunk_num, c->id);
                }else{
                	VERBOSE("Filter phase: container %lld already has this chunk", c->id);
            		assert(destor.index_category[0] != INDEX_CATEGORY_EXACT
            				|| destor.rewrite_algorithm[0]!=REWRITE_NO);
                }

            }else{
                if(CHECK_CHUNK(c, CHUNK_REWRITE_DENIED)){
                    VERBOSE("Filter phase: %lldth fragmented chunk is denied", chunk_num);
                }else if (CHECK_CHUNK(c, CHUNK_OUT_OF_ORDER)) {
                    VERBOSE("Filter phase: %lldth chunk in out-of-order container %lld is already cached",
                            chunk_num, c->id);
                }
            }

            assert(c->id != TEMPORARY_ID);

            /* Collect historical information. */
            har_monitor_update(c->id, c->size);

            /* Restore-aware */
            restore_aware_update(c->id, c->size);

            chunk_num++;
        }

        int full = index_update_buffer(s);

        /* Write a SEGMENT_BEGIN */
        segmentid sid = append_segment_flag(bv, CHUNK_SEGMENT_START, s->chunk_num);

        /* Write recipe */
    	iter = g_sequence_get_begin_iter(s->chunks);
    	end = g_sequence_get_end_iter(s->chunks);
        for (; iter != end; iter = g_sequence_iter_next(iter)) {
            c = g_sequence_get(iter);

        	if(r == NULL){
        		assert(CHECK_CHUNK(c,CHUNK_FILE_START));
        		r = new_file_recipe_meta(c->data);
        	}else if(!CHECK_CHUNK(c,CHUNK_FILE_END)){
        		struct chunkPointer cp;
        		cp.id = c->id;
        		assert(cp.id>=0);
        		memcpy(&cp.fp, &c->fp, sizeof(fingerprint));
        		cp.size = c->size;
        		append_n_chunk_pointers(bv, &cp ,1);
        		r->chunknum++;
        		r->filesize += c->size;

    	    	jcr.chunk_num++;
	    	    jcr.data_size += c->size;

        	}else{
        		assert(CHECK_CHUNK(c,CHUNK_FILE_END));
        		append_file_recipe_meta(bv, r);
        		free_file_recipe_meta(r);
        		r = NULL;

	            jcr.file_num++;
        	}
        }

       	/* Write a SEGMENT_END */
       	append_segment_flag(bv, CHUNK_SEGMENT_END, 0);

        if(destor.index_category[1] == INDEX_CATEGORY_LOGICAL_LOCALITY){
             /*
              * TO-DO
              * Update_index for logical locality
              */
            s->features = sampling(s->chunks, s->chunk_num);
         	if(destor.index_category[0] == INDEX_CATEGORY_EXACT){
         		/*
         		 * For exact deduplication,
         		 * unique fingerprints are inserted.
         		 */
         		VERBOSE("Filter phase: add %d unique fingerprints to %d features",
         				g_hash_table_size(recently_unique_chunks),
         				g_hash_table_size(s->features));
         		GHashTableIter iter;
         		gpointer key, value;
         		g_hash_table_iter_init(&iter, recently_unique_chunks);
         		while(g_hash_table_iter_next(&iter, &key, &value)){
         			struct chunk* uc = value;
         			fingerprint *ft = malloc(sizeof(fingerprint));
         			memcpy(ft, &uc->fp, sizeof(fingerprint));
         			g_hash_table_insert(s->features, ft, NULL);
         		}

         		/*
         		 * OPTION:
         		 * 	It is still an open problem whether we need to update
         		 * 	rewritten fingerprints.
         		 * 	It would increase index update overhead, while the benefit
         		 * 	remains unclear.
         		 * 	More experiments are required.
         		 */
         		VERBOSE("Filter phase: add %d rewritten fingerprints to %d features",
         				g_hash_table_size(recently_rewritten_chunks),
         				g_hash_table_size(s->features));
         		g_hash_table_iter_init(&iter, recently_rewritten_chunks);
         		while(g_hash_table_iter_next(&iter, &key, &value)){
         			struct chunk* uc = value;
         			fingerprint *ft = malloc(sizeof(fingerprint));
         			memcpy(ft, &uc->fp, sizeof(fingerprint));
         			g_hash_table_insert(s->features, ft, NULL);
         		}
         	}
         	index_update(s->features, sid);
         }

        free_segment(s);

        if(index_lock.wait_threshold > 0 && full == 0){
        	pthread_cond_broadcast(&index_lock.cond);
        }
        TIMER_END(1, jcr.filter_time);
        pthread_mutex_unlock(&index_lock.mutex);

        g_hash_table_destroy(recently_rewritten_chunks);
        g_hash_table_destroy(recently_unique_chunks);

        END_TIME_RECORD
    }

    if (storage_buffer.container_buffer
    		&& !container_empty(storage_buffer.container_buffer)){
        if(destor.index_category[1] == INDEX_CATEGORY_PHYSICAL_LOCALITY){
            /*
             * TO-DO
             * Update_index for physical locality
             */
        	GHashTable *features = sampling(storage_buffer.chunks,
        			g_sequence_get_length(storage_buffer.chunks));
        	index_update(features, get_container_id(storage_buffer.container_buffer));

            // do_update index
            if (job == DESTOR_UPDATE && destor.upgrade_level != UPGRADE_NAIVE) {
                pthread_mutex_lock(&upgrade_index_lock.mutex);
                upgrade_index_update(storage_buffer.chunks, get_container_id(storage_buffer.container_buffer));
                pthread_mutex_unlock(&upgrade_index_lock.mutex);
            }

        	g_hash_table_destroy(features);
        	g_sequence_free(storage_buffer.chunks);
        }
        write_container_async(storage_buffer.container_buffer);
    }

	FINISH_TIME_RECORD
    /* All files done */
    jcr.status = JCR_STATUS_DONE;
    return NULL;
}

static void* filter_thread_simplified(void *arg) {
    int enable_rewrite = 1;
    struct fileRecipeMeta* r = NULL;
    struct backupVersion* bv = jcr.new_bv;
    assert(destor.index_category[1] == INDEX_CATEGORY_PHYSICAL_LOCALITY);
    assert(job == DESTOR_UPDATE && destor.upgrade_level != UPGRADE_NAIVE);

    while (1) {
        struct chunk* c = sync_queue_pop(rewrite_queue);

        if (c == NULL)
            /* backup job finish */
            break;

        /* reconstruct a segment */
        struct segment* s = new_segment();

        /* segment head */
        assert(CHECK_CHUNK(c, CHUNK_SEGMENT_START));
        free_chunk(c);

        c = sync_queue_pop(rewrite_queue);
        while (!(CHECK_CHUNK(c, CHUNK_SEGMENT_END))) {
            g_sequence_append(s->chunks, c);
            if (!CHECK_CHUNK(c, CHUNK_FILE_START)
                    && !CHECK_CHUNK(c, CHUNK_FILE_END))
                s->chunk_num++;

            c = sync_queue_pop(rewrite_queue);
        }
        free_chunk(c);

        pthread_mutex_lock(&index_lock.mutex);

        TIMER_DECLARE(1);
        TIMER_BEGIN(1);
        /* This function will check the fragmented chunks
         * that would be rewritten later.
         * If we find an early copy of the chunk in earlier segments,
         * has been rewritten,
         * the rewrite request for it will be denied. */

    	GSequenceIter *iter = g_sequence_get_begin_iter(s->chunks);
    	GSequenceIter *end = g_sequence_get_end_iter(s->chunks);
        for (; iter != end; iter = g_sequence_iter_next(iter)) {
            c = g_sequence_get(iter);

    		if (CHECK_CHUNK(c, CHUNK_FILE_START) || CHECK_CHUNK(c, CHUNK_FILE_END))
    			continue;

            /* A fragmented chunk will be denied if it has been rewritten recently */
            if (!CHECK_CHUNK(c, CHUNK_DUPLICATE) 
					|| (!CHECK_CHUNK(c, CHUNK_REWRITE_DENIED)
            		&& (CHECK_CHUNK(c, CHUNK_SPARSE)
                    || (enable_rewrite && CHECK_CHUNK(c, CHUNK_OUT_OF_ORDER)
                        && !CHECK_CHUNK(c, CHUNK_IN_CACHE))))) {

                append_chunk_to_buffer(c);

            }else{
                if(CHECK_CHUNK(c, CHUNK_REWRITE_DENIED)){
                    VERBOSE("Filter phase: %lldth fragmented chunk is denied", chunk_num);
                }else if (CHECK_CHUNK(c, CHUNK_OUT_OF_ORDER)) {
                    VERBOSE("Filter phase: %lldth chunk in out-of-order container %lld is already cached",
                            chunk_num, c->id);
                }
            }

            assert(c->id != TEMPORARY_ID);

            chunk_num++;
        }

        /* Write a SEGMENT_BEGIN */
        segmentid sid = append_segment_flag(bv, CHUNK_SEGMENT_START, s->chunk_num);

        /* Write recipe */
    	iter = g_sequence_get_begin_iter(s->chunks);
    	end = g_sequence_get_end_iter(s->chunks);
        for (; iter != end; iter = g_sequence_iter_next(iter)) {
            c = g_sequence_get(iter);

        	if(r == NULL){
        		assert(CHECK_CHUNK(c,CHUNK_FILE_START));
        		r = new_file_recipe_meta(c->data);
        	}else if(!CHECK_CHUNK(c,CHUNK_FILE_END)){
        		struct chunkPointer cp;
        		cp.id = c->id;
        		assert(cp.id>=0);
        		memcpy(&cp.fp, &c->fp, sizeof(fingerprint));
        		cp.size = c->size;
        		append_n_chunk_pointers(bv, &cp ,1);
        		r->chunknum++;
        		r->filesize += c->size;

    	    	jcr.chunk_num++;
	    	    jcr.data_size += c->size;

        	}else{
        		assert(CHECK_CHUNK(c,CHUNK_FILE_END));
        		append_file_recipe_meta(bv, r);
        		free_file_recipe_meta(r);
        		r = NULL;

	            jcr.file_num++;
        	}
        }

       	/* Write a SEGMENT_END */
       	append_segment_flag(bv, CHUNK_SEGMENT_END, 0);

        free_segment(s);

        TIMER_END(1, jcr.filter_time);
        pthread_mutex_unlock(&index_lock.mutex);

    }

    if (storage_buffer.container_buffer
    		&& !container_empty(storage_buffer.container_buffer)){
        flush_container();
    }

    /* All files done */
    jcr.status = JCR_STATUS_DONE;
    return NULL;
}

static void* filter_thread_2D(void* arg) {
    struct fileRecipeMeta* r = NULL;
	struct backupVersion* bv = jcr.new_bv;
    GHashTable *htb = NULL;
    upgrade_index_kv_t *kv = malloc(sizeof(upgrade_index_kv_t) * MAX_META_PER_CONTAINER); // sql insertion buffer
    int kv_num = 0;
	GSequence *file_chunks = NULL;
	int in_container = FALSE;

    DECLARE_TIME_RECORDER("filter_thread");

	while (1) {
		struct chunk* c = sync_queue_pop(hash_queue);

		if (c == NULL) {
			sync_queue_term(hash_queue);
			break;
		}

        TIMER_DECLARE(1);
        TIMER_BEGIN(1);

        BEGIN_TIME_RECORD

		if (CHECK_CHUNK(c, CHUNK_FILE_START)) {
			assert(r == NULL);
			r = new_file_recipe_meta(c->data); // filename
			file_chunks = g_sequence_new(free_chunk);
			free_chunk(c);
		} else if (CHECK_CHUNK(c, CHUNK_FILE_END)) {
            assert(!in_container);
			free_chunk(c);

			append_segment_flag(bv, CHUNK_SEGMENT_START, g_sequence_get_length(file_chunks));
			// iter seq
			GSequenceIter *iter = g_sequence_get_begin_iter(file_chunks);
			GSequenceIter *end = g_sequence_get_end_iter(file_chunks);
			for (; iter != end; iter = g_sequence_iter_next(iter)) {
				c = g_sequence_get(iter);
				assert(CHECK_CHUNK(c, CHUNK_DUPLICATE));
				struct chunkPointer cp;
        		cp.id = c->id;
        		assert(cp.id>=0);
        		memcpy(&cp.fp, &c->fp, sizeof(fingerprint));
        		cp.size = c->size;
        		append_n_chunk_pointers(bv, &cp ,1);
        		r->chunknum++;
        		r->filesize += c->size;

    	    	jcr.chunk_num++;
	    	    jcr.data_size += c->size;
			}
			append_file_recipe_meta(bv, r);
			free_file_recipe_meta(r);
			r = NULL;

			append_segment_flag(bv, CHUNK_SEGMENT_END, 0);
			jcr.file_num++;
            g_sequence_free(file_chunks);
		} else if (CHECK_CHUNK(c, CHUNK_CONTAINER_START)) {
			assert(!in_container);
			in_container = TRUE;
            assert(htb == NULL);
            htb = g_hash_table_new_full(g_feature_hash, g_feature_equal, free, NULL);
			kv_num = 0;
            free_chunk(c);
		} else if (CHECK_CHUNK(c, CHUNK_CONTAINER_END)) {
			assert(in_container);
			in_container = FALSE;
            assert(c->id>=0);
            pthread_mutex_lock(&upgrade_index_lock.mutex);
            setDB(DB_UPGRADE, (char *)&c->id, sizeof(containerid), (char *)kv, kv_num * sizeof(upgrade_index_kv_t));
            if (upgrade_storage_buffer) {
                upgrade_fingerprint_cache_insert(upgrade_storage_buffer_id, upgrade_storage_buffer);
            }
            upgrade_storage_buffer = htb;
            upgrade_storage_buffer_id = c->id;
            htb = NULL;
            g_hash_table_remove(upgrade_processing, &c->id);
            pthread_mutex_unlock(&upgrade_index_lock.mutex);
            DEBUG("Insert id: %ld %d records %ldB into sql", c->id, kv_num, kv_num * sizeof(upgrade_index_kv_t));
            free_chunk(c);
		} else if (in_container){
			// container chunks
			append_chunk_to_buffer(c);
			assert(c->id >= 0);

            upgrade_index_kv_t *kvp;

            kvp = (upgrade_index_kv_t *)malloc(sizeof(upgrade_index_kv_t));
            memcpy(kvp->old_fp, c->old_fp, sizeof(fingerprint));
            memcpy(kvp->value.fp, c->fp, sizeof(fingerprint));
            kvp->value.id = c->id;
            g_hash_table_insert(htb, &kvp->old_fp, &kvp->value);

            memcpy(&kv[kv_num], kvp, sizeof(upgrade_index_kv_t));
            ++kv_num;

            free_chunk(c);
		} else {
			// recipe chunks
            if (CHECK_CHUNK(c, CHUNK_PROCESSING)) {
                // 使用标记而不是id为TEMPORARY_ID来标记这个chunk在pre_dedup时正在处理
                // 可以避免LRU过小时刚生成的container直接被挤出去的问题
                UNSET_CHUNK(c, CHUNK_DUPLICATE);
            }
            if (!CHECK_CHUNK(c, CHUNK_DUPLICATE)) {
                // c->id == TEMPORARY_ID 代表这个chunk在pre_dedup时还在处理
                pthread_mutex_lock(&upgrade_index_lock.mutex);
                upgrade_index_lookup_2D_filter(c);
                pthread_mutex_unlock(&upgrade_index_lock.mutex);
            }
            assert(CHECK_CHUNK(c, CHUNK_DUPLICATE));
            assert(c->id >= 0);
            
			g_sequence_append(file_chunks, c);
		}
        END_TIME_RECORD
        TIMER_END(1, jcr.filter_time);
	}
    if (storage_buffer.container_buffer
    		&& !container_empty(storage_buffer.container_buffer)){
        flush_container();
    }
    /* All files done */
    FINISH_TIME_RECORD
    free(kv);
    jcr.status = JCR_STATUS_DONE;
    return NULL;
}

static void* filter_thread_constrained(void* arg) {
    struct fileRecipeMeta* r = NULL;
	struct backupVersion* bv = jcr.new_bv;
    GHashTable *htb = NULL;
    int kv_buffer_size = MAX_META_PER_CONTAINER * 3;
    upgrade_index_kv_t *kv = malloc(sizeof(upgrade_index_kv_t) * kv_buffer_size); // sql insertion buffer
    int kv_num = 0;
	GSequence *file_chunks = NULL;
	int in_container = FALSE;
    containerid container_begin = -1;
    int16_t container_num = 0;

    DECLARE_TIME_RECORDER("filter_thread");

	while (1) {
		struct chunk* c = sync_queue_pop(hash_queue);

		if (c == NULL) {
			sync_queue_term(hash_queue);
			break;
		}

        TIMER_DECLARE(1);
        TIMER_BEGIN(1);

        BEGIN_TIME_RECORD

		if (CHECK_CHUNK(c, CHUNK_FILE_START)) {
            assert(!in_container);
			assert(r == NULL);
			r = new_file_recipe_meta(c->data); // filename
			file_chunks = g_sequence_new(free_chunk);
			free_chunk(c);
		} else if (CHECK_CHUNK(c, CHUNK_FILE_END)) {
            assert(!in_container);
			free_chunk(c);

			append_segment_flag(bv, CHUNK_SEGMENT_START, g_sequence_get_length(file_chunks));
			// iter seq
			GSequenceIter *iter = g_sequence_get_begin_iter(file_chunks);
			GSequenceIter *end = g_sequence_get_end_iter(file_chunks);
			for (; iter != end; iter = g_sequence_iter_next(iter)) {
				c = g_sequence_get(iter);
				assert(CHECK_CHUNK(c, CHUNK_DUPLICATE));
				struct chunkPointer cp;
        		cp.id = c->id;
        		assert(cp.id>=0);
        		memcpy(&cp.fp, &c->fp, sizeof(fingerprint));
        		cp.size = c->size;
        		append_n_chunk_pointers(bv, &cp ,1);
        		r->chunknum++;
        		r->filesize += c->size;

    	    	jcr.chunk_num++;
	    	    jcr.data_size += c->size;
			}
			append_file_recipe_meta(bv, r);
			free_file_recipe_meta(r);
			r = NULL;

			append_segment_flag(bv, CHUNK_SEGMENT_END, 0);
			jcr.file_num++;
            g_sequence_free(file_chunks);
		} else if (CHECK_CHUNK(c, CHUNK_CONTAINER_START)) {
			assert(!in_container);
			in_container = TRUE;
            assert(htb == NULL);
            htb = g_hash_table_new_full(g_feature_hash, g_feature_equal, free, NULL);
			kv_num = 0;
            free_chunk(c);
		} else if (CHECK_CHUNK(c, CHUNK_CONTAINER_END)) {
			assert(in_container);
			in_container = FALSE;
            assert(c->id>=0);
            pthread_mutex_lock(&upgrade_index_lock.mutex);

            if (!CHECK_CHUNK(c, CHUNK_REPROCESS)) {
                // add to containerMap
                struct containerMap *cm = malloc(sizeof(struct containerMap));
                cm->old_id = c->id;
                cm->new_id = container_begin;
                cm->container_num = container_num;
                g_hash_table_insert(upgrade_container, &cm->old_id, cm);
                DEBUG("container id: %ld, container_begin: %ld, container_num: %d\n", c->id, container_begin, container_num);
                container_begin = -1;
                container_num = 0;

                setDB(DB_UPGRADE, (char *)&c->id, sizeof(containerid), (char *)kv, kv_num * sizeof(upgrade_index_kv_t));
            } else {
                // 检查热process的逻辑是否正确 随时可删
                assert(container_begin == -1);
                assert(container_num == 0);
            }
            if (upgrade_storage_buffer) {
                upgrade_fingerprint_cache_insert(upgrade_storage_buffer_id, upgrade_storage_buffer);
            }
            upgrade_storage_buffer = htb;
            upgrade_storage_buffer_id = c->id;
            htb = NULL;
            g_hash_table_remove(upgrade_processing, &c->id);
            pthread_mutex_unlock(&upgrade_index_lock.mutex);
            DEBUG("Insert id: %ld %d records %ldB into sql", c->id, kv_num, kv_num * sizeof(upgrade_index_kv_t));
            free_chunk(c);
		} else if (in_container){
			// container chunks
            if (!CHECK_CHUNK(c, CHUNK_REPROCESS)) {
                append_chunk_to_buffer(c);
                assert(c->id >= 0);
                if (container_begin == -1) {
                    container_begin = c->id;
                }
                containerid new_num = c->id - container_begin + 1;
                assert(container_num <= new_num);
                container_num = new_num;
                assert(container_num <= 3);
            }

            upgrade_index_kv_t *kvp;
            kvp = (upgrade_index_kv_t *)malloc(sizeof(upgrade_index_kv_t));
            memcpy(kvp->old_fp, c->old_fp, sizeof(fingerprint));
            memcpy(kvp->value.fp, c->fp, sizeof(fingerprint));
            kvp->value.id = c->id;
            g_hash_table_insert(htb, &kvp->old_fp, &kvp->value);

            assert(kv_num <= kv_buffer_size);
            memcpy(&kv[kv_num], kvp, sizeof(upgrade_index_kv_t));
            ++kv_num;

            free_chunk(c);
		} else {
			// recipe chunks
            if (CHECK_CHUNK(c, CHUNK_PROCESSING)) {
                UNSET_CHUNK(c, CHUNK_DUPLICATE);
            }
            if (!CHECK_CHUNK(c, CHUNK_DUPLICATE)) {
                // c->id == TEMPORARY_ID 代表这个chunk在pre_dedup时还在处理
                pthread_mutex_lock(&upgrade_index_lock.mutex);
                upgrade_index_lookup_2D_filter(c);
                pthread_mutex_unlock(&upgrade_index_lock.mutex);
            }
            assert(CHECK_CHUNK(c, CHUNK_DUPLICATE));
            assert(c->id >= 0);
            
			g_sequence_append(file_chunks, c);
		}
        END_TIME_RECORD
        TIMER_END(1, jcr.filter_time);
	}
    if (storage_buffer.container_buffer
    		&& !container_empty(storage_buffer.container_buffer)){
        flush_container();
    }
    /* All files done */
    FINISH_TIME_RECORD
    free(kv);
    jcr.status = JCR_STATUS_DONE;
    return NULL;
}

void start_filter_phase() {

	storage_buffer.container_buffer = NULL;

    init_restore_aware();

    if (job == DESTOR_UPDATE) {
        if (destor.upgrade_level == UPGRADE_2D_RELATION) 
            pthread_create(&filter_t, NULL, filter_thread_2D, NULL);
        else if (destor.upgrade_level == UPGRADE_SIMILARITY || destor.upgrade_level == UPGRADE_2D_CONSTRAINED)
            pthread_create(&filter_t, NULL, filter_thread_constrained, NULL);
        else if (destor.upgrade_level == UPGRADE_1D_RELATION)
            pthread_create(&filter_t, NULL, filter_thread, NULL);
        else
            pthread_create(&filter_t, NULL, filter_thread, NULL);
    } else {
        pthread_create(&filter_t, NULL, filter_thread, NULL);
    }
}

void stop_filter_phase() {
    pthread_join(filter_t, NULL);
    if (destor.upgrade_level == UPGRADE_NAIVE || destor.upgrade_level == UPGRADE_1D_RELATION)
        close_har();
	NOTICE("filter phase stops successfully!");

}
