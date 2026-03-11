#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
  resqlite v0.0.1

  2026 @ mtatton - donationware

  Important limitation that remains:
    - Statements are still captured at SQLITE_TRACE_STMT time, before execution.
      If a write statement fails inside an explicit transaction but the overall
      transaction remains open, the queued SQL may still poison COMMIT later.
      Fixing that robustly would require a larger design change.
*/

typedef struct ResqliteSqlNode {
  char *sql;
  struct ResqliteSqlNode *next;
} ResqliteSqlNode;

typedef struct ResqliteReplica {
  sqlite3 *db;
  char *path;
  struct ResqliteReplica *next;
} ResqliteReplica;

typedef struct ResqliteState {
  sqlite3 *primary;
  ResqliteReplica *replicas;
  ResqliteSqlNode *head;
  ResqliteSqlNode *tail;
  int enabled;
  int installed;
  int applying;
  int in_commit;
  int unsupported_txn_seen;
  char *last_error;
} ResqliteState;

static void resqlite_set_error(ResqliteState *st, const char *fmt, ...){
  va_list ap;
  if(!st) return;
  if(st->last_error){
    sqlite3_free(st->last_error);
    st->last_error = NULL;
  }
  va_start(ap, fmt);
  st->last_error = sqlite3_vmprintf(fmt, ap);
  va_end(ap);
}

static void resqlite_clear_error(ResqliteState *st){
  if(st && st->last_error){
    sqlite3_free(st->last_error);
    st->last_error = NULL;
  }
}

static void resqlite_free_queue(ResqliteState *st){
  ResqliteSqlNode *n, *next;
  if(!st) return;
  n = st->head;
  while(n){
    next = n->next;
    sqlite3_free(n->sql);
    sqlite3_free(n);
    n = next;
  }
  st->head = NULL;
  st->tail = NULL;
}

static void resqlite_free_replicas(ResqliteState *st){
  ResqliteReplica *r, *next;
  if(!st) return;
  r = st->replicas;
  while(r){
    next = r->next;
    if(r->db) sqlite3_close(r->db);
    sqlite3_free(r->path);
    sqlite3_free(r);
    r = next;
  }
  st->replicas = NULL;
}

static int resqlite_queue_push(ResqliteState *st, const char *sql){
  ResqliteSqlNode *n;
  size_t len;
  if(!st || !sql) return SQLITE_MISUSE;

  len = strlen(sql);
  n = (ResqliteSqlNode *)sqlite3_malloc((int)sizeof(*n));
  if(!n) return SQLITE_NOMEM;
  memset(n, 0, sizeof(*n));

  n->sql = (char *)sqlite3_malloc64((sqlite3_uint64)len + 1);
  if(!n->sql){
    sqlite3_free(n);
    return SQLITE_NOMEM;
  }

  memcpy(n->sql, sql, len + 1);
  if(st->tail){
    st->tail->next = n;
    st->tail = n;
  }else{
    st->head = st->tail = n;
  }
  return SQLITE_OK;
}

static int resqlite_replica_count(ResqliteState *st){
  int count = 0;
  ResqliteReplica *r;
  if(!st) return 0;
  for(r = st->replicas; r; r = r->next) count++;
  return count;
}

static int resqlite_queue_count(ResqliteState *st){
  int count = 0;
  ResqliteSqlNode *n;
  if(!st) return 0;
  for(n = st->head; n; n = n->next) count++;
  return count;
}

static const char *resqlite_skip_space_only(const char *z){
  while(z && *z && isspace((unsigned char)*z)) z++;
  return z;
}

static const char *resqlite_skip_space_and_comments(const char *z){
  while(z && *z){
    while(*z && isspace((unsigned char)*z)) z++;
    if(z[0] == '-' && z[1] == '-'){
      z += 2;
      while(*z && *z != '\n') z++;
      continue;
    }
    if(z[0] == '/' && z[1] == '*'){
      z += 2;
      while(*z && !(z[0] == '*' && z[1] == '/')) z++;
      if(*z) z += 2;
      continue;
    }
    break;
  }
  return z;
}

