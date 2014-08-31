/*************************************************
 * Author: jie123108@163.com
 * Copyright: jie123108
 *************************************************/

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <time.h>
#include <string.h>
#include "ngx_shmap.h"
#include "ngx_http_request_stats_module.h"

#define atom_add(p, n) __sync_add_and_fetch(p, n);

int status_codes[] = {
	NGX_HTTP_OK,
	NGX_HTTP_PARTIAL_CONTENT,
	NGX_HTTP_SPECIAL_RESPONSE,
	NGX_HTTP_MOVED_PERMANENTLY,
	NGX_HTTP_MOVED_TEMPORARILY,
	NGX_HTTP_NOT_MODIFIED,
	NGX_HTTP_TEMPORARY_REDIRECT,
	NGX_HTTP_BAD_REQUEST,
	NGX_HTTP_UNAUTHORIZED,
	NGX_HTTP_FORBIDDEN,
	NGX_HTTP_NOT_FOUND,
	NGX_HTTP_NOT_ALLOWED,
	NGX_HTTP_REQUEST_TIME_OUT,
	NGX_HTTP_CONFLICT,
	NGX_HTTP_CLIENT_CLOSED_REQUEST,
	NGX_HTTP_INTERNAL_SERVER_ERROR,
	NGX_HTTP_BAD_GATEWAY,
	NGX_HTTP_SERVICE_UNAVAILABLE,
	NGX_HTTP_GATEWAY_TIME_OUT
	};
#define HTTP_CODE_CNT ((int)(sizeof(status_codes)/sizeof(int)))
static char g_code_index[512];
static void code_index_init(){
	memset(g_code_index,-1, sizeof(g_code_index));
	int i;
	for(i=0;i< HTTP_CODE_CNT;i++){
		g_code_index[status_codes[i]] = i;
	}
}

typedef struct {
	uint32_t stats_time; //stat
	uint32_t request_count; //请求次数
	uint32_t req_count_detail[HTTP_CODE_CNT]; //每种错误码的详细记录
	uint64_t request_time;//请求时间累计(毫秒)。
	uint64_t recv;   //接收的请求总大小(byte)
	uint64_t sent;   //发送的响应总大小(byte)
}request_stats_value_t;

typedef struct {
	uint32_t user_flags;
	ngx_str_t name;
}stats_name_t;

typedef struct {
	ngx_shm_zone_t* shmap;
	size_t shmap_size; 
	time_t shmap_exptime;

	ngx_array_t* stats_names;
} ngx_http_request_stats_main_conf_t;

typedef struct {
   ngx_flag_t request_stats_query; //on | off
   ngx_array_t                *stats;       /* array of ngx_http_request_stats_t */

   ngx_uint_t                  off;        /* unsigned  off:1 */
} ngx_http_request_stats_loc_conf_t;

static ngx_int_t ngx_http_request_stats_shm_get(
		ngx_http_request_t *r,ngx_str_t* key, 
		request_stats_value_t** pvalue);

static ngx_int_t  ngx_http_request_stats_init_process(ngx_cycle_t *cycle);
static void ngx_http_request_stats_exit_process(ngx_cycle_t *cycle);

static ngx_int_t ngx_http_request_stats_variable_compile(ngx_conf_t *cf,
    ngx_http_request_stats_op_t *op, ngx_str_t *value);
static size_t ngx_http_request_stats_variable_getlen(ngx_http_request_t *r,
    uintptr_t data);
static u_char *ngx_http_request_stats_variable(ngx_http_request_t *r, u_char *buf,
    ngx_http_request_stats_op_t *op);


