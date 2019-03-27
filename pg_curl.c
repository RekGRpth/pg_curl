#include <postgres.h>
#include <fmgr.h>

#include <access/htup_details.h>
#include <access/htup.h>
#include <catalog/dependency.h>
#include <catalog/namespace.h>
#include <catalog/pg_type.h>
#include <commands/extension.h>
#include <curl/curl.h>
#include <funcapi.h>
#include <lib/stringinfo.h>
#include <limits.h>	/* INT_MAX */
#include <mb/pg_wchar.h>
#include <nodes/pg_list.h>
#include <regex.h>
#include <signal.h> /* SIGINT */
#include <stdlib.h>
#include <string.h>
#include <utils/array.h>
#include <utils/builtins.h>
#include <utils/catcache.h>
#include <utils/guc.h>
#include <utils/lsyscache.h>
#include <utils/syscache.h>
#include <utils/typcache.h>
#include <utils/varlena.h>

PG_MODULE_MAGIC;