static int resqlite_starts_with_keyword(const char *sql, const char *kw){
  size_t i;
  sql = resqlite_skip_space_and_comments(sql);
  if(!sql) return 0;
  for(i = 0; kw[i]; i++){
    if(toupper((unsigned char)sql[i]) != toupper((unsigned char)kw[i])){
      return 0;
    }
  }
  if(sql[i] == '\0') return 1;
  if(isspace((unsigned char)sql[i]) || sql[i] == ';' || sql[i] == '(') return 1;
  return 0;
}

static int resqlite_is_trigger_substatement(const char *trace_sql){
  const char *z = resqlite_skip_space_only(trace_sql);
  return z && z[0] == '-' && z[1] == '-';
}

static int resqlite_is_txn_control_sql(const char *sql){
  return resqlite_starts_with_keyword(sql, "BEGIN") ||
         resqlite_starts_with_keyword(sql, "COMMIT") ||
         resqlite_starts_with_keyword(sql, "END") ||
         resqlite_starts_with_keyword(sql, "ROLLBACK");
}

static int resqlite_is_unsupported_txn_sql(const char *sql){
  return resqlite_starts_with_keyword(sql, "SAVEPOINT") ||
         resqlite_starts_with_keyword(sql, "RELEASE") ||
         resqlite_starts_with_keyword(sql, "ROLLBACK TO");
}

static int resqlite_is_unmirrorable_connection_sql(const char *sql){
  return resqlite_starts_with_keyword(sql, "ATTACH") ||
         resqlite_starts_with_keyword(sql, "DETACH");
}

static int resqlite_exec_simple(sqlite3 *db, const char *sql, char **errmsg){
  return sqlite3_exec(db, sql, 0, 0, errmsg);
}

static int resqlite_install_hooks(ResqliteState *st);

static char *resqlite_json_quote(const char *zIn){
  sqlite3_str *s;
  const unsigned char *z;
  if(!zIn) zIn = "";

  s = sqlite3_str_new(NULL);
  if(!s) return NULL;

  sqlite3_str_appendchar(s, 1, '"');
  z = (const unsigned char *)zIn;
  while(*z){
    switch(*z){
      case '"': sqlite3_str_appendall(s, "\\\""); break;
      case '\\': sqlite3_str_appendall(s, "\\\\"); break;
      case '\b': sqlite3_str_appendall(s, "\\b"); break;
      case '\f': sqlite3_str_appendall(s, "\\f"); break;
      case '\n': sqlite3_str_appendall(s, "\\n"); break;
      case '\r': sqlite3_str_appendall(s, "\\r"); break;
      case '\t': sqlite3_str_appendall(s, "\\t"); break;
      default:
        if(*z < 0x20){
          sqlite3_str_appendf(s, "\\u%04x", (unsigned int)*z);
        }else{
          sqlite3_str_appendchar(s, 1, (char)*z);
        }
        break;
    }
    z++;
  }
  sqlite3_str_appendchar(s, 1, '"');
  return sqlite3_str_finish(s);
}

static int resqlite_replicate_commit(ResqliteState *st){
  ResqliteReplica *r;
  ResqliteSqlNode *n;
  char *errmsg = NULL;
  int rc;
  int begun = 0;

  if(!st) return SQLITE_MISUSE;
  if(!st->head) return SQLITE_OK;
  if(!st->replicas) return SQLITE_OK;

  st->applying = 1;

  for(r = st->replicas; r; r = r->next){
    rc = resqlite_exec_simple(r->db, "BEGIN IMMEDIATE", &errmsg);
    if(rc != SQLITE_OK){
      resqlite_set_error(st, "replica begin failed for '%s': %s", r->path,
                         errmsg ? errmsg : sqlite3_errmsg(r->db));
      sqlite3_free(errmsg);
      errmsg = NULL;
      goto fail;
    }
  }
  begun = 1;

  for(n = st->head; n; n = n->next){
    for(r = st->replicas; r; r = r->next){
      rc = resqlite_exec_simple(r->db, n->sql, &errmsg);
      if(rc != SQLITE_OK){
        resqlite_set_error(st,
                           "replica apply failed for '%s' on SQL [%s]: %s",
                           r->path, n->sql,
                           errmsg ? errmsg : sqlite3_errmsg(r->db));
        sqlite3_free(errmsg);
        errmsg = NULL;
        goto fail;
      }
    }
  }

  for(r = st->replicas; r; r = r->next){
    rc = resqlite_exec_simple(r->db, "COMMIT", &errmsg);
    if(rc != SQLITE_OK){
      resqlite_set_error(st, "replica commit failed for '%s': %s", r->path,
                         errmsg ? errmsg : sqlite3_errmsg(r->db));
      sqlite3_free(errmsg);
      errmsg = NULL;
      goto fail;
    }
  }

  st->applying = 0;
  return SQLITE_OK;

fail:
  if(begun){
    for(r = st->replicas; r; r = r->next){
      sqlite3_exec(r->db, "ROLLBACK", 0, 0, 0);
    }
  }
  st->applying = 0;
  return SQLITE_ERROR;
}

