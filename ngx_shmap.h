
#ifndef __NGX_SHMAP_H__
#define __NGX_SHMAP_H__
/*************************************************
 * Author: jie123108@163.com
 * Copyright: jie123108
 *************************************************/
#include <ngx_config.h>
#include <ngx_core.h>
#include <stdint.h>

#define VT_BINARY 0
#define VT_INT32 1
#define VT_INT64 2
#define VT_DOUBLE 3
#define VT_STRING 4
#define VT_NULL 5

#pragma pack(push) //保存对齐状态
#pragma pack(4) //设置4字节对齐
typedef struct {
    u_char                       color;
    u_char                       dummy;
    u_short                      key_len;
    ngx_queue_t                  queue;
    uint64_t                     expires;  //过期的绝对时间(毫秒)(函数参数中的exptime都是秒)
    uint32_t                     value_len;
    uint32_t                     user_flags;
    uint8_t                      value_type;
    u_char                       data[1];
} ngx_shmap_node_t;
#pragma pack(pop) //恢复对齐状态。

typedef struct {
    ngx_rbtree_t                  rbtree;
    ngx_rbtree_node_t             sentinel;
    ngx_queue_t                   queue;
} ngx_shmap_shctx_t;


typedef struct {
    ngx_shmap_shctx_t  *sh;
    ngx_slab_pool_t              *shpool;
    ngx_str_t                     name;
    //ngx_http_lua_main_conf_t     *main_conf;
    ngx_log_t                    *log;
} ngx_shmap_ctx_t;

/**
 * 初始化共享内存字典
 **/
ngx_shm_zone_t* ngx_shmap_init(ngx_conf_t *cf, ngx_str_t* name, size_t size, void* module);

/**
 * 取得一个key的值。
 * key 为字典的key.
 * data 为取得的数据(取得的数据是直接指向共享内存区的，
 *          所以如果你修改了该数据，共享内存中的数据也会被修改)
 * value_type 为数据的类型
 * exptime 为还有多久过期(秒)
 * user_flags 返回设置的user_flags值
 **/
int ngx_shmap_get(ngx_shm_zone_t* zone, ngx_str_t* key, 
		ngx_str_t* data, uint8_t* value_type,uint32_t* exptime,uint32_t* user_flags);

/**
 * 与ngx_shmap_get相同，取得一个key的值。
 * 与ngx_shmap_get不同之处在于user_flags返回的是设置的
 *    user_flags的指针，可以在获取后对user_flags进行修改。
 **/
int ngx_shmap_get_ex(ngx_shm_zone_t* zone, ngx_str_t* key, 
		ngx_str_t* data, uint8_t* value_type,uint32_t* exptime,uint32_t** user_flags);

int ngx_shmap_get_int32(ngx_shm_zone_t* zone, ngx_str_t* key, int32_t* i);
int ngx_shmap_get_int64(ngx_shm_zone_t* zone, ngx_str_t* key, int64_t* i);
int ngx_shmap_get_int64_and_clear(ngx_shm_zone_t* zone, 
				ngx_str_t* key, int64_t* i);
// 删除一个KEY
int ngx_shmap_delete(ngx_shm_zone_t* zone, ngx_str_t* key);

typedef void (*foreach_pt)(ngx_shmap_node_t* node, void* extarg);

/**
 * 循环处理该字典
 */
int ngx_shmap_foreach(ngx_shm_zone_t* zone, foreach_pt func, void* args);

//清空整个字典
int ngx_shmap_flush_all(ngx_shm_zone_t* zone);
//清空过期的key
int ngx_shmap_flush_expired(ngx_shm_zone_t* zone, int attempts);
//添加一个key,value, 如果存在会报错(空间不够时，会删除最早过期的数据)
int ngx_shmap_add(ngx_shm_zone_t* zone, ngx_str_t* key, ngx_str_t* value,
			uint8_t value_type, uint32_t exptime, uint32_t user_flags);
//添加一个key,value, 如果存在会报错(空间不够时，会返回失败)
int ngx_shmap_safe_add(ngx_shm_zone_t* zone, ngx_str_t* key, ngx_str_t* value,
			uint8_t value_type, uint32_t exptime, uint32_t user_flags);
//替换一个key,value
int ngx_shmap_replace(ngx_shm_zone_t* zone, ngx_str_t* key, ngx_str_t* value,
			uint8_t value_type, uint32_t exptime, uint32_t user_flags);
//设置一个key,value.
int ngx_shmap_set(ngx_shm_zone_t* zone, ngx_str_t* key, ngx_str_t* value,
			uint8_t value_type, uint32_t exptime, uint32_t user_flags);

//给key增加i,并返回增加后的值。
int ngx_shmap_inc_int(ngx_shm_zone_t* zone, ngx_str_t* key,int64_t i,uint32_t exptime, int64_t* ret);
//给key增加d,并返回增加后的值。
int ngx_shmap_inc_double(ngx_shm_zone_t* zone, ngx_str_t* key,double d,uint32_t exptime,double* ret);

void ngx_str_set_int32(ngx_str_t* key, int32_t* ikey);
void ngx_str_set_int64(ngx_str_t* key, int64_t* ikey);
void ngx_str_set_double(ngx_str_t* key, double* value);

#endif

