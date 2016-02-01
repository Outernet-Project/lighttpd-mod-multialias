#include "base.h"
#include "log.h"
#include "buffer.h"

#include "plugin.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

/* plugin config for all request/connections */
typedef struct {
	array *alias;
} plugin_config;

typedef struct {
	PLUGIN_DATA;

	plugin_config **config_storage;

	plugin_config conf;
} plugin_data;

/* insert key / value pair into array, no dupe check performed */
int array_insert(array *a, data_unset *str) {
	int ndx = -1;
	int pos = 0;
	size_t j;

	/* generate unique index if neccesary */
	if (buffer_is_empty(str->key) || str->is_index_key) {
		buffer_copy_int(str->key, a->unique_ndx++);
		str->is_index_key = 1;
	}

	/* insert */

	if (a->used+1 > INT_MAX) {
		/* we can't handle more then INT_MAX entries: see array_get_index() */
		return -1;
	}

	if (a->size == 0) {
		a->size   = 16;
		a->data   = malloc(sizeof(*a->data)     * a->size);
		a->sorted = malloc(sizeof(*a->sorted)   * a->size);
		force_assert(a->data);
		force_assert(a->sorted);
		for (j = a->used; j < a->size; j++) a->data[j] = NULL;
	} else if (a->size == a->used) {
		a->size  += 16;
		a->data   = realloc(a->data,   sizeof(*a->data)   * a->size);
		a->sorted = realloc(a->sorted, sizeof(*a->sorted) * a->size);
		force_assert(a->data);
		force_assert(a->sorted);
		for (j = a->used; j < a->size; j++) a->data[j] = NULL;
	}

	ndx = (int) a->used;

	/* make sure there is nothing here */
	if (a->data[ndx]) a->data[ndx]->free(a->data[ndx]);

	a->data[a->used++] = str;

	if (pos != ndx &&
	    ((pos < 0) ||
	     buffer_caseless_compare(CONST_BUF_LEN(str->key), CONST_BUF_LEN(a->data[a->sorted[pos]]->key)) > 0)) {
		pos++;
	}

	/* move everything on step to the right */
	if (pos != ndx) {
		memmove(a->sorted + (pos + 1), a->sorted + (pos), (ndx - pos) * sizeof(*a->sorted));
	}

	/* insert */
	a->sorted[pos] = ndx;

	if (a->next_power_of_2 == (size_t)ndx) a->next_power_of_2 <<= 1;

	return 0;
}

/* add specific key / value pair to the first free slot of the array (no dupe check) */
void array_add_key_value(array *a, const char *key, size_t key_len, const char *value, size_t val_len) {
	data_string *ds_dst;

	if (NULL == (ds_dst = (data_string *)array_get_unused_element(a, TYPE_STRING))) {
		ds_dst = data_string_init();
	}

	buffer_copy_string_len(ds_dst->key, key, key_len);
	buffer_copy_string_len(ds_dst->value, value, val_len);
	array_insert(a, (data_unset *)ds_dst);
}

/* init the plugin data */
INIT_FUNC(mod_multialias_init) {
	plugin_data *p;

	p = calloc(1, sizeof(*p));



	return p;
}

/* detroy the plugin data */
FREE_FUNC(mod_multialias_free) {
	plugin_data *p = p_d;

	if (!p) return HANDLER_GO_ON;

	if (p->config_storage) {
		size_t i;

		for (i = 0; i < srv->config_context->used; i++) {
			plugin_config *s = p->config_storage[i];

			if (NULL == s) continue;

			array_free(s->alias);

			free(s);
		}
		free(p->config_storage);
	}

	free(p);

	return HANDLER_GO_ON;
}

/* handle plugin config and check values */