static int resqlite_commit_hook_cb(void *ctx){
  ResqliteState *st = (ResqliteState *)ctx;
  int rc;

  if(!st || !st->enabled) return 0;
  if(st->applying) return 0;

  if(st->unsupported_txn_seen){
    if(!st->last_error){
      resqlite_set_error(st,
        "transaction uses unsupported SQL that resqlite cannot safely mirror");
    }
    return 1;
  }

  st->in_commit = 1;
  rc = resqlite_replicate_commit(st);
  st->in_commit = 0;

  if(rc != SQLITE_OK){
    return 1;
  }

  resqlite_free_queue(st);
  resqlite_clear_error(st);
  st->unsupported_txn_seen = 0;
  return 0;
}

static void resqlite_rollback_hook_cb(void *ctx){
  ResqliteState *st = (ResqliteState *)ctx;
  if(!st) return;
  resqlite_free_queue(st);
  st->unsupported_txn_seen = 0;
}

static int resqlite_trace_cb(unsigned mask, void *ctx, void *p, void *x){
  ResqliteState *st = (ResqliteState *)ctx;
  sqlite3_stmt *stmt = (sqlite3_stmt *)p;
  const char *trace_sql = (const char *)x;
  const char *captured = NULL;
  char *expanded = NULL;
  int rc;

  (void)mask;

  if(!st || !st->enabled || st->applying || st->in_commit) return 0;
  if(!stmt) return 0;

  if(trace_sql && resqlite_is_trigger_substatement(trace_sql)){
    return 0;
  }

  expanded = sqlite3_expanded_sql(stmt);
  if(expanded && expanded[0]){
    captured = expanded;
  }else{
    captured = sqlite3_sql(stmt);
  }

  if(!captured || !captured[0]){
    sqlite3_free(expanded);
    return 0;
  }

  if(resqlite_is_unsupported_txn_sql(captured)){
    st->unsupported_txn_seen = 1;
    resqlite_set_error(st,
      "saw unsupported nested transaction SQL: %s", captured);
    sqlite3_free(expanded);
    return 0;
  }

  if(resqlite_is_unmirrorable_connection_sql(captured)){
    st->unsupported_txn_seen = 1;
    resqlite_set_error(st,
      "saw connection-local SQL that cannot be mirrored safely: %s", captured);
    sqlite3_free(expanded);
    return 0;
  }

  if(resqlite_is_txn_control_sql(captured)){
    sqlite3_free(expanded);
    return 0;
  }

  if(sqlite3_stmt_readonly(stmt)){
    sqlite3_free(expanded);
    return 0;
  }

  rc = resqlite_queue_push(st, captured);
  if(rc != SQLITE_OK){
    resqlite_set_error(st, "out of memory while queueing SQL for replication");
  }

  sqlite3_free(expanded);
  return 0;
}

static int resqlite_install_hooks(ResqliteState *st){
  if(!st) return SQLITE_MISUSE;
  if(st->installed) return SQLITE_OK;

  sqlite3_trace_v2(st->primary, SQLITE_TRACE_STMT, resqlite_trace_cb, st);
  sqlite3_commit_hook(st->primary, resqlite_commit_hook_cb, st);
  sqlite3_rollback_hook(st->primary, resqlite_rollback_hook_cb, st);
  st->installed = 1;
  return SQLITE_OK;
}