static void *ngx_http_request_stats_create_main_conf(ngx_conf_t *cf);
static char* ngx_http_request_stats_init_main_conf(ngx_conf_t *cf, void *conf);
static void *ngx_http_request_stats_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_request_stats_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_request_stats_set_request_stat(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_request_stats_compile_key(ngx_conf_t *cf,
    ngx_array_t *flushes, ngx_array_t *ops, ngx_array_t *args, ngx_uint_t s);
static ngx_int_t ngx_http_request_stats_init(ngx_conf_t *cf);
ngx_int_t ngx_http_request_stats_query_handler(ngx_http_request_t *r);
static char* ngx_http_request_stats_query_post_handler (
			ngx_conf_t *cf, void *data, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_request_stats_query_handler;

    return NGX_CONF_OK;}

static ngx_conf_post_t query_post = {&ngx_http_request_stats_query_post_handler};

static ngx_command_t  ngx_http_request_stats_commands[] = {
	{ngx_string("request_stats_query"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_request_stats_loc_conf_t, request_stats_query),
      &query_post },

    { ngx_string("shmap_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_request_stats_main_conf_t, shmap_size),
      NULL },
    { ngx_string("shmap_exptime"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_sec_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_request_stats_main_conf_t, shmap_exptime),
      NULL },

    { ngx_string("request_stats"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
                        |NGX_HTTP_LMT_CONF|NGX_CONF_TAKE12,
      ngx_http_request_stats_set_request_stat,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_request_stats_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_request_stats_init,                     /* postconfiguration */

    ngx_http_request_stats_create_main_conf,         /* create main configuration */
    ngx_http_request_stats_init_main_conf,         /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_request_stats_create_loc_conf,          /* create location configuration */
    ngx_http_request_stats_merge_loc_conf            /* merge location configuration */
};


ngx_module_t  ngx_http_request_stats_module = {
    NGX_MODULE_V1,
    &ngx_http_request_stats_module_ctx,              /* module context */
    ngx_http_request_stats_commands,                 /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    &ngx_http_request_stats_init_process,       /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    &ngx_http_request_stats_exit_process,       /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

ngx_http_request_stats_var_t  ngx_http_request_stats_vars[] = {
    { ngx_string("uri_full"), 0, ngx_http_request_stats_uri_full,ngx_http_request_stats_uri_full_getlen },
    //{ ngx_string("json_stat"), 0, ngx_http_request_stats_json_stat,ngx_http_request_stats_json_stats_getlen},
    //{ ngx_string("json_reason"), 0, ngx_http_request_stats_json_reason,ngx_http_request_stats_json_reason_getlen},

	{ ngx_string("msec"), NGX_TIME_T_LEN + 4, ngx_http_request_stats_msec,NULL },
    { ngx_string("request_time"), NGX_TIME_T_LEN + 4,ngx_http_request_stats_request_time,NULL },
    { ngx_string("status"), 3, ngx_http_request_stats_status,NULL },
    { ngx_string("date"), 10, ngx_http_request_stats_date,NULL }, //yyyy-MM-dd
    { ngx_string("time"), 8, ngx_http_request_stats_time,NULL }, //hh:mm:ss
    { ngx_string("year"), 4, ngx_http_request_stats_year,NULL }, //yyyy
    { ngx_string("month"), 2, ngx_http_request_stats_month,NULL }, //MM
    { ngx_string("day"), 2, ngx_http_request_stats_day,NULL }, //dd
    { ngx_string("hour"), 2, ngx_http_request_stats_hour,NULL }, //hh
    { ngx_string("minute"), 2, ngx_http_request_stats_minute,NULL }, //mm
    { ngx_string("second"), 2, ngx_http_request_stats_second,NULL }, //ss
    { ngx_string("bytes_sent"), NGX_OFF_T_LEN, ngx_http_request_stats_bytes_sent,NULL },
    { ngx_string("body_bytes_sent"), NGX_OFF_T_LEN,
                          ngx_http_request_stats_body_bytes_sent,NULL },
    { ngx_string("request_length"), NGX_SIZE_T_LEN,
                          ngx_http_request_stats_request_length,NULL },

    { ngx_null_string, 0, NULL,NULL }
};


static ngx_int_t  ngx_http_request_stats_init_process(ngx_cycle_t *cycle)
{
    ngx_http_request_stats_main_conf_t   *lmcf;

	lmcf = (ngx_http_request_stats_main_conf_t*)ngx_get_conf(cycle->conf_ctx, ngx_http_request_stats_module);
	if(lmcf == NULL){
		return 0;
	}

	
	return NGX_OK;
}


static void ngx_http_request_stats_exit_process(ngx_cycle_t *cycle)
{
    ngx_http_request_stats_main_conf_t   *lmcf;

	lmcf = (ngx_http_request_stats_main_conf_t*)ngx_get_conf(cycle->conf_ctx, ngx_http_request_stats_module);
	if(lmcf == NULL){
		return;
	}
	
}

static ngx_int_t ngx_http_request_stats_shm_get(
		ngx_http_request_t *r,ngx_str_t* key, 
		request_stats_value_t** pvalue)
{
	ngx_int_t rc = NGX_OK;
	ngx_http_request_stats_main_conf_t* lmcf;
    lmcf = ngx_http_get_module_main_conf(r, ngx_http_request_stats_module);
	ngx_str_t data = ngx_null_string;
	uint8_t value_type = VT_NULL;

	ngx_memzero(&data,sizeof(ngx_str_t));
	value_type = 0;
	
	rc = ngx_shmap_get(lmcf->shmap, key, &data, &value_type, NULL,NULL);
	if(rc == 0){
		if(value_type != VT_BINARY){
			ngx_log_error(NGX_LOG_ERR, r->connection->log,0, 
					"ngx_shmap_get failed! key [%V] value_type [%d] invalid!",
					key, value_type);
			ngx_shmap_delete(lmcf->shmap, key);
			return -1;
		}

		//大小不对。
		if(data.len != sizeof(request_stats_value_t)){
			ngx_shmap_delete(lmcf->shmap, key);
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
					"ngx_shmap_get failed!  key [%V] value_len [%d] invalid! valid size:%d",
					key, (int)data.len, (int)sizeof(request_stats_value_t));
			return -1;
		}

		*pvalue = (request_stats_value_t*)data.data;
		rc = NGX_OK;
	}

	return rc;
}

static ngx_int_t ngx_http_request_stats_shm_get_ex(
		ngx_http_request_t *r,
		ngx_str_t* key, uint32_t user_flag, 
		request_stats_value_t** pvalue)
{
	ngx_int_t rc = NGX_OK;
	ngx_http_request_stats_main_conf_t* lmcf;
    lmcf = ngx_http_get_module_main_conf(r, ngx_http_request_stats_module);
	ngx_str_t data = ngx_null_string;
	uint8_t value_type = VT_NULL;
	int add_times = 0;
SHM_GET:
	ngx_memzero(&data,sizeof(ngx_str_t));
	rc = ngx_http_request_stats_shm_get(r, key, pvalue);
	if(rc != NGX_OK)
	{
		if(add_times++ < 1){//只插入一次。
			//不存在，插入新的
			request_stats_value_t tmp_value;
			memset(&tmp_value,0,sizeof(tmp_value));
			tmp_value.stats_time = (uint32_t)ngx_time();
			data.len = sizeof(request_stats_value_t);
			data.data = (u_char*)&tmp_value;
			value_type = VT_BINARY;

			rc =ngx_shmap_set(lmcf->shmap, key, &data, value_type, lmcf->shmap_exptime,user_flag);
			if(rc != NGX_OK){
				ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
					"ngx_shmap_set(key=%V) failed! rc=%d",
					key, rc);
			}
			goto SHM_GET;
		}else{
			rc = NGX_ERROR;
		}
	}
	return rc;
}

char *strcut(char *strin, unsigned int frompos,unsigned int count) {
    int len = strlen(strin);
    if(count > 0){
        if(count > len-frompos) count = len-frompos;
        memmove(strin+frompos, strin+frompos+count, len-frompos-count+1);
    }
    return strin;
}

char *time2str(const time_t *t, const char *mask, char *sout) {
    struct tm *s;
    char ch, b[10], b1[20];
    unsigned int len, i, j;

    sout[0] = 0;
    s = localtime(t);

    for(i = 0; i < strlen(mask); i++) {
        len = 0;

        if(strchr("DMYhms", ch = mask[i])) {
            j = i; len = 1;
            while(mask[++j] == ch) len++;
            sprintf(b, "%%0%dd", len);
            i += len-1;

            switch(ch) {
                case 'D': sprintf(b1, b, s->tm_mday); break;
                case 'M': sprintf(b1, b, s->tm_mon+1); break;
                case 'Y':
                    j = s->tm_year + 1900;
                    sprintf(b1, b, j);
                    if(len <= 3) strcut(b1, 0, 2);
                    break;
                case 'h': sprintf(b1, b, s->tm_hour); break;
                case 'm': sprintf(b1, b, s->tm_min); break;
                case 's': sprintf(b1, b, s->tm_sec); break;
            }
            strcat(sout, b1);
        } else {
            len = strlen(sout);
            sout[len+1] = 0;
            sout[len] = mask[i];
        }
    }
    return sout;
}

/**
 * 当buf 剩余空间小于1K时，重新分配空间，使其大小为原来一倍
 */
ngx_int_t request_stats_check_buf_size(ngx_http_request_t* r, ngx_buf_t* buf)
{
	size_t old_size = buf->end - buf->start;
	size_t rest_size = buf->end - buf->last;
	
	if(rest_size < 1024){
		u_char* b = (u_char*)ngx_pcalloc(r->connection->pool, old_size*2);
		if(b == NULL){
			return NGX_ERROR;
		}

		size_t old_buf_size = ngx_buf_size(buf);
        ngx_memcpy(b, buf->pos, old_buf_size);
        buf->start = b;
        buf->end = buf->start+old_size*2;
        buf->pos = b;
        buf->last = b+old_buf_size;
	}

	return NGX_OK;
}


typedef struct {
	const char* all_begin;
	const char* comment_begin;
	const char* comment_clean;
	const char* comment_fmt;
	const char* comment_stats_name;
	const char* comment_end;

	const char* mark_begin; //所有记录开始
	const char* title_line;
	const char* line_begin;
	const char* line;
	const char* http_code_begin;
	const char* http_code;
	const char* http_code_end;
	const char* line_end;
	const char* mark_end;

	const char* all_end;
}resp_format_t;

#define COMMENT_CLEAN "clean=true, query stats and set the all query data to zero in the share memory."
#define COMMENT_FMT "fmt=[html|json|text], The default is text."
#define COMMENT_STAT_NAME "stats_name=[%s], The default is all."

static resp_format_t fmt_plaintext = {
	.comment_begin="# Optional parameters:\n",
	.comment_clean="# "COMMENT_CLEAN"\n",
	.comment_fmt="# "COMMENT_FMT"\n",
	.comment_stats_name="# "COMMENT_STAT_NAME"\n",
	
	.mark_begin=NULL,
	.title_line="key\tstats_time\trequest\trecv\tsent\tavg_time\tstat\n",
	.line_begin=NULL,						//
	.line="%V\t%s\t%uD\t%uL\t%uL\t%uL\t",
	.http_code_begin=NULL,.http_code=" %03d:%d,",.http_code_end=NULL,
	.line_end="\n",
	.mark_end=NULL
};

static resp_format_t fmt_html = {
	.all_begin="<body>\n",
	.all_end="</body>\n",
	.comment_begin="<table width=\"1000\" align='center'>\n"
					"<tr><td>Optional parameters:</td></tr>\n"
					"<tr>\n<td>\n<ul>\n",
	.comment_clean="<li>"COMMENT_CLEAN"</li>\n",
	.comment_fmt="<li>"COMMENT_FMT"</li>\n",
	.comment_stats_name="<li>"COMMENT_STAT_NAME"</li>\n",
	.comment_end="</ul>\n</td>\n</tr>\n</table>\n",
	
	.mark_begin="<table width=\"1000\" bgcolor=\"#FFFFFF\" align=\"center\">\n",
	.title_line="<tr bgcolor=\"#666666\" height=\"25\"><td>key</td><td>stats_time</td><td>request</td><td>recv</td><td>sent</td><td>avg_time</td><td>stat</td></tr>\n",
	.line_begin="<tr height=\"20\" bgcolor=\"#CCCCCC\">",	
	.line="<td>%V</td><td>%s</td><td>%uD</td><td>%uL</td><td>%uL</td><td>%uL</td><td>",
	.http_code_begin=NULL,
	.http_code=" %03d:%d|",
	.http_code_end=NULL,
	.line_end="</td></tr>\n",
	.mark_end="</table>"
};

static resp_format_t fmt_json = {
	.all_begin="{",
	.all_end="}\n",
	.comment_begin="\"Optional parameters\":{\n",
	.comment_end="},\n",
	
	.comment_clean="\"clean\":\""COMMENT_CLEAN"\",\n",
	.comment_fmt="\"fmt\":\""COMMENT_FMT"\",\n",
	.comment_stats_name="\"stats_name\":[\"%s\"]\n",

	.mark_begin="\"request-stat\":{\n",
	.title_line=NULL,
	.line_begin=NULL,						//
	.line="\"%V\":{\"stats_time\":\"%s\",\"request\":%uD,\"recv\":%uL,\"sent\":%uL,\"avg_time\":%uL,",
	.http_code_begin="\"stat\":{",
	.http_code="\"%03d\":%d,",
	.http_code_end="}",
	.line_end="},\n",
	.mark_end="}\n"
};


#define ngx_sappend(buf, format, args...); \
	if(format!=NULL){\
		buf = ngx_sprintf(buf,format, ##args);\
	}

typedef struct {
	ngx_int_t clean; //取得统计信息后，将原来的信息清0.
	resp_format_t* fmt; //输出格式串
	ngx_buf_t* buf;
	ngx_str_t* stats_name;//当有该参数时，只取该统计项的值。
	ngx_uint_t stats_name_user_flags;
	ngx_http_request_t* r;
}foreach_args_t;

void request_stats_foreach(ngx_shmap_node_t* node, void* extarg)
{
	foreach_args_t* args = (foreach_args_t*)extarg;
	ngx_buf_t* buf = args->buf;
	ngx_http_request_t* r = args->r;
	resp_format_t* fmt = args->fmt;
	char buf_time[32];
	ngx_memzero(buf_time,sizeof(buf_time));
	
	if(args->stats_name_user_flags > 0 && node->user_flags != args->stats_name_user_flags){
		return;
	}

	if(request_stats_check_buf_size(r, buf)!=NGX_OK){
		ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
			"request_stats_check_buf_size() failed!");
		return;
	}
	
	ngx_str_t key = {(size_t)node->key_len, node->data};
	request_stats_value_t* value = (request_stats_value_t*)(node->data+node->key_len);
	if(value->request_count < 1){
		return;
	}
	ngx_sappend(buf->last, fmt->line_begin);

	uint32_t request_count = value->request_count<1?1:value->request_count;
	time_t stats_time = (time_t)value->stats_time;
	ngx_sappend(buf->last, fmt->line,
					&key, time2str(&stats_time,"YYYY-MM-DD hh:mm:ss", buf_time),
					value->request_count, 
					value->recv, value->sent,
					value->request_time/request_count
					);
	unsigned i;
	int del_comma = 0;
	ngx_sappend(buf->last, fmt->http_code_begin);
	for(i=0;i<HTTP_CODE_CNT;i++){
		if(value->req_count_detail[i] > 0){
			int http_code = status_codes[i];
			del_comma = 1;
			buf->last = ngx_sprintf(buf->last,fmt->http_code,http_code,value->req_count_detail[i]);
		}
	}
	if(del_comma){ buf->last-=1;}
	
	ngx_sappend(buf->last, fmt->http_code_end);
	ngx_sappend(buf->last, fmt->line_end);

	if(args->clean>0){
		ngx_memzero(value, sizeof(request_stats_value_t));
	}
}

ngx_int_t ngx_http_request_stats_query_handler(ngx_http_request_t *r)
{
	ngx_int_t rc = NGX_HTTP_OK;
	unsigned i;
	if (!(r->method & NGX_HTTP_GET)) {
		return NGX_HTTP_NOT_ALLOWED;
	}
	ngx_http_request_stats_main_conf_t   *lmcf;
    lmcf = ngx_http_get_module_main_conf(r, ngx_http_request_stats_module);
    foreach_args_t foreach_args;
    ngx_memzero(&foreach_args,sizeof(foreach_args));
	resp_format_t* fmt = &fmt_plaintext;
	ngx_str_t content_type = ngx_string("text/plain");
	/******************* request para parse ****************/
    ngx_str_t stats_name = ngx_null_string;
    if(ngx_http_arg(r, (u_char*)"stats_name", sizeof("stats_name")-1, &stats_name) == NGX_OK){
		if(stats_name.len > 0){
			foreach_args.stats_name = &stats_name;
			foreach_args.stats_name_user_flags = ngx_crc32_long(stats_name.data,stats_name.len);
		}
    }
    ngx_str_t szclean = ngx_null_string;
    if(ngx_http_arg(r, (u_char*)"clean", sizeof("clean")-1, &szclean) == NGX_OK){
		if(szclean.len > 0 && ngx_strncmp(szclean.data, "true",4)==0){
			foreach_args.clean = 1;
		}
    }
    ngx_str_t szfmt = ngx_null_string;
    if(ngx_http_arg(r, (u_char*)"fmt", sizeof("fmt")-1, &szfmt) == NGX_OK){
		if(ngx_strncmp(szfmt.data, "html",4)==0){
			fmt = &fmt_html;
			static ngx_str_t content_type_html = ngx_string("text/html");
			content_type = content_type_html;
		}else if(ngx_strncmp(szfmt.data, "json",4)==0){
			fmt = &fmt_json;
			static ngx_str_t content_type_json = ngx_string("application/json");
			content_type = content_type_json;
		}
    }
    

	ngx_buf_t* buf = ngx_create_temp_buf(r->connection->pool, 4096);
	if (buf == NULL) { 
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ngx_chain_t* out = ngx_alloc_chain_link(r->pool);
    if(out == NULL){
		return NGX_HTTP_INITING_REQUEST_STATE;
    }

	ngx_sappend(buf->last, fmt->all_begin);
	if(stats_name.len < 1){
	    stats_name_t* stats_names = lmcf->stats_names->elts;
	    u_char stats_name_buf[2048];
	    ngx_memzero(stats_name_buf,sizeof(stats_name_buf));
	    u_char* p = stats_name_buf;
		for(i=0;i<lmcf->stats_names->nelts;i++){
			if(i==lmcf->stats_names->nelts-1){
				ngx_sappend(p," %V", &stats_names[i].name);
			}else{
				ngx_sappend(p," %V|", &stats_names[i].name);
			}
		}

		ngx_sappend(buf->last, fmt->comment_begin);
	    ngx_sappend(buf->last, fmt->comment_clean);
		ngx_sappend(buf->last, fmt->comment_fmt);
	    ngx_sappend(buf->last, fmt->comment_stats_name, stats_name_buf);
		ngx_sappend(buf->last, fmt->comment_end);
	}

	ngx_sappend(buf->last, fmt->mark_begin);
    ngx_sappend(buf->last, fmt->title_line);

	foreach_args.fmt = fmt;
    foreach_args.buf = buf;
    foreach_args.r = r;
    //foreach_args.buf[0] = buf;
    //foreach_args.index = 0;
    
	ngx_shmap_foreach(lmcf->shmap, &request_stats_foreach, &foreach_args);
	ngx_sappend(buf->last, fmt->mark_end);
	ngx_sappend(buf->last, fmt->all_end);

	out->buf = buf;
	out->next = NULL;
	r->headers_out.content_type = content_type;
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = ngx_buf_size(buf);

    buf->last_buf = (r == r->main) ? 1 : 0;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, out);
}

ngx_int_t
ngx_http_request_stats_log_handler(ngx_http_request_t *r)
{
    u_char                   *buf_key, *p_key;
    size_t                    len_key;
    ngx_uint_t                i, l;
    ngx_http_request_stats_t           *stat;
    //ngx_open_file_t          *file;
    ngx_http_request_stats_op_t        *op_key;
    ngx_http_request_stats_loc_conf_t  *lcf;
	ngx_http_request_stats_main_conf_t   *lmcf;
	
    lmcf = ngx_http_get_module_main_conf(r, ngx_http_request_stats_module);

    //ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->stat, 0,
    //               "http req stat handler");

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_request_stats_module);
	if (lcf->off || lcf->stats == NULL) {
        return NGX_OK;
    }
	
    stat = lcf->stats->elts;
    for (l = 0; l < lcf->stats->nelts; l++) {
		//############# key ###################
        ngx_http_script_flush_no_cacheable_variables(r, stat[l].key->flushes);

        len_key = 0;
		
        op_key = stat[l].key->ops->elts;
		
        for (i = 0; i < stat[l].key->ops->nelts; i++) {
            if (op_key[i].len == 0) {
                len_key += op_key[i].getlen(r, op_key[i].data);
            } else {
                len_key += op_key[i].len;
            }
        }

        buf_key = ngx_pcalloc(r->pool, len_key+1);
        if (buf_key == NULL) {
            return NGX_ERROR;
        }

        p_key = buf_key;

        for (i = 0; i < stat[l].key->ops->nelts; i++) {
            p_key = op_key[i].run(r, p_key, &op_key[i]);
        }

		ngx_str_t key = {p_key-buf_key, buf_key};

		uint32_t user_flag = ngx_crc32_long(stat->stats_name.data, stat->stats_name.len);
		request_stats_value_t* value=NULL;
		ngx_int_t rc = 0;
		rc = ngx_http_request_stats_shm_get_ex(r, &key, user_flag, &value);
		if(rc == NGX_OK && value != NULL){
			atom_add(&value->request_count,1);
			atom_add(&value->recv, (uint64_t)r->request_length);
			atom_add(&value->sent, (uint64_t)r->connection->sent);
			ngx_uint_t  status;
		    if (r->err_status) {
		        status = r->err_status;
		    } else if (r->headers_out.status) {
		        status = r->headers_out.status;
		    } else if (r->http_version == NGX_HTTP_VERSION_9) {
		        status = 9;
		    } else {
		        status = 0;
		    }
			int sidx = -1;
			if(status < sizeof(g_code_index)){
				sidx = g_code_index[status];
				if(sidx >=0 && sidx < HTTP_CODE_CNT){
					atom_add(&value->req_count_detail[sidx],1);
				}
			}

			/************** request_time ****************/
		    ngx_time_t      *tp;
		    ngx_int_t   ms;

		    tp = ngx_timeofday();
		    ms = (ngx_int_t)((tp->sec - r->start_sec) * 1000 
		    					+ (tp->msec - r->start_msec));
		    ms = ngx_max(ms, 0);

			atom_add(&value->request_time, (uint64_t)ms);
		}
	}
    return NGX_OK;
}


static u_char *
ngx_http_request_stats_copy_short(ngx_http_request_t *r, u_char *buf,
    ngx_http_request_stats_op_t *op)
{
    size_t     len;
    uintptr_t  data;

    len = op->len;
    data = op->data;

    while (len--) {
        *buf++ = (u_char) (data & 0xff);
        data >>= 8;
    }

    return buf;
}


static u_char *
ngx_http_request_stats_copy_long(ngx_http_request_t *r, u_char *buf,
    ngx_http_request_stats_op_t *op)
{
    return ngx_cpymem(buf, (u_char *) op->data, op->len);
}



static ngx_int_t
ngx_http_request_stats_variable_compile(ngx_conf_t *cf, ngx_http_request_stats_op_t *op,
    ngx_str_t *value)
{
    ngx_int_t  index;

    index = ngx_http_get_variable_index(cf, value);
    if (index == NGX_ERROR) {
        return NGX_ERROR;
    }

    op->len = 0;
    op->getlen = ngx_http_request_stats_variable_getlen;
    op->run = ngx_http_request_stats_variable;
    op->data = index;

    return NGX_OK;
}


static size_t
ngx_http_request_stats_variable_getlen(ngx_http_request_t *r, uintptr_t data)
{
    uintptr_t                   len;
    ngx_http_variable_value_t  *value;

    value = ngx_http_get_indexed_variable(r, data);

    if (value == NULL || value->not_found) {
        return 1;
    }

	//UTF8等编码不进行转义 (转的话，会转成\\xE5\\xAE之类的，然后导致json_c解析失败。)
    len =  0; 
    value->escape = len ? 1 : 0;

    return value->len + len * 3;
}


static u_char *
ngx_http_request_stats_variable(ngx_http_request_t *r, u_char *buf, ngx_http_request_stats_op_t *op)
{
    ngx_http_variable_value_t  *value;

    value = ngx_http_get_indexed_variable(r, op->data);

    if (value == NULL || value->not_found) {
        *buf = '-';
        return buf + 1;
    }
	
    return ngx_cpymem(buf, value->data, value->len);
}


static void *
ngx_http_request_stats_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_request_stats_main_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_request_stats_main_conf_t));
    if (conf == NULL) {
        return NULL;
    }

	conf->shmap_size= NGX_CONF_UNSET_SIZE;
	conf->shmap_exptime = NGX_CONF_UNSET;
    return conf;
}

