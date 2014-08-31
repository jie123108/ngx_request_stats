/*************************************************
 * Author: jie123108@163.com
 * Copyright: jie123108
 *************************************************/
#ifndef __NGX_HTTP_REQUEST_STATUS_PUB_H__
#define __NGX_HTTP_REQUEST_STATUS_PUB_H__
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_string.h>

typedef struct ngx_http_request_status_op_s  ngx_http_request_status_op_t;

typedef u_char *(*ngx_http_request_status_op_run_pt) (ngx_http_request_t *r, u_char *buf,
    ngx_http_request_status_op_t *op);

typedef size_t (*ngx_http_request_status_op_getlen_pt) (ngx_http_request_t *r,
    uintptr_t data);


struct ngx_http_request_status_op_s {
    size_t                      len;
    ngx_http_request_status_op_getlen_pt   getlen;
    ngx_http_request_status_op_run_pt      run;
    uintptr_t                   data;
};


typedef struct {
    ngx_str_t                   name;
    ngx_array_t                *flushes;
    ngx_array_t                *ops;        /* array of ngx_http_request_status_op_t */
} ngx_http_request_status_key_t;


typedef struct {
    ngx_array_t                *lengths;
    ngx_array_t                *values;
} ngx_http_request_status_script_t;


typedef struct {
	//统计的名称，用于显示，查询。
    ngx_str_t stat_name;
    ngx_http_request_status_key_t       *key;
	//ngx_http_request_status_value_t		*value;
} ngx_http_request_status_t;

typedef struct {
    ngx_str_t                   name;
    size_t                      len;
    ngx_http_request_status_op_run_pt      run;
	ngx_http_request_status_op_getlen_pt   getlen;
} ngx_http_request_status_var_t;


inline u_char *ngx_http_request_status_msec(ngx_http_request_t *r, u_char *buf, ngx_http_request_status_op_t *op)
{
    ngx_time_t  *tp;

    tp = ngx_timeofday();

    return ngx_sprintf(buf, "%T.%03M", tp->sec, tp->msec);
}

inline int ngx_http_request_status_uri_parse(ngx_http_request_t *r,size_t* begin, size_t* end)
{
	size_t len = 0;
	size_t url_pos = 0;
	int parse_step = 0;
	int for_break = 0;
	for(len=0;len<r->request_line.len;len++){
		switch(parse_step){
		case 0://解析method
			if(r->request_line.data[len] == ' '){
				parse_step++;
			}
		break;
		case 1://解析url
		if(r->request_line.data[len] != ' ')
		{
			if(url_pos == 0){
				url_pos = len;
				parse_step++;
			}
		}
		break;
		case 2:
		if(r->request_line.data[len] == '?' 
			|| r->request_line.data[len] == ' '
			|| r->request_line.data[len] == '\t'){
			parse_step++;
			for_break = 1;
			break;
		}
		break;
		}
		if(for_break) break;
	}

	if(url_pos == 0){
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                           "invalid uri \"%V\" not found url!", &r->request_line);
		return -1;
	}
	*begin = url_pos;
	*end = len;

	return 0;
}

inline size_t ngx_http_request_status_uri_full_getlen(ngx_http_request_t *r, uintptr_t data)
{
	if(r->request_line.len == 0){
		return 1;
	}	
	size_t begin=0;
	size_t end = 0;
	int ret = ngx_http_request_status_uri_parse(r, &begin, &end);
	if(ret == -1){
		return 1;
	}	
	return end-begin;
}

inline u_char *
ngx_http_request_status_uri_full(ngx_http_request_t *r, u_char *buf, ngx_http_request_status_op_t *op)
{
	if(r->request_line.len == 0){
		return ngx_sprintf(buf, "-");
	}
	
	size_t begin=0;
	size_t end = 0;
	int ret = ngx_http_request_status_uri_parse(r, &begin, &end);
	if(ret == -1){
		return ngx_sprintf(buf, "-");
	}	


	ngx_str_t tmp = {end-begin, r->request_line.data+begin};
    return ngx_sprintf(buf, "%V", &tmp);
}