static void resqlite_uninstall_hooks(ResqliteState *st){
  if(!st || !st->installed) return;
  sqlite3_trace_v2(st->primary, 0, 0, 0);
  sqlite3_commit_hook(st->primary, 0, 0);
  sqlite3_rollback_hook(st->primary, 0, 0);
  st->installed = 0;
}

static void resqlite_disable_internal(ResqliteState *st){
  if(!st) return;
  st->enabled = 0;
  st->applying = 0;
  st->in_commit = 0;
  st->unsupported_txn_seen = 0;
  resqlite_uninstall_hooks(st);
  resqlite_free_queue(st);
  resqlite_free_replicas(st);
  resqlite_clear_error(st);
}

static void resqlite_state_destructor(void *p){
  ResqliteState *st = (ResqliteState *)p;
  if(!st) return;
  resqlite_disable_internal(st);
  sqlite3_free(st);
}

static int resqlite_add_replica_internal(ResqliteState *st, const char *path){
  sqlite3 *rdb = NULL;
  ResqliteReplica *r = NULL;
  int rc;

  if(!st || !path || !path[0]) return SQLITE_MISUSE;

  rc = sqlite3_open_v2(path, &rdb,
                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,
                       NULL);
  if(rc != SQLITE_OK){
    if(rdb){
      resqlite_set_error(st, "cannot open replica '%s': %s",
                         path, sqlite3_errmsg(rdb));
      sqlite3_close(rdb);
    }else{
      resqlite_set_error(st, "cannot open replica '%s'", path);
    }
    return SQLITE_ERROR;
  }

  sqlite3_busy_timeout(rdb, 5000);
  sqlite3_extended_result_codes(rdb, 1);
  sqlite3_exec(rdb, "PRAGMA foreign_keys = ON", 0, 0, 0);

  r = (ResqliteReplica *)sqlite3_malloc((int)sizeof(*r));
  if(!r){
    sqlite3_close(rdb);
    resqlite_set_error(st, "out of memory while adding replica '%s'", path);
    return SQLITE_NOMEM;
  }
  memset(r, 0, sizeof(*r));

  r->path = sqlite3_mprintf("%s", path);
  if(!r->path){
    sqlite3_close(rdb);
    sqlite3_free(r);
    resqlite_set_error(st, "out of memory while saving replica path '%s'", path);
    return SQLITE_NOMEM;
  }

  r->db = rdb;
  r->next = st->replicas;
  st->replicas = r;
  return SQLITE_OK;
}

static void resqlite_fn_add_replica(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  ResqliteState *st = (ResqliteState *)sqlite3_user_data(ctx);
  const unsigned char *path;
  int rc;

  if(argc != 1){
    sqlite3_result_error(ctx, "resqlite_add_replica(path) expects 1 argument", -1);
    return;
  }

  path = sqlite3_value_text(argv[0]);
  if(!path || !path[0]){
    sqlite3_result_error(ctx, "replica path must be a non-empty string", -1);
    return;
  }

  rc = resqlite_add_replica_internal(st, (const char *)path);
  if(rc != SQLITE_OK){
    sqlite3_result_error(ctx, st->last_error ? st->last_error : "failed to add replica", -1);
    return;
  }

  sqlite3_result_int(ctx, resqlite_replica_count(st));
}

static void resqlite_fn_enable(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  ResqliteState *st = (ResqliteState *)sqlite3_user_data(ctx);
  int rc;
  (void)argc;
  (void)argv;

  if(!st){
    sqlite3_result_error(ctx, "internal state missing", -1);
    return;
  }

  rc = resqlite_install_hooks(st);
  if(rc != SQLITE_OK){
    sqlite3_result_error(ctx, "failed to install SQLite hooks", -1);
    return;
  }

  st->enabled = 1;
  resqlite_clear_error(st);
  sqlite3_result_text(ctx, "ok", -1, SQLITE_STATIC);
}