static char*   ngx_http_request_stats_init_main_conf(ngx_conf_t *cf, void *conf)
{
	ngx_http_request_stats_main_conf_t  *rscf = conf;

	if(rscf->shmap_size == NGX_CONF_UNSET_SIZE)rscf->shmap_size = 1024*1024*32;
	if(rscf->shmap_exptime == NGX_CONF_UNSET)rscf->shmap_exptime = 3600*24*2;
	
	return NGX_CONF_OK;
}

static void *
ngx_http_request_stats_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_request_stats_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_request_stats_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }
	conf->off = 0;
	conf->request_stats_query = NGX_CONF_UNSET;
	
    return conf;
}


static char *
ngx_http_request_stats_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_request_stats_loc_conf_t *prev = parent;
    ngx_http_request_stats_loc_conf_t *conf = child;

	ngx_conf_merge_value(conf->request_stats_query, prev->request_stats_query, 0);
    if (conf->stats || conf->off) {
       return NGX_CONF_OK;
    }

    conf->stats = prev->stats;
    conf->off = prev->off;
 
    if (conf->stats || conf->off) {
        return NGX_CONF_OK;
    }


    return NGX_CONF_OK;
}


static char *
ngx_http_request_stats_set_request_stat(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_request_stats_loc_conf_t *llcf = conf;

    ngx_str_t                  *value;
    ngx_http_request_stats_t             *stat;
    //ngx_http_request_stats_key_t         *key;
    ngx_http_request_stats_main_conf_t   *lmcf;
    value = cf->args->elts;

	if (ngx_strcmp(value[1].data, "off") == 0) {
        llcf->off = 1;
        if (cf->args->nelts == 2) {
            return NGX_CONF_OK;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }else if(cf->args->nelts != 3) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid config \"%V\", missing param!", &value[0]);
        return NGX_CONF_ERROR;
    }

    if (llcf->stats == NULL) {
        llcf->stats = ngx_array_create(cf->pool, 2, sizeof(ngx_http_request_stats_t));
        if (llcf->stats == NULL) {
            return NGX_CONF_ERROR;
        }
    }

	lmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_request_stats_module);

	stat = ngx_array_push(llcf->stats);
	if (stat == NULL) {
		return NGX_CONF_ERROR;
	}

	ngx_memzero(stat, sizeof(ngx_http_request_stats_t));

	stat->stats_name = value[1];

	if(lmcf->stats_names == NULL){
		lmcf->stats_names = ngx_array_create(cf->pool, 8, sizeof(stats_name_t));
		if (lmcf->stats_names == NULL) {
            return NGX_CONF_ERROR;
        }
	}
	unsigned i;
	int exist = 0;
	uint32_t user_flags = ngx_crc32_long(stat->stats_name.data,stat->stats_name.len);
	stats_name_t* stats_names = lmcf->stats_names->elts;
	for(i=0;i<lmcf->stats_names->nelts;i++){
		if(stats_names[i].user_flags == user_flags){
			exist = 1;
			break;
		}
	}
	if(!exist){
		stats_name_t* new_stats_name = ngx_array_push(lmcf->stats_names);
		if(new_stats_name == NULL){
        	return NGX_CONF_ERROR;
		}
		new_stats_name->user_flags = user_flags;
		new_stats_name->name = stat->stats_name;
	}
	
	//compile key..	
	stat->key = ngx_pcalloc(cf->pool, sizeof(ngx_http_request_stats_key_t));
    stat->key->flushes = ngx_array_create(cf->pool, 4, sizeof(ngx_int_t));
    if (stat->key->flushes == NULL) {
        return NGX_CONF_ERROR;
    }

    stat->key->ops = ngx_array_create(cf->pool, 4, sizeof(ngx_http_request_stats_op_t));
    if (stat->key->ops == NULL) {
        return NGX_CONF_ERROR;
    }

    return ngx_http_request_stats_compile_key(cf, stat->key->flushes, stat->key->ops, cf->args, 2);
}