inline u_char *
ngx_http_request_status_request_time(ngx_http_request_t *r, u_char *buf,
    ngx_http_request_status_op_t *op)
{
    ngx_time_t      *tp;
    ngx_msec_int_t   ms;

    tp = ngx_timeofday();

    ms = (ngx_msec_int_t)
             ((tp->sec - r->start_sec) * 1000 + (tp->msec - r->start_msec));
    ms = ngx_max(ms, 0);

    return ngx_sprintf(buf, "%T.%04M", ms / 1000, ms % 1000);
}


inline u_char *
ngx_http_request_status_status(ngx_http_request_t *r, u_char *buf, ngx_http_request_status_op_t *op)
{
    ngx_uint_t  status;

    if (r->err_status) {
        status = r->err_status;

    } else if (r->headers_out.status) {
        status = r->headers_out.status;

    } else if (r->http_version == NGX_HTTP_VERSION_9) {
        *buf++ = '0';
        *buf++ = '0';
        *buf++ = '9';
        return buf;

    } else {
        status = 0;
    }

    return ngx_sprintf(buf, "%ui", status);
}

inline u_char* ngx_http_request_status_date(ngx_http_request_t *r, u_char *buf, ngx_http_request_status_op_t *op)
{
	u_char* p = ngx_cpymem(buf, ngx_cached_http_log_iso8601.data, 10);
	//buf[4] = '-';
	//buf[7] = '-';
	
    return p;
}


inline u_char* ngx_http_request_status_time(ngx_http_request_t *r, u_char *buf, ngx_http_request_status_op_t *op)
{
	u_char* p = ngx_cpymem(buf, ngx_cached_err_log_time.data+11, 8);
    return p;
}


inline u_char* ngx_http_request_status_year(ngx_http_request_t *r, u_char *buf, ngx_http_request_status_op_t *op)
{
	u_char* p = ngx_cpymem(buf, ngx_cached_err_log_time.data, 4);
    return p;
}

inline u_char* ngx_http_request_status_month(ngx_http_request_t *r, u_char *buf, ngx_http_request_status_op_t *op)
{
	u_char* p = ngx_cpymem(buf, ngx_cached_err_log_time.data+5, 2);
    return p;
}

inline u_char* ngx_http_request_status_day(ngx_http_request_t *r, u_char *buf, ngx_http_request_status_op_t *op)
{
	u_char* p = ngx_cpymem(buf, ngx_cached_err_log_time.data+8, 2);
    return p;
}

inline u_char* ngx_http_request_status_hour(ngx_http_request_t *r, u_char *buf, ngx_http_request_status_op_t *op)
{
	u_char* p = ngx_cpymem(buf, ngx_cached_err_log_time.data+11, 2);
    return p;
}

inline u_char* ngx_http_request_status_minute(ngx_http_request_t *r, u_char *buf, ngx_http_request_status_op_t *op)
{
	u_char* p = ngx_cpymem(buf, ngx_cached_err_log_time.data+14, 2);
    return p;
}

inline u_char* ngx_http_request_status_second(ngx_http_request_t *r, u_char *buf, ngx_http_request_status_op_t *op)
{
	u_char* p = ngx_cpymem(buf, ngx_cached_err_log_time.data+17, 2);
    return p;
}



inline u_char *
ngx_http_request_status_bytes_sent(ngx_http_request_t *r, u_char *buf,
    ngx_http_request_status_op_t *op)
{
    return ngx_sprintf(buf, "%O", r->connection->sent);
}


/*
 * although there is a real $body_bytes_sent variable,
 * this log operation code function is more optimized for logging
 */

inline u_char *
ngx_http_request_status_body_bytes_sent(ngx_http_request_t *r, u_char *buf,
    ngx_http_request_status_op_t *op)
{
    off_t  length;

    length = r->connection->sent - r->header_size;

    if (length > 0) {
        return ngx_sprintf(buf, "%O", length);
    }

    *buf = '0';

    return buf + 1;
}


inline u_char *
ngx_http_request_status_request_length(ngx_http_request_t *r, u_char *buf,
    ngx_http_request_status_op_t *op)
{
    return ngx_sprintf(buf, "%O", r->request_length);
}


#endif