SETDEFAULTS_FUNC(mod_multialias_set_defaults) {
	plugin_data *p = p_d;
	size_t i = 0;

	config_values_t cv[] = {
		{ "alias.url",                  NULL, T_CONFIG_LOCAL, T_CONFIG_SCOPE_CONNECTION },       /* 0 */
		{ NULL,                         NULL, T_CONFIG_UNSET,  T_CONFIG_SCOPE_UNSET }
	};

	if (!p) return HANDLER_ERROR;

	p->config_storage = calloc(1, srv->config_context->used * sizeof(plugin_config *));

	for (i = 0; i < srv->config_context->used; i++) {
		data_config const* config = (data_config const*)srv->config_context->data[i];
		plugin_config *s;
		size_t l;
		data_unset *du;
		data_array *da;

		s = calloc(1, sizeof(plugin_config));
		s->alias = array_init();
		cv[0].destination = s->alias;

		p->config_storage[i] = s;

		if (0 != config_insert_values_global(srv, config->value, cv, i == 0 ? T_CONFIG_SCOPE_SERVER : T_CONFIG_SCOPE_CONNECTION)) {
			return HANDLER_ERROR;
		}

		if (NULL == (du = array_get_element(config->value, "alias.url"))) {
			/* no alias.url defined */
			continue;
		}

		if (du->type != TYPE_ARRAY) {
			log_error_write(srv, __FILE__, __LINE__, "sss",
					"unexpected type for key: ", "alias.url", "array of strings");

			return HANDLER_ERROR;
		}

		da = (data_array *)du;

		for (l = 0; l < da->value->used; l++) {
			if (da->value->data[l]->type == TYPE_STRING) {
				data_string *pair = (data_string *)(da->value->data[l]);
				array_add_key_value(s->alias,
									pair->key->ptr,
									buffer_string_length(pair->key),
									pair->value->ptr,
									buffer_string_length(pair->value));
			} else if (da->value->data[l]->type == TYPE_ARRAY) {
				data_array *paths = (data_array *)da->value->data[l];
				for (size_t m = 0; m < paths->value->used; m++) {
					data_string *pair = (data_string *)(paths->value->data[m]);
					array_add_key_value(s->alias,
										da->value->data[l]->key->ptr,
										buffer_string_length(da->value->data[l]->key),
										pair->value->ptr,
										buffer_string_length(pair->value));
				}
			} else {
				log_error_write(srv, __FILE__, __LINE__, "sssbs",
						"unexpected type for key: ",
						"alias.url",
						"[", da->value->data[l]->key, "](string)");

				return HANDLER_ERROR;
			}
		}

		if (s->alias->used >= 2) {
			const array *a = s->alias;
			size_t j, k;

			for (j = 0; j < a->used; j ++) {
				const buffer *prefix = a->data[a->sorted[j]]->key;
				for (k = j + 1; k < a->used; k ++) {
					const buffer *key = a->data[a->sorted[k]]->key;
					if (buffer_string_length(key) < buffer_string_length(prefix)) {
						break;
					}
					if (memcmp(key->ptr, prefix->ptr, buffer_string_length(prefix)) != 0) {
						break;
					}
					/* ok, they have same prefix. check position */
					if (a->sorted[j] < a->sorted[k]) {
						log_error_write(srv, __FILE__, __LINE__, "SBSBS",
							"url.alias: `", key, "' will never match as `", prefix, "' matched first");
						return HANDLER_ERROR;
					}
				}
			}
		}
	}

	return HANDLER_GO_ON;
}

#define PATCH(x) \
	p->conf.x = s->x;
static int mod_multialias_patch_connection(server *srv, connection *con, plugin_data *p) {
	size_t i, j;
	plugin_config *s = p->config_storage[0];

	PATCH(alias);

	/* skip the first, the global context */
	for (i = 1; i < srv->config_context->used; i++) {
		data_config *dc = (data_config *)srv->config_context->data[i];
		s = p->config_storage[i];

		/* condition didn't match */
		if (!config_check_cond(srv, con, dc)) continue;

		/* merge config */
		for (j = 0; j < dc->value->used; j++) {
			data_unset *du = dc->value->data[j];

			if (buffer_is_equal_string(du->key, CONST_STR_LEN("alias.url"))) {
				PATCH(alias);
			}
		}
	}

	return 0;
}
#undef PATCH

PHYSICALPATH_FUNC(mod_multialias_physical_handler) {
	plugin_data *p = p_d;
	int uri_len, basedir_len;
	char *uri_ptr;
	size_t k;

	if (buffer_is_empty(con->physical.path)) return HANDLER_GO_ON;

	mod_multialias_patch_connection(srv, con, p);

	/* not to include the tailing slash */
	basedir_len = buffer_string_length(con->physical.basedir);
	if ('/' == con->physical.basedir->ptr[basedir_len-1]) --basedir_len;
	uri_len = buffer_string_length(con->physical.path) - basedir_len;
	uri_ptr = con->physical.path->ptr + basedir_len;

	for (k = 0; k < p->conf.alias->used; k++) {
		data_string *ds = (data_string *)p->conf.alias->data[k];
		int alias_len = buffer_string_length(ds->key);

		if (alias_len > uri_len) continue;
		if (buffer_is_empty(ds->key)) continue;

		if (0 == (con->conf.force_lowercase_filenames ?
					strncasecmp(uri_ptr, ds->key->ptr, alias_len) :
					strncmp(uri_ptr, ds->key->ptr, alias_len))) {
			/* matched */

			// check if request path exists
			struct stat info;
			buffer *target;
			target = buffer_init();
			buffer_copy_buffer(target, ds->value);
			buffer_append_string(target, uri_ptr + alias_len);
			buffer_urldecode_path(target);
			int exists = (0 == stat(target->ptr, &info));
			buffer_free(target);
			if (exists) {
				buffer_copy_buffer(con->physical.basedir, ds->value);
				buffer_copy_buffer(srv->tmp_buf, ds->value);
				buffer_append_string(srv->tmp_buf, uri_ptr + alias_len);
				buffer_copy_buffer(con->physical.path, srv->tmp_buf);
				return HANDLER_GO_ON;
			}
		}
	}

	/* not found */
	return HANDLER_GO_ON;
}

/* this function is called at dlopen() time and inits the callbacks */

int mod_multialias_plugin_init(plugin *p);
int mod_multialias_plugin_init(plugin *p) {
	p->version     = LIGHTTPD_VERSION_ID;

	p->name        = buffer_init_string("multialias");

	p->init           = mod_multialias_init;
	p->handle_physical= mod_multialias_physical_handler;
	p->set_defaults   = mod_multialias_set_defaults;
	p->cleanup        = mod_multialias_free;

	p->data        = NULL;

	return 0;
}
