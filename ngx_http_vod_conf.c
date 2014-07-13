#include "ngx_http_vod_conf.h"
#include "ngx_child_http_request.h"
#include "ngx_http_vod_module.h"
#include "vod/common.h"

static void *
ngx_http_vod_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_vod_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_vod_loc_conf_t));
    if (conf == NULL) 
	{
        return NGX_CONF_ERROR;
    }

	init_upstream_conf(&conf->upstream);
	init_upstream_conf(&conf->fallback_upstream);
	conf->request_handler = NGX_CONF_UNSET_PTR;
	conf->segment_duration = NGX_CONF_UNSET_UINT;
	conf->initial_read_size = NGX_CONF_UNSET_SIZE;
	conf->max_moov_size = NGX_CONF_UNSET_SIZE;
	conf->cache_buffer_size = NGX_CONF_UNSET_SIZE;
	conf->max_path_length = NGX_CONF_UNSET_SIZE;

    return conf;
}

static char *
ngx_http_vod_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_vod_loc_conf_t *prev = parent;
    ngx_http_vod_loc_conf_t *conf = child;
	u_char* p;
	char* err;

	ngx_conf_merge_ptr_value(conf->request_handler, prev->request_handler, local_request_handler);

	ngx_conf_merge_uint_value(conf->segment_duration, prev->segment_duration, 10000);
	ngx_conf_merge_str_value(conf->secret_key, prev->secret_key, "");

	ngx_conf_merge_size_value(conf->initial_read_size, prev->initial_read_size, 4096);
	ngx_conf_merge_size_value(conf->max_moov_size, prev->max_moov_size, 1 * 1024 * 1024);
	ngx_conf_merge_size_value(conf->cache_buffer_size, prev->cache_buffer_size, 64 * 1024);

	err = merge_upstream_conf(cf, &conf->upstream, &prev->upstream);
	if (err != NGX_CONF_OK)
	{
		return err;
	}
	ngx_conf_merge_str_value(conf->upstream_host_header, prev->upstream_host_header, "");
	ngx_conf_merge_str_value(conf->upstream_extra_args, prev->upstream_extra_args, "");

	ngx_conf_merge_str_value(conf->path_response_prefix, prev->path_response_prefix, "<?xml version=\"1.0\" encoding=\"utf-8\"?><xml><result>");
	ngx_conf_merge_str_value(conf->path_response_postfix, prev->path_response_postfix, "</result></xml>");
	ngx_conf_merge_size_value(conf->max_path_length, prev->max_path_length, 1024);

	err = merge_upstream_conf(cf, &conf->fallback_upstream, &prev->fallback_upstream);
	if (err != NGX_CONF_OK)
	{
		return err;
	}
	ngx_conf_merge_str_value(conf->proxy_header_name, prev->proxy_header_name, "X-Kaltura-Proxy");
	ngx_conf_merge_str_value(conf->proxy_header_value, prev->proxy_header_value, "dumpApiRequest");

	ngx_conf_merge_str_value(conf->clip_to_param_name, prev->clip_to_param_name, "clipTo");
	ngx_conf_merge_str_value(conf->clip_from_param_name, prev->clip_from_param_name, "clipFrom");
	ngx_conf_merge_str_value(conf->index_file_name_prefix, prev->index_file_name_prefix, "index");
	ngx_conf_merge_str_value(conf->iframes_file_name_prefix, prev->iframes_file_name_prefix, "iframes");
	ngx_conf_merge_str_value(conf->m3u8_config.segment_file_name_prefix, prev->m3u8_config.segment_file_name_prefix, "seg-");
	ngx_conf_merge_str_value(conf->encryption_key_file_name, prev->encryption_key_file_name, "encryption.key");

	if (conf->encryption_key_file_name.len > MAX_ENCRYPTION_KEY_FILE_NAME_LEN)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"encryption_key_file_name\" should not be more than %d characters", MAX_ENCRYPTION_KEY_FILE_NAME_LEN);
		return NGX_CONF_ERROR;
	}

	// validate vod_upstream / vod_upstream_host_header used when needed
	if (conf->request_handler == remote_request_handler || conf->request_handler == mapped_request_handler)
	{
		if (conf->upstream.upstream == NULL)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"\"vod_upstream\" is mandatory for remote/mapped modes");
			return NGX_CONF_ERROR;
		}

		if (conf->upstream_host_header.len == 0)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"\"vod_upstream_host_header\" is mandatory for remote/mapped modes");
			return NGX_CONF_ERROR;
		}
	}
	else
	{
		if (conf->upstream.upstream != NULL)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"\"vod_upstream\" does not apply to local mode");
			return NGX_CONF_ERROR;
		}

		if (conf->upstream_host_header.len != 0)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"\"vod_upstream_host_header\" does not apply to local mode");
			return NGX_CONF_ERROR;
		}
	}
	
	if (conf->request_handler == remote_request_handler)
	{
		if (conf->upstream.upstream != NULL)
		{
			ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
				"\"vod_fallback_upstream\" does not apply to remote mode");
			return NGX_CONF_ERROR;
		}
	}

	if (conf->segment_duration <= 0)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"segment_duration\" must be positive");
		return NGX_CONF_ERROR;
	}

	init_m3u8_config(
		&conf->m3u8_config,
		conf->segment_duration,
		conf->secret_key.len > 0 ? (char*)conf->encryption_key_file_name.data : NULL);

	// combine the proxy header name and value to a single line
	conf->proxy_header.len = conf->proxy_header_name.len + sizeof(": ") - 1 + conf->proxy_header_value.len + sizeof(CRLF);
	conf->proxy_header.data = ngx_alloc(conf->proxy_header.len, cf->log);
	if (conf->proxy_header.data == NULL)
	{
		return NGX_CONF_ERROR;
	}
	p = conf->proxy_header.data;
	p = ngx_copy(p, conf->proxy_header_name.data, conf->proxy_header_name.len);
	*p++ = ':';		*p++ = ' ';
	p = ngx_copy(p, conf->proxy_header_value.data, conf->proxy_header_value.len);
	*p++ = '\r';		*p++ = '\n';
	*p = '\0';

    return NGX_CONF_OK;
}

