# Name 

  ngx_request_status是一个nginx状态统计模块，其统计项是可配置的，并且可以统计不同的虚拟主机，不同的URL。可以统计的包括请求次数，各个状态码的次数，输出的流量累计信息，平均处理时间等等。

# Table of Contents

* [Synopsis](#synopsis)
* [Nginx Compatibility] (#nginx-compatibility)
* [Module Compile] (#module-compile)
* [Module Variables](#module-variables)
* [Directives](#directives)
    * [shmap_size](#shmap_size)
    * [shmap_exptime](#shmap_exptime)
    * [request_status](#request_status)
    * [request_status_query](#request_status_query)
* [Statistical query](#statistical-query)
	* [文本格式](#文本格式)
	* [html格式](#html格式)
 	* [json格式](#json格式)
	* [查询并且将查询项清零](#查询并且将查询项清零)
	* [查询某一个统计项](#查询某一个统计项)
* [作用域说明](#作用域说明)
* [简单脚本测试](#简单脚本测试)
* [相关模块](#相关模块)

# Synopsis
```nginx
http {
	request_status statby_host "$host";	
	shmap_size 32m;
	shmap_exptime 2d;

	server {
		listen 81;
		server_name localhost;
		location / {
			echo "byhost:$uri";
		}
		location /404 {
			return 404;
		}
	}
	
	server {
		listen       80;
		server_name  localhost;

		location /status {
			request_status off; #do not status.
			request_status_query on;
			allow 127.0.0.1;
			allow 192.168.0.0/16;
			deny all;
		}

		location /byuri {
			request_status statby_uri "uri:$uri";
			echo "byuri: $uri";
		}

		location /byarg {
			echo_sleep 0.005;
			request_status statby_arg "clitype:$arg_client_type";		
			echo "login $args";
		}
		
		location /byarg/404 {
			request_status statby_arg "clitype:$arg_client_type";		
			return 404;
		}

		location /byuriarg {
			request_status statby_uriarg "$uri?$arg_from";	
			echo "$uri?$args";
		}

		location /byhttpheaderin {
			request_status statby_headerin "header_in:$http_city";
			echo "city: $http_city";
		}
		
		location /byhttpheaderout/ {
			request_status statby_headerout "cache:$sent_http_cache";
			proxy_pass http://127.0.0.1:82;
		}
	}

  server {
	listen       82;
	server_name  localhost;
	location /byhttpheaderout/hit {
		add_header cache hit;
		echo "cache: hit";
	}
	location /byhttpheaderout/miss {
		add_header cache miss;
		echo "cache: miss";
	}
  }
}

```

# Nginx Compatibility
The latest module is compatible with the following versions of Nginx:
* 1.7.x (last tested: 1.7.4)
* 1.6.x (last tested: 1.6.1)
* 1.4.x (last tested: 1.4.7)
* 1.2.x (last tested: 1.2.9)
* 1.0.x (last tested: 1.0.15)


# Module Compile
```
# echo-nginx-module只是测试时需要使用,本模块并不依赖它。
cd nginx-1.x.x
./configure --add-module=path/to/ngx_request_status \
--add-module=path/to/echo-nginx-module-0.49/
make
make install
```

# Module Variables
* nginx_core模块支持的变量：http://nginx.org/en/docs/http/ngx_http_core_module.html#variables
* 本模块变量
    * uri_full 重定向之前的uri。
    * status Http响应码
    * date 当前日期，格式为：1970-09-28
    * time 当前时间，格式为：12:00:00
    * year 当前年份
    * month 当前月份
    * day 当前日
    * hour 当前小时
    * minute 当前分
    * second 当前秒

# Directives
* [shmap_size](#shmap_size)
* [shmap_exptime](#shmap_exptime)
* [request_status](#request_status)
* [request_status_query](#request_status_query)

shmap_size
----------
**syntax:** *shmap_size &lt;size&gt;*

**default:** *32m*

**context:** *http*

定义统计使用的共享内存大小。可以使用k,m,g等单位表示KB,MB,GB。


shmap_exptime
----------
**syntax:** *shmap_exptime &lt;expire time&gt;*

**default:** *2d*

**context:** *http*

定义统计信息在共享内存中的过期时间。单位为秒，可以使用m,h,d等表示分钟，小时，天。

request_status
----------
**syntax:** *request_status &lt;status-name&gt; &lt;status-key&gt;*

**default:** *no*

**context:** *http,server,location,location if*

定义统计信息格式，使用 `request_status off;` 可关闭某个http,server,location下的统计。
* status-name是该统计名称(类别)，可按功能随意定义，在后面的查询指令中，可指定status-name查询指定的统计类型。
* status-key定义统计的key。key中可使用各种变量，及字符串，这样不同的请求便分别记录。[支持的变量](#支持的变量)一节中列出了所有支持的变量。**注意：不要使用过于随机化的变量当成key,这样会导致每个请求有一份统计信息，因而占用大量共享内存空间**

#### 按host进行统计
```nginx
request_status statby_host "$host";	
```
#### 按uri进行统计
```nginx
request_status statby_uri "uri:$uri"; #还添加了 uri:前缀。
```
### 按请求参数(GET)进行统计
```nginx
request_status statby_arg "clitype:$arg_client_type"; #按参数client_type统计
```

### 按uri和参数进行统计
```nginx
request_status statby_uriarg "$uri?$arg_from";	
```

### 按HTTP请求头字段进行统计
```nginx
request_status statby_uriarg "header_in:$http_city";
```
### 按HTTP响应头字段进行统计
```nginx
# *注意，当前location下通过add_header添加的响应头读取不到。
request_status statby_uriarg "cache:$sent_http_cache";
```

request_status_query
----------
**syntax:** *request_status_query &lt;on&gt;*

**default:** *off*

**context:** *location*

开启统计查询模块。开启后，就可以通过该location访问到统计信息。
统计信息查询模块有三个可选的参数：
* clean: 为true时表示，查询统计信息，并将本次查询的统计项清零。
* fmt: 可选值为：html,json,text，分别以html,json,text格式显示。默认格式为text。html可以直接以浏览器查看，json格式方便你使用python等脚本语言解析结果。text格式方便在命令下查询，及通过awk等shell命令进行处理。
* stat_name：要查询的统计名，该统计名称必须是在request_status指令的第一个参数指定的stat-name中的一个。 当不指定该参数时，表示查询所有统计。


最小示例：
```nginx
location /status {
	request_status_query on;
}
```
统计查询请见[统计查询](#统计查询)一节

Statistical query
--------------
&nbsp;&nbsp;配置好request_status_query后，就可以通过相应的uri访问统计结果，比如上节配置中，访问
http://192.168.1.201/status 就可以显示相关统计信息。**192.168.1.201是我的主机**

查询结果中一般有如下几个字段：
* key, request_status中定义的key
* request, 请求次数
* recv, 接收字节数
* sent, 发送字节数
* avg_time, 请求平均时间(毫秒)
* status, http响应码, 其中499是后端超时了。

&nbsp;&nbsp;**以下所有查询结果都是在运行[简单脚本测试](#简单脚本测试)一节中的测试脚本后产生的。**

#### 文本格式
http://192.168.1.201/status
```bash
# Optional parameters:
# clean=true, query statistics and set the all query data to zero in the share memory.
# fmt=[html|json|text], The default is text.
# stat_name=[ statby_host| statby_uri| statby_arg| statby_uriarg], The default is all.
key	request	recv	sent	avg_time	status
localhost	5	0	0	0	 400:5
127.0.0.1	80	14689	15855	0	 200:60, 404:20
cache:miss	20	3560	3800	0	 200:20
cache:hit	20	3540	3760	0	 200:20
header_in:beijing	20	3740	3580	0	 200:20
header_in:shengzheng	20	3800	3660	0	 200:20
header_in:shanghai	20	3760	3600	0	 200:20
/byuriarg?mobile_cli	20	3640	3840	0	 200:20
/byuriarg?pc_cli	20	3560	3760	0	 200:20
/byuriarg?partner	20	3580	3780	0	 200:20
clitype:android	40	7400	10280	2	 200:20, 404:20
clitype:ios	20	3580	3760	5	 200:20
clitype:pc	20	3560	3740	5	 200:20
uri:/byuri/12964	1	169	186	0	 200:1
uri:/byuri/25507	1	169	186	0	 200:1
uri:/byuri/10608	1	169	186	0	 200:1
...
```
#### html格式
http://192.168.1.201/status?fmt=html
![查询界面](view_html.png)

#### json格式
http://192.168.1.201/status?fmt=json
```json
{"Optional parameters":{
"clean":"clean=true, query statistics and set the all query data to zero in the share memory.",
"fmt":"fmt=[html|json|text], The default is text.",
"stat_name":[" statby_host| statby_uri| statby_arg| statby_uriarg"]
},
"request-status":{
"localhost":{"request":8,"recv":0,"sent":0,"avg_time":0,"status":{"400":8}},
"192.168.1.201":{"request":3,"recv":927,"sent":966,"avg_time":0,"status":{"404":3}},
"127.0.0.1":{"request":80,"recv":14689,"sent":15855,"avg_time":0,"status":{"200":60,"404":20}},
"cache:miss":{"request":20,"recv":3560,"sent":3800,"avg_time":0,"status":{"200":20}},
"cache:hit":{"request":20,"recv":3540,"sent":3760,"avg_time":0,"status":{"200":20}},
"header_in:beijing":{"request":20,"recv":3740,"sent":3580,"avg_time":0,"status":{"200":20}},
"header_in:shengzheng":{"request":20,"recv":3800,"sent":3660,"avg_time":0,"status":{"200":20}},
"header_in:shanghai":{"request":20,"recv":3760,"sent":3600,"avg_time":0,"status":{"200":20}},
"/byuriarg?mobile_cli":{"request":20,"recv":3640,"sent":3840,"avg_time":0,"status":{"200":20}},
"/byuriarg?pc_cli":{"request":20,"recv":3560,"sent":3760,"avg_time":0,"status":{"200":20}},
"/byuriarg?partner":{"request":20,"recv":3580,"sent":3780,"avg_time":0,"status":{"200":20}},
"clitype:android":{"request":40,"recv":7400,"sent":10280,"avg_time":2,"status":{"200":20,"404":20}},
"clitype:ios":{"request":20,"recv":3580,"sent":3760,"avg_time":5,"status":{"200":20}},
"clitype:pc":{"request":20,"recv":3560,"sent":3740,"avg_time":5,"status":{"200":20}},
"uri:/byuri/12964":{"request":1,"recv":169,"sent":186,"avg_time":0,"status":{"200":1}},
"uri:/byuri/25507":{"request":1,"recv":169,"sent":186,"avg_time":0,"status":{"200":1}},
"uri:/byuri/10608":{"request":1,"recv":169,"sent":186,"avg_time":0,"status":{"200":1}},
"uri:/byuri/9349":{"request":1,"recv":168,"sent":185,"avg_time":0,"status":{"200":1}},
"uri:/byuri/25684":{"request":1,"recv":169,"sent":186,"avg_time":0,"status":{"200":1}}
}
}
```
#### 查询并且将查询项清零
http://192.168.1.201/status?clean=true
使用clean=true参数后，本次查询结果依然正常显示，只是所有结果项会被清零。

#### 查询某一个统计项
* http://192.168.1.201/status?stat_name=statby_headerin

```text
key	request	recv	sent	avg_time	status
header_in:beijing	20	3740	3580	0	 200:20
header_in:shengzheng	20	3800	3660	0	 200:20
header_in:shanghai	20	3760	3600	0	 200:20
```
* http://192.168.1.201/status?stat_name=statby_uri

```text
key	request	recv	sent	avg_time	status
uri:/byuri/9510	1	168	185	0	 200:1
uri:/byuri/31706	1	169	186	0	 200:1
uri:/byuri/22372	1	169	186	0	 200:1
uri:/byuri/28608	1	169	186	0	 200:1
uri:/byuri/28809	1	169	186	0	 200:1
uri:/byuri/2427	1	168	185	0	 200:1
uri:/byuri/700	1	167	184	0	 200:1
uri:/byuri/1096	1	168	185	0	 200:1
uri:/byuri/26854	1	169	186	0	 200:1
uri:/byuri/27665	1	169	186	0	 200:1
uri:/byuri/12983	1	169	186	0	 200:1
uri:/byuri/26040	1	169	186	0	 200:1
```

#作用域说明
>request_status指令作用域是:
`NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF`,也就是说request_status指令在http,server,location,if等节点下都可以出现。但是由于本模块是一个在NGX_HTTP_LOG_PHASE阶段的插件，一次请求只会有一个配置项是有效。当在不同的层次中出现时，只有最内层的会启作用。当然，如果同一层里面有多个request_status指令，多个都会有效果。比如：
```
http {
    #...
	request_status byhost "$host";	
	server {
		listen       80;
		location /login {
			echo "login";
		}
		location /login_new {
			request_status byarg "$arg_client_type";		
			echo "login_new $args";
		}
	}
}
```
##### 使用上面的配置，如果作如下三次请求：
```shell
curl http://127.0.0.1:80/login
curl http://127.0.0.1:80/login_new?client_type=pc
curl http://127.0.0.1:80/login_new?client_type=android
```
##### 统计结果会是：
```
key         request recv    sent    avg_time
android     1       187     210     0
pc          1       182     205     0
127.0.0.1   1       163     185     0
```
/login_new下面的请求，由于已经有一个名为byarg的统计，不会重复统计到byhost里面。有时候这可能不是你想要的结果。如果你想/login_new也统计进byhost里面，可以在/login_new下面再加个request_status指令后的新的配置：
```
http {
    #...
	request_status byhost "$host";	
	server {
		listen       80;
		location /login {
			echo "login";
		}
		location /login_new {
			request_status byarg "$arg_client_type";
			request_status byhost "$host";	
			echo "login_new $args";
		}
	}
}
```
##### 重新测试后，结果如下：
```
key         request recv    sent    avg_time
127.0.0.1   3       532     600     0
android     1       187     210     0
pc          1       182     205     0
```



#### 简单脚本测试

本测试对应配置在Synopsis一节中。
测试依赖于curl命令，请确认你的系统已经安装curl命令行。
源代码目录下的[test.sh](#test.sh)
```bash
for ((i=0;i<20;i++));do
curl http://127.0.0.1:81/$RANDOM
curl http://127.0.0.1:81/404/$RANDOM
curl http://127.0.0.1:80/byuri/$RANDOM
curl http://127.0.0.1:80/byarg?client_type=pc
curl http://127.0.0.1:80/byarg?client_type=ios
curl http://127.0.0.1:80/byarg?client_type=android
curl http://127.0.0.1:80/byarg/404?client_type=android
curl http://127.0.0.1:80/byuriarg?from=partner
curl http://127.0.0.1:80/byuriarg?from=pc_cli
curl http://127.0.0.1:80/byuriarg?from=mobile_cli
curl http://127.0.0.1:80/byhttpheaderin -H"city: shanghai"
curl http://127.0.0.1:80/byhttpheaderin -H"city: shengzheng"
curl http://127.0.0.1:80/byhttpheaderin -H"city: beijing"
curl http://127.0.0.1:80/byhttpheaderout/hit
curl http://127.0.0.1:80/byhttpheaderout/miss
done;

```

#### 相关模块
&nbsp;&nbsp;&nbsp;&nbsp;本模块把所有统计信息都存储在内存中，需要用户自己获取相关信息，再存储，汇总。作者的另外一个项目[ngx_req_stat](https://github.com/jie123108/ngx_req_stat)也是一个请求统计模块，但它功能更加强大，不光key是可以自定义的，连统计的值也是可以自定义的。并且统计信息存储在mongodb中。项目地址：(https://github.com/jie123108/ngx_req_stat)

联系作者:
=====
jie123108@163.com

