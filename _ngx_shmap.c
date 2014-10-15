/*************************************************
 * Author: jie123108@163.com
 * Copyright: jie123108
 *************************************************/
#ifndef NGX_SHMAP

#include "_ngx_shmap.h"
#include <assert.h>

#define NLOG_DEBUG(format, args...);
#define NLOG_ERROR(format, args...);
#define NLOG_INFO(format, args...);


static int ngx_shmap_set_helper(ngx_shm_zone_t* zone, ngx_str_t* key, ngx_str_t* value,
			uint8_t value_type, uint32_t exptime, uint32_t user_flags, int flags);
ngx_int_t ngx_shmap_init_zone(ngx_shm_zone_t *shm_zone, void *data);

#define NGX_SHARED_MAP_ADD         0x0001
#define NGX_SHARED_MAP_REPLACE     0x0002
#define NGX_SHARED_MAP_SAFE_STORE  0x0004
#define NGX_SHARED_MAP_DELETE	   0x0008

void ngx_str_set_int32(ngx_str_t* key, int32_t* value)
{
	key->len= sizeof(int32_t);
	key->data = (u_char*)value;
}
void ngx_str_set_int64(ngx_str_t* key, int64_t* value)
{
	key->len= sizeof(int64_t);
	key->data = (u_char*)value;
}
void ngx_str_set_double(ngx_str_t* key, double* value)
{
	key->len= sizeof(double);
	key->data = (u_char*)value;
}

static ngx_inline uint32_t ngx_shmap_crc32(u_char *p, size_t len)
{
	if(len == sizeof(ngx_int_t)){
		uint32_t* pi = (uint32_t*)p;
		return *pi;
	}else{
		return ngx_crc32_short(p, len);
	}
}

ngx_shm_zone_t* ngx_shmap_init(ngx_conf_t *cf, ngx_str_t* name, size_t size, void* module)
{
	ngx_shmap_ctx_t* 			ctx;
	ngx_shm_zone_t             *zone;
    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_shmap_ctx_t));
    if (ctx == NULL) {
        return NULL; 
    }

    zone = ngx_shared_memory_add(cf, name, size, module);
    if (zone == NULL) {
		ngx_pfree(cf->pool, ctx);
		ctx = NULL;
        return NULL;
    }

    ctx->name = *name;
    ctx->log = &cf->cycle->new_log;


    if (zone->data) {
        ctx = zone->data;

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "lua_shared_dict \"%V\" is already defined as "
                           "\"%V\"", name, &ctx->name);
        return NULL;
    }

    zone->init = ngx_shmap_init_zone;
    zone->data = ctx;

  	return zone;
}