static char *
ngx_http_vod_mode_command(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_vod_loc_conf_t    *mgcf = conf;
	ngx_str_t                       *value;

	value = cf->args->elts;

	if (ngx_strcasecmp(value[1].data, (u_char *) "local") == 0) 
	{
		mgcf->request_handler = local_request_handler;
	}
	else if (ngx_strcasecmp(value[1].data, (u_char *) "remote") == 0) 
	{
		mgcf->request_handler = remote_request_handler;
	}
	else if (ngx_strcasecmp(value[1].data, (u_char *) "mapped") == 0) 
	{
		mgcf->request_handler = mapped_request_handler;
	}
	else 
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"invalid value \"%s\" in \"%s\" directive, "
			"it must be \"local\", \"remote\" or \"mapped\"",
			value[1].data, cmd->name.data);
		return NGX_CONF_ERROR;
	}

	return NGX_CONF_OK;
}

static char *
ngx_http_upstream_command(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_upstream_conf_t *upstream_conf = (ngx_http_upstream_conf_t*)((u_char*)conf + cmd->offset);
    ngx_str_t                       *value;
    ngx_url_t                        u;

	if (upstream_conf->upstream)
	{
        return "is duplicate";
    }

    value = cf->args->elts;

    ngx_memzero(&u, sizeof(ngx_url_t));

    u.url = value[1];
    u.no_resolve = 1;
    u.default_port = 80;

	upstream_conf->upstream = ngx_http_upstream_add(cf, &u, 0);
	if (upstream_conf->upstream == NULL)
	{
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static char *
ngx_http_vod(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_core_loc_conf_t *clcf;

	clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
	clcf->handler = ngx_http_vod_handler; /* handler to process the 'hello' directive */

	return NGX_CONF_OK;
}

ngx_command_t ngx_http_vod_commands[] = {

	// basic parameters
	{ ngx_string("vod"),
	NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
	ngx_http_vod,
	0,
	0,
	NULL },

	{ ngx_string("vod_mode"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_vod_mode_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	0,
	NULL },

	// hls output generation parameters
	{ ngx_string("vod_segment_duration"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, segment_duration),
	NULL },

	{ ngx_string("vod_secret_key"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, secret_key),
	NULL },

	// mp4 reading parameters
	{ ngx_string("vod_initial_read_size"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, initial_read_size),
	NULL },

	{ ngx_string("vod_max_moov_size"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, max_moov_size),
	NULL },

	{ ngx_string("vod_cache_buffer_size"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, cache_buffer_size),
	NULL },

	// upstream parameters - only for mapped/remote modes
	{ ngx_string("vod_upstream"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_upstream_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, upstream),
	NULL },

	{ ngx_string("vod_upstream_host_header"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, upstream_host_header),
	NULL },

	{ ngx_string("vod_upstream_extra_args"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, upstream_extra_args),
	NULL },

	{ ngx_string("vod_connect_timeout"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_msec_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, upstream.connect_timeout),
	NULL },

	{ ngx_string("vod_send_timeout"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_msec_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, upstream.send_timeout),
	NULL },

	{ ngx_string("vod_read_timeout"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_msec_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, upstream.read_timeout),
	NULL },

	// path request parameters - mapped mode only
	{ ngx_string("vod_path_response_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, path_response_prefix),
	NULL },

	{ ngx_string("vod_path_response_postfix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, path_response_postfix),
	NULL },

	{ ngx_string("vod_max_path_length"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_size_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, max_path_length),
	NULL },

	// fallback upstream - only for local/mapped modes
	{ ngx_string("vod_fallback_upstream"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_http_upstream_command,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, fallback_upstream),
	NULL },

	{ ngx_string("vod_fallback_connect_timeout"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_msec_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, fallback_upstream.connect_timeout),
	NULL },

	{ ngx_string("vod_fallback_send_timeout"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_msec_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, fallback_upstream.send_timeout),
	NULL },

	{ ngx_string("vod_fallback_read_timeout"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_msec_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, fallback_upstream.read_timeout),
	NULL },

	{ ngx_string("vod_proxy_header_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, proxy_header_name),
	NULL },

	{ ngx_string("vod_proxy_header_value"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, proxy_header_value),
	NULL },

	// request format settings
	{ ngx_string("vod_clip_to_param_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, clip_to_param_name),
	NULL },

	{ ngx_string("vod_clip_from_param_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, clip_from_param_name),
	NULL },

	{ ngx_string("vod_index_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, index_file_name_prefix),
	NULL },

	{ ngx_string("vod_iframes_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, iframes_file_name_prefix),
	NULL },

	{ ngx_string("vod_segment_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, m3u8_config.segment_file_name_prefix),
	NULL },

	{ ngx_string("vod_encryption_key_file_name"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	offsetof(ngx_http_vod_loc_conf_t, encryption_key_file_name),
	NULL },

	ngx_null_command
};

ngx_http_module_t  ngx_http_vod_module_ctx = {
	NULL,                               /* preconfiguration */
	NULL,                               /* postconfiguration */

	NULL,                               /* create main configuration */
	NULL,                               /* init main configuration */

	NULL,                               /* create server configuration */
	NULL,                               /* merge server configuration */

	ngx_http_vod_create_loc_conf,       /* create location configuration */
	ngx_http_vod_merge_loc_conf         /* merge location configuration */
};