static char *
ngx_http_request_stats_compile_key(ngx_conf_t *cf, ngx_array_t *flushes,
    ngx_array_t *ops, ngx_array_t *args, ngx_uint_t s)
{
    u_char              *data, *p, ch;
    size_t               i, len;
    ngx_str_t           *value, var;
    ngx_int_t           *flush;
    ngx_uint_t           bracket;
    ngx_http_request_stats_op_t   *op;
    ngx_http_request_stats_var_t  *v;

    value = args->elts;

    for ( /* void */ ; s < args->nelts; s++) {

        i = 0;

        while (i < value[s].len) {

            op = ngx_array_push(ops);
            if (op == NULL) {
                return NGX_CONF_ERROR;
            }

            data = &value[s].data[i];

            if (value[s].data[i] == '$') {

                if (++i == value[s].len) {
                    goto invalid;
                }

                if (value[s].data[i] == '{') {
                    bracket = 1;

                    if (++i == value[s].len) {
                        goto invalid;
                    }

                    var.data = &value[s].data[i];

                } else {
                    bracket = 0;
                    var.data = &value[s].data[i];
                }

                for (var.len = 0; i < value[s].len; i++, var.len++) {
                    ch = value[s].data[i];

                    if (ch == '}' && bracket) {
                        i++;
                        bracket = 0;
                        break;
                    }

                    if ((ch >= 'A' && ch <= 'Z')
                        || (ch >= 'a' && ch <= 'z')
                        || (ch >= '0' && ch <= '9')
                        || ch == '_')
                    {
                        continue;
                    }

                    break;
                }

                if (bracket) {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                       "the closing bracket in \"%V\" "
                                       "variable is missing", &var);
                    return NGX_CONF_ERROR;
                }

                if (var.len == 0) {
                    goto invalid;
                }

                for (v = ngx_http_request_stats_vars; v->name.len; v++) {

                    if (v->name.len == var.len
                        && ngx_strncmp(v->name.data, var.data, var.len) == 0)
                    {
                        op->len = v->len;
                        op->getlen = v->getlen;
                        op->run = v->run;
                        op->data = 0;

                        goto found;
                    }
                }

                if (ngx_http_request_stats_variable_compile(cf, op, &var) != NGX_OK) {
                    return NGX_CONF_ERROR;
                }

                if (flushes) {

                    flush = ngx_array_push(flushes);
                    if (flush == NULL) {
                        return NGX_CONF_ERROR;
                    }

                    *flush = op->data; /* variable index */
                }

            found:

                continue;
            }

            i++;

            while (i < value[s].len && value[s].data[i] != '$') {
                i++;
            }

            len = &value[s].data[i] - data;

            if (len) {

                op->len = len;
                op->getlen = NULL;

                if (len <= sizeof(uintptr_t)) {
                    op->run = ngx_http_request_stats_copy_short;
                    op->data = 0;

                    while (len--) {
                        op->data <<= 8;
                        op->data |= data[len];
                    }

                } else {
                    op->run = ngx_http_request_stats_copy_long;

                    p = ngx_pnalloc(cf->pool, len);
                    if (p == NULL) {
                        return NGX_CONF_ERROR;
                    }

                    ngx_memcpy(p, data, len);
                    op->data = (uintptr_t) p;
                }
            }
        }
    }

    return NGX_CONF_OK;

invalid:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid parameter \"%s\"", data);

    return NGX_CONF_ERROR;
}


static ngx_int_t
ngx_http_request_stats_init(ngx_conf_t *cf)
{
	ngx_http_handler_pt        *h;
	ngx_http_request_stats_main_conf_t   *lmcf;
	ngx_http_core_main_conf_t  *cmcf;

	lmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_request_stats_module);
	cf->cycle->conf_ctx[ngx_http_request_stats_module.index] = (void***)lmcf;

	static ngx_str_t shm_name_req_stat = ngx_string("ngx_request_stats_shm");
	ngx_shm_zone_t* zone = ngx_shmap_init(cf, 
				&shm_name_req_stat, lmcf->shmap_size,
				&ngx_http_request_stats_module);
	if(zone == NULL){
		u_char buf[256];
		ngx_sprintf(buf, "ngx_shmap_init(%V,%z) failed!\n",
			&shm_name_req_stat, lmcf->shmap_size);
		printf((char*)buf);
		return NGX_ERROR;
	}
	lmcf->shmap = zone;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_request_stats_log_handler;
	
	//初始化g_code_index
	code_index_init();
    return NGX_OK;
}