static void ngx_shmap_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t          **p;
    ngx_shmap_node_t   *sdn, *sdnt;

    for ( ;; ) {

        if (node->key < temp->key) {

            p = &temp->left;

        } else if (node->key > temp->key) {

            p = &temp->right;

        } else { /* node->key == temp->key */

            sdn = (ngx_shmap_node_t *) &node->color;
            sdnt = (ngx_shmap_node_t *) &temp->color;

            p = ngx_memn2cmp(sdn->data, sdnt->data, sdn->key_len,
                             sdnt->key_len) < 0 ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


ngx_int_t
ngx_shmap_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_shmap_ctx_t  *octx = data;

    size_t                      len;
    ngx_shmap_ctx_t  *ctx;
    //ngx_http_lua_main_conf_t   *lmcf;

    //dd("init zone");

    ctx = shm_zone->data;

    if (octx) {
        ctx->sh = octx->sh;
        ctx->shpool = octx->shpool;

        goto done;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;

        goto done;
    }

    ctx->sh = ngx_slab_alloc(ctx->shpool, sizeof(ngx_shmap_shctx_t));
    if (ctx->sh == NULL) {
        return NGX_ERROR;
    }

    ctx->shpool->data = ctx->sh;

    ngx_rbtree_init(&ctx->sh->rbtree, &ctx->sh->sentinel,
                    ngx_shmap_rbtree_insert_value);

    ngx_queue_init(&ctx->sh->queue);

    len = sizeof(" in ngx_shared_map zone \"\"") + shm_zone->shm.name.len;

    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(ctx->shpool->log_ctx, " in ngx_shared_map zone \"%V\"%Z",
                &shm_zone->shm.name);

done:

    return NGX_OK;
}

static ngx_int_t
ngx_shmap_lookup(ngx_shm_zone_t *shm_zone, ngx_uint_t hash,
    u_char *kdata, size_t klen, ngx_shmap_node_t **sdp)
{
    ngx_int_t                    rc;
    ngx_time_t                  *tp;
    uint64_t                     now;
    int64_t                      ms;
    ngx_rbtree_node_t           *node, *sentinel;
    ngx_shmap_ctx_t   *ctx;
    ngx_shmap_node_t  *sd;

    ctx = shm_zone->data;

    node = ctx->sh->rbtree.root;
    sentinel = ctx->sh->rbtree.sentinel;

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        sd = (ngx_shmap_node_t *) &node->color;

        rc = ngx_memn2cmp(kdata, sd->data, klen, (size_t) sd->key_len);

        if (rc == 0) {
            ngx_queue_remove(&sd->queue);
            ngx_queue_insert_head(&ctx->sh->queue, &sd->queue);

            *sdp = sd;

            //dd("node expires: %lld", (long long) sd->expires);

            if (sd->expires != 0) {
                tp = ngx_timeofday();

                now = (uint64_t) tp->sec * 1000 + tp->msec;
                ms = sd->expires - now;

                //dd("time to live: %lld", (long long) ms);

                if (ms < 0) {
                    //dd("node already expired");
                    return NGX_DONE;
                }
            }

            return NGX_OK;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    *sdp = NULL;

    return NGX_DECLINED;
}


static int
ngx_shmap_expire(ngx_shmap_ctx_t *ctx, ngx_uint_t n)
{
    ngx_time_t                  *tp;
    uint64_t                     now;
    ngx_queue_t                 *q;
    int64_t                      ms;
    ngx_rbtree_node_t           *node;
    ngx_shmap_node_t  *sd;
    int                          freed = 0;

    tp = ngx_timeofday();

    now = (uint64_t) tp->sec * 1000 + tp->msec;

    /*
     * n == 1 deletes one or two expired entries
     * n == 0 deletes oldest entry by force
     *        and one or two zero rate entries
     */

    while (n < 3) {

        if (ngx_queue_empty(&ctx->sh->queue)) {
            return freed;
        }

        q = ngx_queue_last(&ctx->sh->queue);

        sd = ngx_queue_data(q, ngx_shmap_node_t, queue);

        if (n++ != 0) {

            if (sd->expires == 0) {
                return freed;
            }

            ms = sd->expires - now;
            if (ms > 0) {
                return freed;
            }
        }

        ngx_queue_remove(q);

        node = (ngx_rbtree_node_t *)
                   ((u_char *) sd - offsetof(ngx_rbtree_node_t, color));

        ngx_rbtree_delete(&ctx->sh->rbtree, node);

        ngx_slab_free_locked(ctx->shpool, node);

        freed++;
    }

    return freed;
}

int ngx_shmap_get_int32(ngx_shm_zone_t* zone, ngx_str_t* key, int32_t* i)
{
	uint8_t value_type = VT_NULL;
	ngx_str_t data = ngx_null_string;
	int ret = ngx_shmap_get(zone, key, &data, &value_type,NULL,NULL);
	if(ret == 0){
		if(value_type != VT_INT32){
			ret = -1;
			NLOG_ERROR("ngx_shmap_get_int32(key=%V) return invalid value_type=%d",
						key, value_type);
		}else{
			int32_t* p = (int32_t*)data.data;
			*i = *p;
		}
	}
	return ret;
}

int ngx_shmap_get_int64(ngx_shm_zone_t* zone, ngx_str_t* key, int64_t* i)
{
	uint8_t value_type = VT_NULL;
	ngx_str_t data = ngx_null_string;
	int ret = ngx_shmap_get(zone, key, &data, &value_type,NULL,NULL);
	if(ret == 0){
		if(value_type != VT_INT64){
			ret = -1;
			NLOG_ERROR("ngx_shmap_get_int64(key=%V) return invalid value_type=%d",
						key, value_type);
		}else{
			int64_t* p = (int64_t*)data.data;
			*i = *p;
		}
	}
	return ret;
}

int ngx_shmap_get_int64_and_clear(ngx_shm_zone_t* zone, ngx_str_t* key, int64_t* i)
{
	uint8_t value_type = VT_NULL;
	ngx_str_t data = ngx_null_string;
	int ret = ngx_shmap_get(zone, key, &data, &value_type,NULL,NULL);
	if(ret == 0){
		if(value_type != VT_INT64){
			ret = -1;
			NLOG_ERROR("ngx_shmap_get_int64(key=%V) return invalid value_type=%d",
						key, value_type);
		}else{
			int64_t* p = (int64_t*)data.data;
			*i = __sync_fetch_and_and(p, 0);
		}
	}
	return ret;
}

int ngx_shmap_get(ngx_shm_zone_t* zone, ngx_str_t* key, 
		ngx_str_t* data, uint8_t* value_type,uint32_t* exptime,
		uint32_t* user_flags)
{
    ngx_str_t                    name;
    uint32_t                     hash;
    ngx_int_t                    rc;
    ngx_shmap_ctx_t   *ctx;
    ngx_shmap_node_t  *sd;
	assert(zone != NULL);
	assert(key != NULL);
	assert(data != NULL);
	
    ctx = zone->data;

    name = ctx->name;

    if (key->len == 0 || key->len > 65535) {
        return -1;
    }

    hash = ngx_shmap_crc32(key->data, key->len);

    ngx_shmtx_lock(&ctx->shpool->mutex);

#if 1
    ngx_shmap_expire(ctx, 1);
#endif
 
    rc = ngx_shmap_lookup(zone, hash, key->data, key->len, &sd);

    //dd("shdict lookup returns %d", (int) rc);

    if (rc == NGX_DECLINED || rc == NGX_DONE) {
        ngx_shmtx_unlock(&ctx->shpool->mutex);
        return -1;
    }

    /* rc == NGX_OK */


    //dd("data: %p", sd->data);
    //dd("key len: %d", (int) sd->key_len);

    data->data = sd->data + sd->key_len;
    data->len = (size_t) sd->value_len;

	if(value_type) *value_type = sd->value_type;
    if(user_flags) *user_flags = sd->user_flags;
	if(exptime){
		if(sd->expires == 0){
			*exptime = 0;
		}else{
			ngx_time_t* tp = ngx_timeofday();
			*exptime = (sd->expires-((uint64_t) tp->sec * 1000 + tp->msec))/1000;
		}
	}
    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return 0;
}

int ngx_shmap_get_ex(ngx_shm_zone_t* zone, ngx_str_t* key, 
		ngx_str_t* data, uint8_t* value_type,uint32_t* exptime,
		uint32_t** user_flags)
{
    ngx_str_t                    name;
    uint32_t                     hash;
    ngx_int_t                    rc;
    ngx_shmap_ctx_t   *ctx;
    ngx_shmap_node_t  *sd;
	assert(zone != NULL);
	assert(key != NULL);
	assert(data != NULL);
	
    ctx = zone->data;

    name = ctx->name;

    if (key->len == 0 || key->len > 65535) {
        return -1;
    }

    hash = ngx_shmap_crc32(key->data, key->len);

    ngx_shmtx_lock(&ctx->shpool->mutex);

#if 1
    ngx_shmap_expire(ctx, 1);
#endif
 
    rc = ngx_shmap_lookup(zone, hash, key->data, key->len, &sd);

    //dd("shdict lookup returns %d", (int) rc);

    if (rc == NGX_DECLINED || rc == NGX_DONE) {
        ngx_shmtx_unlock(&ctx->shpool->mutex);
        return -1;
    }

    /* rc == NGX_OK */


    //dd("data: %p", sd->data);
    //dd("key len: %d", (int) sd->key_len);

    data->data = sd->data + sd->key_len;
    data->len = (size_t) sd->value_len;

	if(value_type) *value_type = sd->value_type;
    if(user_flags) *user_flags = &sd->user_flags;
	if(exptime){ 
		if(sd->expires == 0){
			*exptime = 0;
		}else{
			ngx_time_t* tp = ngx_timeofday();
			*exptime = (sd->expires-((uint64_t) tp->sec * 1000 + tp->msec))/1000;
		}
	}
    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return 0;
}

int ngx_shmap_delete(ngx_shm_zone_t* zone, ngx_str_t* key)
{
	assert(zone != NULL);
	assert(key != NULL);
   	return ngx_shmap_set_helper(zone,key, NULL, VT_NULL,0,0,NGX_SHARED_MAP_DELETE);
}
 

int ngx_shmap_flush_all(ngx_shm_zone_t* zone)
{
    ngx_queue_t                 *q;
    ngx_shmap_node_t  *sd;
    ngx_shmap_ctx_t   *ctx;
	assert(zone != NULL);

    ctx = zone->data;

    ngx_shmtx_lock(&ctx->shpool->mutex);

    for (q = ngx_queue_head(&ctx->sh->queue);
         q != ngx_queue_sentinel(&ctx->sh->queue);
         q = ngx_queue_next(q))
    {
        sd = ngx_queue_data(q, ngx_shmap_node_t, queue);
        sd->expires = 1;
    }

    ngx_shmap_expire(ctx, 0);

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return 0;
}


int ngx_shmap_flush_expired(ngx_shm_zone_t* zone, int attempts)
{
    ngx_queue_t                 *q, *prev;
    ngx_shmap_node_t  *sd;
    ngx_shmap_ctx_t   *ctx;
    ngx_time_t                  *tp;
    int                          freed = 0;
    ngx_rbtree_node_t           *node;
    uint64_t                     now;
	assert(zone != NULL);

    ctx = zone->data;

    ngx_shmtx_lock(&ctx->shpool->mutex);

    if (ngx_queue_empty(&ctx->sh->queue)) {
        return 0;
    }

    tp = ngx_timeofday();

    now = (uint64_t) tp->sec * 1000 + tp->msec;

    q = ngx_queue_last(&ctx->sh->queue);

    while (q != ngx_queue_sentinel(&ctx->sh->queue)) {
        prev = ngx_queue_prev(q);

        sd = ngx_queue_data(q, ngx_shmap_node_t, queue);

        if (sd->expires != 0 && sd->expires <= now) {
            ngx_queue_remove(q);

            node = (ngx_rbtree_node_t *)
                ((u_char *) sd - offsetof(ngx_rbtree_node_t, color));

            ngx_rbtree_delete(&ctx->sh->rbtree, node);
            ngx_slab_free_locked(ctx->shpool, node);
            freed++;

            if (attempts && freed == attempts) {
                break;
            }
        }

        q = prev;
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return freed;
}

int ngx_shmap_add(ngx_shm_zone_t* zone, ngx_str_t* key, ngx_str_t* value,
			uint8_t value_type, uint32_t exptime, uint32_t user_flags)
{
	assert(zone != NULL);
	assert(key != NULL);
	assert(value != NULL);
    return ngx_shmap_set_helper(zone,key,value,value_type,exptime,user_flags, 
						NGX_SHARED_MAP_ADD);
}


int ngx_shmap_safe_add(ngx_shm_zone_t* zone, ngx_str_t* key, ngx_str_t* value,
			uint8_t value_type, uint32_t exptime, uint32_t user_flags)
{
	assert(zone != NULL);
	assert(key != NULL);
	assert(value != NULL);

    return ngx_shmap_set_helper(zone,key,value,value_type,exptime,user_flags, 
				NGX_SHARED_MAP_ADD|NGX_SHARED_MAP_SAFE_STORE);
}


int ngx_shmap_replace(ngx_shm_zone_t* zone, ngx_str_t* key, ngx_str_t* value,
			uint8_t value_type, uint32_t exptime, uint32_t user_flags)
{
	assert(zone != NULL);
	assert(key != NULL);
	assert(value != NULL);

    return ngx_shmap_set_helper(zone,key,value,value_type,exptime,user_flags, NGX_SHARED_MAP_REPLACE);
}


int ngx_shmap_set(ngx_shm_zone_t* zone, ngx_str_t* key, ngx_str_t* value,
			uint8_t value_type, uint32_t exptime, uint32_t user_flags)
{
	assert(zone != NULL);
	assert(key != NULL);
	assert(value != NULL);

    return ngx_shmap_set_helper(zone,key,value,value_type,exptime,user_flags,0);
}


int ngx_shmap_safe_set(ngx_shm_zone_t* zone, ngx_str_t* key, ngx_str_t* value,
			uint8_t value_type, uint32_t exptime, uint32_t user_flags)
{
	assert(zone != NULL);
	assert(key != NULL);
	assert(value != NULL);

    return ngx_shmap_set_helper(zone,key,value,value_type,exptime,user_flags,NGX_SHARED_MAP_SAFE_STORE);
}


int ngx_shmap_inc_int(ngx_shm_zone_t* zone, ngx_str_t* key,int64_t i,uint32_t exptime, int64_t* ret)
{
	assert(zone != NULL);
	assert(key != NULL);
	assert(ret != NULL);
	
	ngx_int_t rc = 0;
	ngx_str_t data = ngx_null_string;
	uint8_t value_type = VT_INT64;
	rc = ngx_shmap_get(zone, key, &data, &value_type, NULL,NULL);
	if(rc == 0){
		if(value_type != VT_INT64){
			NLOG_ERROR("key [%V] value_type [%d] invalid!",
					key, value_type);
			return -1;
		}
		int64_t* p = (int64_t*)data.data;
		*ret = __sync_add_and_fetch(p, i);
	}else{
		//不存在，插入新的
		ngx_str_set_int64(&data, &i);
		rc =ngx_shmap_set(zone, key, &data, value_type, exptime,0);
		if(rc == 0){
			*ret = i;
		}
	}
	return rc;
}

int ngx_shmap_inc_double(ngx_shm_zone_t* zone, ngx_str_t* key,double d,uint32_t exptime,double* ret)
{
	ngx_int_t rc = 0;
	ngx_str_t data = ngx_null_string;
	assert(zone != NULL);
	assert(key != NULL);
	assert(ret != NULL);
	
	uint8_t value_type = VT_DOUBLE;
	rc = ngx_shmap_get(zone, key, &data, &value_type, NULL,NULL);
	if(rc == 0){
		if(value_type != VT_DOUBLE){
			NLOG_ERROR("key [%V] value_type [%d] invalid!",
					key, value_type);
			return -1;
		}
		double* p = (double*)data.data;
		//要改成原子操作
		*ret = (*p += d);
	}else{
		//不存在，插入新的
		ngx_str_set_double(&data, &d);
		rc =ngx_shmap_set(zone, key, &data, value_type, exptime,0);
		if(rc == 0){
			*ret = d;
		}
	}
	return rc;
}


static int ngx_shmap_set_helper(ngx_shm_zone_t* zone, ngx_str_t* key, ngx_str_t* value,
			uint8_t value_type, uint32_t exptime, uint32_t user_flags, int flags)
{
    int                          i, n;
    ngx_str_t                    name;
    uint32_t                     hash;
    ngx_int_t                    rc;
    ngx_shmap_ctx_t   *ctx;
    ngx_shmap_node_t  *sd;
    u_char                      *p;
    ngx_rbtree_node_t           *node;
    ngx_time_t                  *tp;
    int                          forcible = 0;
                         /* indicates whether to foricibly override other
                          * valid entries */

    ctx = zone->data;
 
    name = ctx->name;

    if (key->len == 0 || key->len > 65535) {
        return -1;
    }

    hash = ngx_shmap_crc32(key->data, key->len);

    //dd("looking up key %s in shared dict %s", key->data, name.data);

    ngx_shmtx_lock(&ctx->shpool->mutex);

#if 1
    ngx_shmap_expire(ctx, 1);
#endif

    rc = ngx_shmap_lookup(zone, hash, key->data, key->len, &sd);

    NLOG_DEBUG("shdict lookup returned %d", (int) rc);

	if (flags & NGX_SHARED_MAP_DELETE) {
		if (rc == NGX_DECLINED || rc == NGX_DONE) {
            ngx_shmtx_unlock(&ctx->shpool->mutex);
			//not exists
			return 0;
		}
	}

    if (flags & NGX_SHARED_MAP_REPLACE) {

        if (rc == NGX_DECLINED || rc == NGX_DONE) {
            ngx_shmtx_unlock(&ctx->shpool->mutex);
			//not exists
			return -1;
        }

        /* rc == NGX_OK */

        goto replace;
    }

    if (flags & NGX_SHARED_MAP_ADD) {

        if (rc == NGX_OK) {
            ngx_shmtx_unlock(&ctx->shpool->mutex);
			//exists
            return -1;
        }

        if (rc == NGX_DONE) {
            /* exists but expired */
            //dd("go to replace");
            goto replace;
        }

        /* rc == NGX_DECLINED */
        //dd("go to insert");
        goto insert;
    }

    if (rc == NGX_OK || rc == NGX_DONE) {

        if (value_type == VT_NULL) {
            goto remove;
        }

replace:
        if (value->data && value->len == (size_t) sd->value_len) {
            NLOG_DEBUG("shmap set: found old entry and value size matched, reusing it");

            ngx_queue_remove(&sd->queue);
            ngx_queue_insert_head(&ctx->sh->queue, &sd->queue);

            sd->key_len = key->len;

            if (exptime > 0) {
                tp = ngx_timeofday();
                sd->expires = (uint64_t) tp->sec * 1000 + tp->msec
                              + exptime * 1000;
            } else {
                sd->expires = 0;
            }
			//NLOG_DEBUG("sd->expires: %u", sd->expires);
            sd->user_flags = user_flags;

            sd->value_len = (uint32_t) value->len;

            //dd("setting value type to %d", value_type);

            sd->value_type = value_type;

            p = ngx_copy(sd->data, key->data, key->len);
            ngx_memcpy(p, value->data, value->len);

            ngx_shmtx_unlock(&ctx->shpool->mutex);

            return 0;
        }

        NLOG_DEBUG("shmap set: found old entry but value size "
                       "NOT matched, removing it first");

remove:
        ngx_queue_remove(&sd->queue);

        node = (ngx_rbtree_node_t *)
                   ((u_char *) sd - offsetof(ngx_rbtree_node_t, color));

        ngx_rbtree_delete(&ctx->sh->rbtree, node);

        ngx_slab_free_locked(ctx->shpool, node);
        if (value_type == VT_NULL) {
			ngx_shmtx_unlock(&ctx->shpool->mutex);
			return 0;
        }
    }

insert:
    /* rc == NGX_DECLINED or value size unmatch */

    if (value == NULL || value->data == NULL) {
        ngx_shmtx_unlock(&ctx->shpool->mutex);
		NLOG_ERROR("shmap add failed! value is null!");
        return -1;
    }

    n = offsetof(ngx_rbtree_node_t, color)
        + offsetof(ngx_shmap_node_t, data)
        + key->len
        + value->len;

    //NLOG_DEBUG("shmap set: creating a new entry(size=%d)", n);

    node = ngx_slab_alloc_locked(ctx->shpool, n);

    if (node == NULL) {

        if (flags & NGX_SHARED_MAP_SAFE_STORE) {
            ngx_shmtx_unlock(&ctx->shpool->mutex);

			NLOG_ERROR("shmap add failed! no memory!");
			return -1;
        }

        NLOG_DEBUG("shmap set: overriding non-expired items "
                       "due to memory shortage for entry \"%V\"", &name);

        for (i = 0; i < 30; i++) {
            if (ngx_shmap_expire(ctx, 0) == 0) {
                break;
            }

            forcible = 1;

            node = ngx_slab_alloc_locked(ctx->shpool, n);
            if (node != NULL) {
                goto allocated;
            }
        }

        ngx_shmtx_unlock(&ctx->shpool->mutex);

		NLOG_ERROR("shmap add failed! no memory!");
		return -1;
    }

allocated:
    sd = (ngx_shmap_node_t *) &node->color;
 
    node->key = hash;
    sd->key_len = key->len;

    if (exptime > 0) {
        tp = ngx_timeofday();
        sd->expires = (uint64_t) tp->sec * 1000 + tp->msec
                      + exptime * 1000;

    } else {
        sd->expires = 0;
    }

    sd->user_flags = user_flags;

    sd->value_len = (uint32_t) value->len;

    sd->value_type = value_type;

    p = ngx_copy(sd->data, key->data, key->len);
    ngx_memcpy(p, value->data, value->len);

    ngx_rbtree_insert(&ctx->sh->rbtree, node);

    ngx_queue_insert_head(&ctx->sh->queue, &sd->queue);

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return 0;
}

int ngx_shmap_foreach(ngx_shm_zone_t* zone, foreach_pt func, void* args)
{
    ngx_queue_t                 *q;
    ngx_shmap_node_t  *sd;
    ngx_shmap_ctx_t   *ctx;
	assert(zone != NULL);

    ctx = zone->data;

    int locked = ngx_shmtx_trylock(&ctx->shpool->mutex);
	if (!locked){
		return -1;
	}
	
    for (q = ngx_queue_head(&ctx->sh->queue);
         q != ngx_queue_sentinel(&ctx->sh->queue);
         q = ngx_queue_next(q))
    {
        sd = ngx_queue_data(q, ngx_shmap_node_t, queue);
   		func(sd, args);
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return 0;
}

#if 0
static int
ngx_shmap_incr(lua_State *L)
{
    int                          n;
    ngx_str_t                    key;
    uint32_t                     hash;
    ngx_int_t                    rc;
    ngx_shmap_ctx_t   *ctx;
    ngx_shmap_node_t  *sd;
    lua_Number                   num;
    u_char                      *p;
    ngx_shm_zone_t              *zone;
    lua_Number                   value;

    n = lua_gettop(L);

    if (n != 3) {
        return luaL_error(L, "expecting 3 arguments, but only seen %d", n);
    }

    luaL_checktype(L, 1, LUA_TLIGHTUSERDATA);

    zone = lua_touserdata(L, 1);
    if (zone == NULL) {
        return luaL_error(L, "bad user data for the ngx_shm_zone_t pointer");
    }

    ctx = zone->data;

    key->data = (u_char *) luaL_checklstring(L, 2, &key->len);

    if (key->len == 0) {
        return luaL_error(L, "attempt to use empty keys");
    }

    if (key->len > 65535) {
        return luaL_error(L, "the key argument is more than 65535 bytes: %d",
                          (int) key->len);
    }

    hash = ngx_shmap_crc32(key->data, key->len);

    value = luaL_checknumber(L, 3);

    dd("looking up key %.*s in shared dict %.*s", (int) key->len, key->data,
       (int) ctx->name.len, ctx->name.data);

    ngx_shmtx_lock(&ctx->shpool->mutex);

    ngx_shmap_expire(ctx, 1);

    rc = ngx_shmap_lookup(zone, hash, key->data, key->len, &sd);

    dd("shdict lookup returned %d", (int) rc);

    if (rc == NGX_DECLINED || rc == NGX_DONE) {
        ngx_shmtx_unlock(&ctx->shpool->mutex);

        lua_pushnil(L);
        lua_pushliteral(L, "not found");
        return 2;
    }

    /* rc == NGX_OK */

    if (sd->value_type != LUA_TNUMBER || sd->value_len != sizeof(lua_Number)) {
        ngx_shmtx_unlock(&ctx->shpool->mutex);

        lua_pushnil(L);
        lua_pushliteral(L, "not a number");
        return 2;
    }

    ngx_queue_remove(&sd->queue);
    ngx_queue_insert_head(&ctx->sh->queue, &sd->queue);

    dd("setting value type to %d", (int) sd->value_type);

    p = sd->data + key->len;

    num = *(lua_Number *) p;
    num += value;

    ngx_memcpy(p, (lua_Number *) &num, sizeof(lua_Number));

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    lua_pushnumber(L, num);
    lua_pushnil(L);
    return 2;
}


ngx_int_t
ngx_http_lua_shared_dict_get(ngx_shm_zone_t *zone, u_char *key_data,
    size_t key_len, ngx_http_lua_value_t *value)
{
    u_char                      *data;
    size_t                       len;
    uint32_t                     hash;
    ngx_int_t                    rc;
    ngx_shmap_ctx_t   *ctx;
    ngx_shmap_node_t  *sd;

    if (zone == NULL) {
        return NGX_ERROR;
    }

    hash = ngx_shmap_crc32(key_data, key_len);

    ctx = zone->data;

    ngx_shmtx_lock(&ctx->shpool->mutex);

    rc = ngx_shmap_lookup(zone, hash, key_data, key_len, &sd);

    dd("shdict lookup returned %d", (int) rc);

    if (rc == NGX_DECLINED || rc == NGX_DONE) {
        ngx_shmtx_unlock(&ctx->shpool->mutex);

        return rc;
    }

    /* rc == NGX_OK */

    value->type = sd->value_type;

    dd("type: %d", (int) value->type);

    data = sd->data + sd->key_len;
    len = (size_t) sd->value_len;

    switch (value->type) {
    case LUA_TSTRING:

        if (value->value->s.data == NULL || value->value->s.len == 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "no string buffer "
                          "initialized");
            return NGX_ERROR;
        }

        if (len > value->value->s.len) {
            len = value->value->s.len;

        } else {
            value->value->s.len = len;
        }

        ngx_memcpy(value->value->s.data, data, len);
        break;

    case LUA_TNUMBER:

        if (len != sizeof(lua_Number)) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "bad lua number "
                          "value size found for key %*s: %lu", key_len,
                          key_data, (unsigned long) len);

            ngx_shmtx_unlock(&ctx->shpool->mutex);
            return NGX_ERROR;
        }

        ngx_memcpy(&value->value->b, data, len);
        break;

    case LUA_TBOOLEAN:

        if (len != sizeof(u_char)) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "bad lua boolean "
                          "value size found for key %*s: %lu", key_len,
                          key_data, (unsigned long) len);

            ngx_shmtx_unlock(&ctx->shpool->mutex);
            return NGX_ERROR;
        }

        value->value->b = *data;
        break;

    default:
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "bad lua value type "
                      "found for key %*s: %d", key_len, key_data,
                      (int) value->type);

        ngx_shmtx_unlock(&ctx->shpool->mutex);
        return NGX_ERROR;
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);
    return NGX_OK;
}


ngx_shm_zone_t *
ngx_http_lua_find_zone(u_char *name_data, size_t name_len)
{
    ngx_str_t                       *name;
    ngx_uint_t                       i;
    ngx_shm_zone_t                  *zone;
    volatile ngx_list_part_t        *part;

    part = &ngx_cycle->shared_memory.part;
    zone = part->elts;

    for (i = 0; /* void */ ; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            zone = part->elts;
            i = 0;
        }

        name = &zone[i].shm.name;

        dd("name: [%.*s] %d", (int) name->len, name->data, (int) name->len);
        dd("name2: [%.*s] %d", (int) name_len, name_data, (int) name_len);

        if (name->len == name_len
            && ngx_strncmp(name->data, name_data, name_len) == 0)
        {
            return zone;
        }
    }

    return NULL;
}
#endif
#endif