static void resqlite_fn_disable(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  ResqliteState *st = (ResqliteState *)sqlite3_user_data(ctx);
  (void)argc;
  (void)argv;
  resqlite_disable_internal(st);
  sqlite3_result_text(ctx, "ok", -1, SQLITE_STATIC);
}

static void resqlite_fn_clear_error(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  ResqliteState *st = (ResqliteState *)sqlite3_user_data(ctx);
  (void)argc;
  (void)argv;
  resqlite_clear_error(st);
  sqlite3_result_text(ctx, "ok", -1, SQLITE_STATIC);
}

static void resqlite_fn_status(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  ResqliteState *st = (ResqliteState *)sqlite3_user_data(ctx);
  char *quoted_error;
  char *json;
  (void)argc;
  (void)argv;

  quoted_error = resqlite_json_quote((st && st->last_error) ? st->last_error : "");
  if(!quoted_error){
    sqlite3_result_error_nomem(ctx);
    return;
  }

  json = sqlite3_mprintf(
    "{\"enabled\":%d,\"installed\":%d,\"replicas\":%d,\"queued\":%d,\"unsupported_txn_seen\":%d,\"last_error\":%s}",
    st ? st->enabled : 0,
    st ? st->installed : 0,
    st ? resqlite_replica_count(st) : 0,
    st ? resqlite_queue_count(st) : 0,
    st ? st->unsupported_txn_seen : 0,
    quoted_error
  );
  sqlite3_free(quoted_error);

  if(!json){
    sqlite3_result_error_nomem(ctx);
    return;
  }
  sqlite3_result_text(ctx, json, -1, sqlite3_free);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_resqlite_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  ResqliteState *st;
  int rc;

  SQLITE_EXTENSION_INIT2(pApi);

  st = (ResqliteState *)sqlite3_malloc((int)sizeof(*st));
  if(!st){
    if(pzErrMsg) *pzErrMsg = sqlite3_mprintf("resqlite: out of memory");
    return SQLITE_NOMEM;
  }
  memset(st, 0, sizeof(*st));
  st->primary = db;

  rc = sqlite3_create_function_v2(
    db, "resqlite_add_replica", 1,
    SQLITE_UTF8 | SQLITE_DIRECTONLY,
    st, resqlite_fn_add_replica, 0, 0, 0
  );
  if(rc != SQLITE_OK) goto fail;

  rc = sqlite3_create_function_v2(
    db, "resqlite_enable", 0,
    SQLITE_UTF8 | SQLITE_DIRECTONLY,
    st, resqlite_fn_enable, 0, 0, 0
  );
  if(rc != SQLITE_OK) goto fail;

  rc = sqlite3_create_function_v2(
    db, "resqlite_disable", 0,
    SQLITE_UTF8 | SQLITE_DIRECTONLY,
    st, resqlite_fn_disable, 0, 0, 0
  );
  if(rc != SQLITE_OK) goto fail;

  rc = sqlite3_create_function_v2(
    db, "resqlite_clear_error", 0,
    SQLITE_UTF8 | SQLITE_DIRECTONLY,
    st, resqlite_fn_clear_error, 0, 0, 0
  );
  if(rc != SQLITE_OK) goto fail;

  rc = sqlite3_create_function_v2(
    db, "resqlite_status", 0,
    SQLITE_UTF8 | SQLITE_DIRECTONLY,
    st, resqlite_fn_status, 0, 0, resqlite_state_destructor
  );
  if(rc != SQLITE_OK) goto fail;

  return SQLITE_OK;

fail:
  if(pzErrMsg && !*pzErrMsg){
    *pzErrMsg = sqlite3_mprintf("resqlite: initialization failed (%d)", rc);
  }
  resqlite_state_destructor(st);
  return rc;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_replicator_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  return sqlite3_resqlite_init(db, pzErrMsg, pApi);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_resqlitev_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  return sqlite3_resqlite_init(db, pzErrMsg, pApi);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_resqlitev001_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  return sqlite3_resqlite_init(db, pzErrMsg, pApi);
}
